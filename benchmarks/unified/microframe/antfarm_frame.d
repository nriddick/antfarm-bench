module antfarm_frame;
import microframe;
import pool_antfarm;
// Neutral producer stub. Execute every wave of every batch with Antfarm,
// honoring wave order within each batch, before frame returns.
extern(C) nothrow {
    int start(const(AfRunCfg)* cfg) { return AF_ERROR; }
    int frame(MfFrame* frame) { return AF_ERROR; }
    void stop() {}
}
