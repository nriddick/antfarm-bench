#include "microframe.h"
#include "afcore_internal.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>

#define MAX_BATCHES 64u
typedef struct MfTask {
    uint64_t *state[2];
    uint8_t *cells[2];
    uint32_t kind, width, height;
    uint32_t n, iterations, grain, waves;
    uint32_t parity; /* buffer holding the current state */
#ifdef MF_AUDIT
    _Atomic uint32_t *seen; /* [wave * n + item] executions this frame */
    _Atomic uint32_t *done; /* [wave] completed items this frame */
#endif
} MfTask;
struct MfFrame {
    MfTask *tasks;
    uint32_t count, index;
    int worker_only;
    _Atomic uint32_t fault;
#ifdef MF_AUDIT
    _Atomic uint64_t foreign, order;
    _Atomic uint64_t slot_items[AFC_MAX_WORKERS];
    _Atomic uint64_t first[MAX_BATCHES], last[MAX_BATCHES];
#endif
};
typedef struct Sample {
    uint64_t wall, hash;
#ifdef MF_AUDIT
    uint64_t span[MAX_BATCHES][2];
    uint32_t participants;
#endif
    int coverage;
} Sample;
static uint64_t mix(uint64_t x, uint32_t iterations) {
    for (uint32_t j=0; j<iterations; ++j) {
        x ^= x >> 30; x *= UINT64_C(0xbf58476d1ce4e5b9);
        x ^= x >> 27; x *= UINT64_C(0x94d049bb133111eb);
        x ^= x >> 31; x += UINT64_C(0x9e3779b97f4a7c15);
    }
    return x;
}
/* Mixed cost uses a stable 1/8 heavy item class (16 times the iterations). */
static void mix_range(const MfTask *t,const uint64_t *in,uint64_t *out,uint32_t round,uint32_t begin,uint32_t end) {
    for (uint32_t i=begin;i<end;++i) {
        uint32_t cost=t->iterations;
        if(t->kind==MF_KIND_M4 && (mix(i,1)&7)==0) cost*=16;
        out[i]=mix(in[i]^round,cost);
    }
}
static inline uint8_t life(uint8_t n,uint8_t alive) {return (uint8_t)((n==3)|((alive!=0)&(n==2)));}
static void gol_edge(const uint8_t *up,const uint8_t *mid,const uint8_t *dn,uint8_t *out,uint32_t x,uint32_t w) {
    uint32_t xm=x?x-1:w-1,xp=x+1==w?0:x+1;
    out[x]=life((uint8_t)(up[xm]+up[x]+up[xp]+mid[xm]+mid[xp]+dn[xm]+dn[x]+dn[xp]),mid[x]);
}
/* Toroidal B3/S23 for cells [begin, end), row by row: wrapped edge columns
 * separately, a branch-free interior loop the compiler may vectorize. Every
 * assigned cell is computed from the previous generation. */
static void gol_range(const uint8_t *restrict c,uint8_t *restrict o,uint32_t w,uint32_t h,uint32_t begin,uint32_t end) {
    uint32_t y=begin/w,x=begin-y*w;
    for (uint32_t i=begin;i<end;++y,x=0) {
        uint32_t stop=end-i<w-x?x+(end-i):w;
        i+=stop-x;
        const uint8_t *up=c+(size_t)(y?y-1:h-1)*w,*mid=c+(size_t)y*w,*dn=c+(size_t)(y+1==h?0:y+1)*w;
        uint8_t *out=o+(size_t)y*w;
        if(x==0) gol_edge(up,mid,dn,out,x++,w);
        uint32_t inner=stop<w?stop:w-1;
        for(;x<inner;++x)
            out[x]=life((uint8_t)(up[x-1]+up[x]+up[x+1]+mid[x-1]+mid[x+1]+dn[x-1]+dn[x]+dn[x+1]),mid[x]);
        if(x<stop) gol_edge(up,mid,dn,out,x,w);
    }
}
#ifdef MF_AUDIT
static void atomic_min_u64(_Atomic uint64_t *p,uint64_t v) {
    uint64_t old=atomic_load(p);
    while(v<old && !atomic_compare_exchange_weak(p,&old,v)) {}
}
static void atomic_max_u64(_Atomic uint64_t *p,uint64_t v) {
    uint64_t old=atomic_load(p);
    while(v>old && !atomic_compare_exchange_weak(p,&old,v)) {}
}
#endif
uint32_t mf_batch_count(const MfFrame *f) {return f->count;}
void mf_batch_shape(const MfFrame *f,uint32_t batch,MfBatchShape *out) {
    if(batch>=f->count) {*out=(MfBatchShape){0};return;}
    const MfTask *t=&f->tasks[batch];
    *out=(MfBatchShape){t->kind,t->n,t->waves,t->grain,t->width};
}
/* The shared opaque payload: a work count, not a promised nanosecond duration.
 * Invalid arguments mark the frame failed instead of touching memory. */
