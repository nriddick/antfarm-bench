import afcore;
import pool_antfarm;

final class GolStub : GolImpl
{
    private AntfarmPool pool;
    private ubyte[] grid, next;
    private uint width, height;

    int start(ref const AfRunCfg cfg)
    {
        pool = AntfarmPool.create(cfg);
        return AF_OK;
    }

    int load(const(ubyte)* cells, uint w, uint h)
    {
        width = w;
        height = h;
        grid = cells[0 .. cast(size_t) w * h].dup;
        next = new ubyte[grid.length];
        return AF_OK;
    }

    int run(uint generations)
    {
        af_gol_ref_run(grid.ptr, next.ptr, width, height, generations);
        auto previous = grid;
        grid = next;
        next = previous;
        return AF_OK;
    }

    int store(ubyte* output)
    {
        output[0 .. grid.length] = grid[];
        return AF_OK;
    }

    int stop()
    {
        if (pool !is null)
            pool.close();
        return AF_OK;
    }
}

mixin golMain!GolStub;
