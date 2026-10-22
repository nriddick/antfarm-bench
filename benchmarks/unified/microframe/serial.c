#include "microframe.h"
static int start(const AfRunCfg *cfg) {(void)cfg;return AF_OK;}
static int frame(MfFrame *f) {
    for(uint32_t b=0;b<mf_batch_count(f);++b) {
        MfBatchShape s;mf_batch_shape(f,b,&s);
        for(uint32_t w=0;w<s.waves;++w) mf_execute(f,b,w,0,s.items);
    }
    return AF_OK;
}
static void stop(void) {}
int main(int argc,char **argv) {
    const MfAdapter adapter={"serial",start,frame,stop};
    return mf_main(argc,argv,&adapter);
}
