#ifndef AF_TBB_FOR_HPP
#define AF_TBB_FOR_HPP

#include "afcore.hpp"
#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/blocked_range2d.h>
#include <oneapi/tbb/parallel_for.h>
#include <cstddef>

namespace af {

template<class Index, class Partitioner, class F>
void for_each_index(Index n, size_t grain, Partitioner&& partitioner, const F& f) {
    oneapi::tbb::parallel_for(oneapi::tbb::blocked_range<Index>(0, n, grain),
        [&](const oneapi::tbb::blocked_range<Index>& range) {
            AF_AUDIT_AUX();
            for (Index i = range.begin(); i != range.end(); ++i) f(i);
        }, partitioner);
}

template<class Index, class Partitioner, class F>
void for_each_index_2d(Index w, Index h, size_t gx, size_t gy, Partitioner&& partitioner,
                       const F& f) {
    oneapi::tbb::parallel_for(oneapi::tbb::blocked_range2d<Index>(0, h, gy, 0, w, gx),
        [&](const oneapi::tbb::blocked_range2d<Index>& range) {
            AF_AUDIT_AUX();
            for (Index y = range.rows().begin(); y != range.rows().end(); ++y)
                for (Index x = range.cols().begin(); x != range.cols().end(); ++x) f(x, y);
        }, partitioner);
}

} // namespace af
#endif
