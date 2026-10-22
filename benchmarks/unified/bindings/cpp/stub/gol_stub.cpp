#include "afcore.hpp"
#include "pool_tbb.hpp"
#include <algorithm>
#include <memory>
#include <vector>

class GolStub : public af::GolImpl {
    std::unique_ptr<af::TbbPool> pool_;
    std::vector<uint8_t> grid_, next_;
    uint32_t width_ = 0, height_ = 0;
public:
    int start(const AfRunCfg& cfg) override {
        pool_ = std::make_unique<af::TbbPool>(cfg);
        return AF_OK;
    }
    int load(const uint8_t* grid, uint32_t w, uint32_t h) override {
        width_ = w; height_ = h;
        grid_.assign(grid, grid + size_t(w) * h);
        next_.resize(grid_.size());
        return AF_OK;
    }
    int run(uint32_t generations) override {
        af_gol_ref_run(grid_.data(), next_.data(), width_, height_, generations);
        grid_.swap(next_);
        return AF_OK;
    }
    int store(uint8_t* out) override {
        std::copy(grid_.begin(), grid_.end(), out);
        return AF_OK;
    }
    int stop() override { pool_.reset(); return AF_OK; }
};

AF_GOL_MAIN(GolStub)
