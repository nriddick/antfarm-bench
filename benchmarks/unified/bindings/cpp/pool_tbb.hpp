#ifndef AF_POOL_TBB_HPP
#define AF_POOL_TBB_HPP

#include "afcore.hpp"
#include <oneapi/tbb/global_control.h>
#include <oneapi/tbb/task_arena.h>
#include <oneapi/tbb/task_scheduler_observer.h>
#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <latch>
#include <memory>
#include <stdexcept>

namespace af {

class TbbPool {
public:
    struct Options {
        int max_concurrency;
        unsigned reserved_slots;
        oneapi::tbb::task_arena::priority priority = oneapi::tbb::task_arena::priority::normal;
    };
    using OptionsHook = std::function<void(Options&)>;

private:
    class Observer : public oneapi::tbb::task_scheduler_observer {
    public:
        std::atomic<int> error{AF_OK};
        explicit Observer(oneapi::tbb::task_arena& arena) : task_scheduler_observer(arena) {}
        void on_scheduler_entry(bool) override {
            if (af_worker_enter(AF_WORKER_AUTO) != AF_OK)
                error.store(AF_ERROR);
        }
        ~Observer() override { observe(false); }
    };

    oneapi::tbb::global_control control_;
    oneapi::tbb::task_arena arena_;
    Observer observer_;

public:
    explicit TbbPool(const AfRunCfg& cfg, const OptionsHook& options = {})
        : control_(oneapi::tbb::global_control::max_allowed_parallelism, cfg.workers + 1),
          arena_(static_cast<int>(cfg.workers), 0), observer_(arena_) {
        Options settings{static_cast<int>(cfg.workers), 0};
        if (options) options(settings);
        settings.max_concurrency = static_cast<int>(cfg.workers);
        settings.reserved_slots = 0;
        arena_.initialize(settings.max_concurrency, settings.reserved_slots, settings.priority);
        observer_.observe(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        const auto deadline_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            deadline.time_since_epoch()).count();
        auto ready = std::make_shared<std::latch>(cfg.workers);
        for (uint32_t i = 0; i < cfg.workers; ++i) {
            arena_.enqueue([this, ready, deadline_ns] {
                if (af_workers_rendezvous(deadline_ns) != AF_OK)
                    observer_.error.store(AF_ERROR);
                ready->count_down();
            });
        }
        ready->wait();
        if (observer_.error.load() != AF_OK || af_workers_registered() != cfg.workers)
            throw std::runtime_error("worker registration failed");
        if (af_pin_self(cfg.control_cpu) != AF_OK)
            throw std::runtime_error("control placement failed");
    }

    TbbPool(const TbbPool&) = delete;
    TbbPool& operator=(const TbbPool&) = delete;

    template<class F>
    void run(F&& callback) {
        auto done = std::make_shared<std::latch>(1);
        std::exception_ptr error;
        arena_.enqueue([&, done] {
            try { callback(); }
            catch (...) { error = std::current_exception(); }
            done->count_down();
        });
        done->wait();
        if (error) std::rethrow_exception(error);
        if (observer_.error.load() != AF_OK)
            throw std::runtime_error("worker registration failed");
    }

    ~TbbPool() {
        observer_.observe(false);
        arena_.terminate();
    }
};

} // namespace af
#endif
