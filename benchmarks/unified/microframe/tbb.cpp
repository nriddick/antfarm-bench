#include "microframe.h"
#include "pool_tbb.hpp"
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/partitioner.h>
#include <memory>
// Reference adapter: batches in order, one parallel_for per wave.
static std::unique_ptr<af::TbbPool> pool;
static int start(const AfRunCfg *cfg) {
    try {pool=std::make_unique<af::TbbPool>(*cfg);return AF_OK;} catch(...) {return AF_ERROR;}
}
static int frame(MfFrame *f) {
    try {
        pool->run([&] {
            for(uint32_t b=0;b<mf_batch_count(f);++b) {
                MfBatchShape s;mf_batch_shape(f,b,&s);
                for(uint32_t w=0;w<s.waves;++w)
                    oneapi::tbb::parallel_for(oneapi::tbb::blocked_range<uint32_t>(0,s.items,s.grain),
                        [&](const auto &r) {mf_execute(f,b,w,r.begin(),r.end());},oneapi::tbb::simple_partitioner{});
            }
        });
        return AF_OK;
    } catch(...) {return AF_ERROR;}
}
static void stop() {pool.reset();}
int main(int argc,char **argv) {
    const MfAdapter adapter={"tbb",start,frame,stop};
    return mf_main(argc,argv,&adapter);
}