__attribute__((noinline)) void mf_execute(MfFrame *f,uint32_t batch,uint32_t wave,uint32_t begin,uint32_t end) {
    if(batch>=f->count) {atomic_store(&f->fault,1);return;}
    MfTask *t=&f->tasks[batch];
    if(wave>=t->waves||begin>end||end>t->n) {atomic_store(&f->fault,1);return;}
#ifdef MF_AUDIT
    atomic_min_u64(&f->first[batch],afc_now_ns());
    if(f->worker_only) {
        int slot=afc_registry_current_slot();
        if(slot<0) atomic_fetch_add(&f->foreign,end-begin);
        else atomic_fetch_add_explicit(&f->slot_items[slot],end-begin,memory_order_relaxed);
    }
    if(wave && atomic_load_explicit(&t->done[wave-1],memory_order_acquire)!=t->n)
        atomic_fetch_add(&f->order,1);
    for(uint32_t i=begin;i<end;++i) atomic_fetch_add(&t->seen[(size_t)wave*t->n+i],1);
#endif
    uint32_t src=(t->parity+wave)&1;
    if(t->width) gol_range(t->cells[src],t->cells[src^1],t->width,t->height,begin,end);
    else mix_range(t,t->state[src],t->state[src^1],f->index*t->waves+wave,begin,end);
#ifdef MF_AUDIT
    atomic_fetch_add_explicit(&t->done[wave],end-begin,memory_order_release);
    atomic_max_u64(&f->last[batch],afc_now_ns());
#endif
}
static const void *current(const MfTask *t) {return t->width?(const void*)t->cells[t->parity]:(const void*)t->state[t->parity];}
static uint64_t world_hash(const MfTask *tasks,uint32_t count) {
    uint64_t h=UINT64_C(0xcbf29ce484222325);
    for(uint32_t k=0;k<count;++k) {
        const uint8_t *cells=tasks[k].cells[tasks[k].parity];const uint64_t *state=tasks[k].state[tasks[k].parity];
        for(uint32_t i=0;i<tasks[k].n;++i)
            for(unsigned b=0;b<(tasks[k].width?1u:8u);++b) {
                h^=tasks[k].width?cells[i]:(state[i]>>(8*b))&255; h*=UINT64_C(0x100000001b3);
            }
    }
    return h;
}
static void release(MfTask *t) {
    for(int b=0;b<2;++b) {free(t->state[b]);free(t->cells[b]);}
#ifdef MF_AUDIT
    free(t->seen);free(t->done);
#endif
}
static int allocate(MfTask *t,uint64_t seed) {
    if(t->width) {
        t->cells[0]=calloc(t->n,1);t->cells[1]=calloc(t->n,1);
        if(!t->cells[0]||!t->cells[1]) return AF_ERROR;
        af_gol_init(seed,35,t->width,t->height,t->cells[0]);
    } else {
        t->state[0]=calloc(t->n,sizeof(uint64_t));t->state[1]=calloc(t->n,sizeof(uint64_t));
        if(!t->state[0]||!t->state[1]) return AF_ERROR;
        for(uint32_t i=0;i<t->n;++i) t->state[0][i]=mix(seed+i,1);
    }
#ifdef MF_AUDIT
    t->seen=calloc((size_t)t->n*t->waves,sizeof(*t->seen));
    t->done=calloc(t->waves,sizeof(*t->done));
    if(!t->seen||!t->done) return AF_ERROR;
#endif
    return AF_OK;
}
static unsigned number(const char *s,unsigned limit) {
    char *end; unsigned long x=strtoul(s,&end,10);
    if(!*s||*end||!x||x>limit) {fprintf(stderr,"invalid positive integer: %s\n",s);exit(2);}
    return (unsigned)x;
}
/* Independent sequential reference: afcore's GoL reference and a plain loop. */
static void reference_frame(MfTask *t,uint32_t count,uint32_t frame) {
    for(uint32_t k=0;k<count;++k) for(uint32_t p=0;p<t[k].waves;++p) {
        uint32_t s=t[k].parity;
        if(t[k].width) af_gol_ref_run(t[k].cells[s],t[k].cells[s^1],t[k].width,t[k].height,1);
        else mix_range(&t[k],t[k].state[s],t[k].state[s^1],frame*t[k].waves+p,0,t[k].n);
        t[k].parity^=1;
    }
}
/* A worker-slot map, not a CPU set. Order and repeated CPUs are intentional.
 * Pinned mode pins each worker to its slot CPU; open mode treats the distinct
 * values as the allocation and the map as a suggestion. */
