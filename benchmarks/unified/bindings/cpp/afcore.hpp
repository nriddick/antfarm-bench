#ifndef AFCORE_HPP
#define AFCORE_HPP

#include "afcore.h"

static_assert(AF_ABI_VERSION == 1);
static_assert(sizeof(AfRunCfg) == 32 && alignof(AfRunCfg) == 8);
static_assert(sizeof(AfAudit) == 56 && alignof(AfAudit) == 8);
static_assert(sizeof(AfGolImpl) == 64 && alignof(AfGolImpl) == 8);
// afcore.h checks every frozen field size and offset as well.

#ifdef AF_AUDIT
#define AF_AUDIT_CELL(gen, cell, next) af_audit_cell((gen), (cell), (next))
#define AF_AUDIT_CELL_BEGIN(gen, cell) af_audit_cell_begin((gen), (cell))
#define AF_AUDIT_CELL_END(next) af_audit_cell_end((next))
#define AF_AUDIT_AUX() af_audit_aux()
#else
#define AF_AUDIT_CELL(gen, cell, next) ((void)0)
#define AF_AUDIT_CELL_BEGIN(gen, cell) ((void)0)
#define AF_AUDIT_CELL_END(next) ((void)0)
#define AF_AUDIT_AUX() ((void)0)
#endif

namespace af {

class GolImpl {
public:
    virtual ~GolImpl() = default;
    virtual int start(const AfRunCfg& cfg) = 0;
    virtual int load(const uint8_t* grid, uint32_t w, uint32_t h) = 0;
    virtual int run(uint32_t generations) = 0;
    virtual int store(uint8_t* out) = 0;
    virtual int telemetry(char* json, size_t cap) {
        if (cap < 3) return AF_ERROR;
        json[0] = '{'; json[1] = '}'; json[2] = '\0';
        return AF_OK;
    }
    virtual int stop() = 0;
};

template<class F>
int status(F&& callback) noexcept {
    try { return callback(); }
    catch (...) { return AF_ERROR; }
}

namespace detail {

template<class T>
AfGolImpl make_table(T& impl) {
    return {
        AF_ABI_VERSION, &impl,
        [](void* self, const AfRunCfg* cfg) {
            return status([&] { return static_cast<T*>(self)->start(*cfg); });
        },
        [](void* self, const uint8_t* grid, uint32_t w, uint32_t h) {
            return status([&] { return static_cast<T*>(self)->load(grid, w, h); });
        },
        [](void* self, uint32_t generations) {
            return status([&] { return static_cast<T*>(self)->run(generations); });
        },
        [](void* self, uint8_t* out) {
            return status([&] { return static_cast<T*>(self)->store(out); });
        },
        [](void* self, char* json, size_t cap) {
            return status([&] { return static_cast<T*>(self)->telemetry(json, cap); });
        },
        [](void* self) {
            return status([&] { return static_cast<T*>(self)->stop(); });
        }
    };
}

} // namespace detail

template<class T>
int main(int argc, char** argv) noexcept {
    return status([&] {
        static T impl;
        static const AfGolImpl table = detail::make_table(impl);
        return af_gol_main(argc, argv, &table);
    });
}

} // namespace af

#define AF_GOL_MAIN(T) int main(int argc, char** argv) { return af::main<T>(argc, argv); }

#endif
