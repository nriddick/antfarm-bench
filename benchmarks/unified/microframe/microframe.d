module microframe;
public import afcore;

/// Composite-frame sidecar ABI v2; see microframe.h for the contract.
struct MfFrame;
enum : uint { MF_KIND_M1 = 1, MF_KIND_M4 = 2, MF_KIND_M6 = 3, MF_KIND_GOL = 4 }
struct MfBatchShape {
    uint kind, items, waves, grain, width;
}
extern(C) nothrow {
    alias MfStart = int function(const(AfRunCfg)*);
    alias MfFrameRun = int function(MfFrame*);
    alias MfStop = void function();
    uint mf_batch_count(const(MfFrame)*) @nogc;
    void mf_batch_shape(const(MfFrame)*, uint batch, MfBatchShape* out_) @nogc;
    void mf_execute(MfFrame*, uint batch, uint wave, uint begin, uint end) @nogc;
    int mf_main(int, char**, const(MfAdapter)*);
}
struct MfAdapter {
    const(char)* name;
    MfStart start;
    MfFrameRun frame;
    MfStop stop;
}