static int slot_cpus(const char *text,int *cpus,uint32_t expected,int control) {
    const char *p=text;
    for(uint32_t i=0;i<expected;++i) {
        if(*p<'0'||*p>'9') return AF_ERROR;
        char *end;long cpu=strtol(p,&end,10);
        if(cpu<0||cpu>=1024||cpu==control) return AF_ERROR;
        cpus[i]=(int)cpu;
        if(i+1==expected) {if(*end) return AF_ERROR;}
        else {if(*end!=',') return AF_ERROR;}
        p=end+(*end==',');
    }
    return AF_OK;
}
int mf_main(int argc,char **argv,const MfAdapter *adapter) {
    if(argc!=9||(strcmp(argv[8],"open")&&strcmp(argv[8],"pinned"))) {
        fprintf(stderr,"usage: BIN frames warmup workers cpus control seed recipe.txt open|pinned\n");return 2;
    }
    uint32_t frames=number(argv[1],100000),warmup=number(argv[2],100000),workers=number(argv[3],AFC_MAX_WORKERS);
    int cpus[AFC_MAX_WORKERS],control=(int)strtol(argv[5],NULL,10),open=!strcmp(argv[8],"open");
    uint64_t seed=strtoull(argv[6],NULL,10);char why[256]={0};
    if(slot_cpus(argv[4],cpus,workers,control)!=AF_OK) return 2;
    int serial=!strcmp(adapter->name,"serial"),result=0,started=0;
    MfTask actual[MAX_BATCHES]={0},reference[MAX_BATCHES]={0};uint32_t count=0;
    static MfFrame frame;
    FILE *recipe=fopen(argv[7],"r");if(!recipe) {perror("recipe");return 2;}
    char kind[16],line[256],extra;uint64_t slots=0;
    while(fgets(line,sizeof line,recipe)) {
        if(count==MAX_BATCHES) {result=2;break;}
        MfTask *t=&actual[count];
        if(sscanf(line,"%15s %u %u %u %u %c",kind,&t->n,&t->iterations,&t->grain,&t->waves,&extra)!=5 ||
           (!strcmp(kind,"gol") ? !t->waves : (!strcmp(kind,"m6") ? t->waves<2 : t->waves!=1)) ||
           (strcmp(kind,"m1")&&strcmp(kind,"m4")&&strcmp(kind,"m6")&&strcmp(kind,"gol")) ||
           !t->n||t->n>16777216||!t->iterations||t->iterations>10000||!t->grain||t->grain>1000000||t->waves>4096) {
            fprintf(stderr,"invalid recipe line: %s",line);result=2;break;
        }
        t->kind=!strcmp(kind,"m1")?MF_KIND_M1:!strcmp(kind,"m4")?MF_KIND_M4:!strcmp(kind,"m6")?MF_KIND_M6:MF_KIND_GOL;
        if(t->kind==MF_KIND_GOL) {
            t->width=t->iterations;
            if(t->n%t->width) {fprintf(stderr,"GoL items must divide by width\n");result=2;break;}
            t->height=t->n/t->width;
        }
        slots+=(uint64_t)t->n*t->waves;
        if(slots>67108864) {fprintf(stderr,"pilot allocation cap exceeded\n");result=2;break;}
        reference[count]=*t;
        ++count;
        if(allocate(t,seed+count)||allocate(&reference[count-1],seed+count)) {result=2;break;}
    }
    fclose(recipe);
    if(result||!count) goto cleanup;
    frame.tasks=actual;frame.count=count;frame.worker_only=!serial;
    Sample *samples=calloc((size_t)frames+warmup,sizeof(*samples));
    if(!samples) {result=2;goto cleanup;}
    /* Open runs prescribe no waiting policy: NATIVE means the adapter's choice. */
    AfRunCfg cfg={workers,cpus,control,
        !open&&!strcmp(adapter->name,"antfarm")?AF_WAIT_SPIN:AF_WAIT_NATIVE,"{}"};
    if(af_pin_self(control)!=AF_OK || (!serial&&afc_registry_configure(workers,cpus,control,open)!=AF_OK)) {result=2;free(samples);goto cleanup;}
    if(adapter->start(&cfg)!=AF_OK) {fprintf(stderr,"adapter start failed\n");result=2;free(samples);goto cleanup;}
    started=1;
    if(!serial) {
        afc_registry_seal();
        if(af_workers_registered()!=workers) {fprintf(stderr,"start returned with %u of %u workers registered\n",af_workers_registered(),workers);result=2;free(samples);goto cleanup;}
    }
    uint32_t completed=0;
#ifdef MF_AUDIT
    uint64_t before[AFC_MAX_WORKERS]={0};
#endif
    for(uint32_t r=0;r<warmup+frames;++r) {
        Sample *s=&samples[r];s->coverage=1;
#ifdef MF_AUDIT
        for(uint32_t k=0;k<count;++k) {
            for(size_t i=0;i<(size_t)actual[k].n*actual[k].waves;++i) atomic_store(&actual[k].seen[i],0);
            for(uint32_t p=0;p<actual[k].waves;++p) atomic_store(&actual[k].done[p],0);
            atomic_store(&frame.first[k],UINT64_MAX);atomic_store(&frame.last[k],0);
        }
#endif
        frame.index=r;
        uint64_t frame_start=afc_now_ns();
        int rc=adapter->frame(&frame);
        s->wall=afc_now_ns()-frame_start;
        /* Everything below is outside the frame timer. Serial replay happens
         * only after the entire actual sequence. */
        if(rc!=AF_OK) {fprintf(stderr,"frame %u returned %d\n",r,rc);result=1;}
        if(atomic_load(&frame.fault)) {fprintf(stderr,"frame %u: invalid mf_execute arguments\n",r);result=1;}
        for(uint32_t k=0;k<count;++k) actual[k].parity^=actual[k].waves&1;
        s->hash=world_hash(actual,count);
#ifdef MF_AUDIT
        for(uint32_t k=0;k<count;++k) {
            s->span[k][0]=atomic_load(&frame.first[k])-frame_start;s->span[k][1]=atomic_load(&frame.last[k])-frame_start;
        }
        if(!serial) {
            for(uint32_t k=0;k<count;++k) {
                for(size_t i=0;i<(size_t)actual[k].n*actual[k].waves;++i)
                    if(atomic_load(&actual[k].seen[i])!=1) s->coverage=0;
                for(uint32_t p=0;p<actual[k].waves;++p)
                    if(atomic_load(&actual[k].done[p])!=actual[k].n) s->coverage=0;
            }
            if(atomic_load(&frame.foreign)||atomic_load(&frame.order)) s->coverage=0;
            for(uint32_t i=0;i<workers;++i) {
                uint64_t now=atomic_load(&frame.slot_items[i]);
                s->participants+=now!=before[i];before[i]=now;
            }
        }
#endif
        completed=r+1;
        if(result||!s->coverage) {result=1;break;}
    }
    int pins=serial?1:afc_registry_verify_pins(why,sizeof why);
    if(!pins) {fprintf(stderr,"placement verification: %s\n",why);result=1;}
    AfcBuf placement={0};
    if(!serial) afc_registry_describe(&placement);
    struct rusage usage;getrusage(RUSAGE_SELF,&usage);
    for(uint32_t r=0;r<completed;++r) {
        reference_frame(reference,count,r);
        uint64_t expected=world_hash(reference,count);Sample *s=&samples[r];
        int checksum=s->hash==expected;
        if(!checksum) result=1;
        printf("{\"schema\":\"afbench-microframe/3\",\"system\":\"%s\",\"frame\":%u,\"warmup\":%s,\"batches\":%u,\"workers\":%u,\"hosting\":\"%s\",\"audit\":%s,\"wall_ns\":%llu,\"batch_span_ns\":",
            adapter->name,r,r<warmup?"true":"false",count,serial?0:workers,serial?"serial":open?"open":"pinned",
#ifdef MF_AUDIT
            "true",
#else
            "false",
#endif
            (unsigned long long)s->wall);
        const char *coverage="null";
#ifdef MF_AUDIT
        printf("[");
        for(uint32_t k=0;k<count;++k) printf("%s[%llu,%llu]",k?",":"",(unsigned long long)s->span[k][0],(unsigned long long)s->span[k][1]);
        printf("],\"participants\":");
        if(serial) printf("null"); else printf("%u",s->participants);
        if(!serial) coverage=s->coverage?"true":"false";
#else
        printf("null,\"participants\":null");
#endif
        printf(",\"hash\":\"%016llx\",\"expected\":\"%016llx\",\"checksum_ok\":%s,\"coverage_ok\":%s,\"pin_ok\":%s,\"maxrss_kib\":%ld,\"status\":\"%s\"}\n",(unsigned long long)s->hash,(unsigned long long)expected,checksum?"true":"false",coverage,pins?"true":"false",usage.ru_maxrss,result?"failed":"ok");
    }
    if(completed==warmup+frames) for(uint32_t k=0;k<count;++k)
        if(actual[k].width ? memcmp(current(&actual[k]),current(&reference[k]),actual[k].n)
                          : memcmp(current(&actual[k]),current(&reference[k]),(size_t)actual[k].n*8)) result=1;
    /* Final summary line: observed placement and, in audits, items per slot. */
    printf("{\"schema\":\"afbench-microframe-placement/1\",\"system\":\"%s\",\"placement\":%s,\"slot_items\":",adapter->name,
           placement.p&&!placement.oom?placement.p:"null");
#ifdef MF_AUDIT
    if(!serial) {
        printf("[");
        for(uint32_t i=0;i<workers;++i) printf("%s%llu",i?",":"",(unsigned long long)atomic_load(&frame.slot_items[i]));
        printf("]");
    } else printf("null");
#else
    printf("null");
#endif
    printf(",\"process_cpu_ns\":%llu,\"status\":\"%s\"}\n",
           (unsigned long long)(usage.ru_utime.tv_sec+usage.ru_stime.tv_sec)*1000000000ull+(unsigned long long)(usage.ru_utime.tv_usec+usage.ru_stime.tv_usec)*1000ull,
           result?"failed":"ok");
    afc_buf_free(&placement);
    free(samples);
cleanup:
    if(started) adapter->stop();
    for(uint32_t k=0;k<count;++k) {release(&actual[k]);release(&reference[k]);}
    return count?result:2;
}
