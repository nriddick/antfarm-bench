#ifndef AF_MICROFRAME_H
#define AF_MICROFRAME_H
#include "afcore.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Composite-frame sidecar ABI v2. It does not change frozen afcore ABI v1.
 *
 * A frame is a fixed list of mutually independent batches. Batch b runs
 * shape.waves sequential waves of shape.items items. The only dependencies:
 * every item of wave w-1 of batch b must have completed (and be visible)
 * before any item of wave w of the same batch starts. Different batches may
 * run in any order or overlap. frame() returns only after every item of every
 * wave of every batch has executed exactly once; it must not leave work
 * running past its return. mf_execute(frame, b, w, begin, end) executes items
 * [begin, end) of wave w of batch b; ranges may be split at any boundary.
 * Batch state alternates buffers internally, so concurrent calls for
 * different batches, or disjoint ranges of the same wave, are safe.
 */
typedef struct MfFrame MfFrame;
enum { MF_KIND_M1 = 1, MF_KIND_M4 = 2, MF_KIND_M6 = 3, MF_KIND_GOL = 4 };
typedef struct MfBatchShape {
    uint32_t kind;  /* MF_KIND_*: M1 uniform, M4 mixed cost, M6 dependent waves, GoL */
    uint32_t items; /* items per wave; GoL: width * height cells */
    uint32_t waves; /* sequential waves; GoL: generations */
    uint32_t grain; /* recipe grain hint; partitioning is the adapter's choice */
    uint32_t width; /* GoL row width; 0 otherwise */
} MfBatchShape;
typedef struct MfAdapter {
    const char *name;
    int (*start)(const AfRunCfg *); /* untimed; registers exactly cfg->workers workers */
    int (*frame)(MfFrame *);        /* timed; executes the whole frame */
    void (*stop)(void);
} MfAdapter;
uint32_t mf_batch_count(const MfFrame *);
void mf_batch_shape(const MfFrame *, uint32_t batch, MfBatchShape *out);
/* Opaque shared C payload. Writes only the batch's output for these items. */
void mf_execute(MfFrame *, uint32_t batch, uint32_t wave, uint32_t begin, uint32_t end);
int mf_main(int argc, char **argv, const MfAdapter *);
#ifdef __cplusplus
}
#endif
#endif
