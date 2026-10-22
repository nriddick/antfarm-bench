module entry_antfarm;
import microframe;
import antfarm_frame : start, frame, stop;
import std.string : toStringz;

extern(C) __gshared string[] rt_options = ["gcopt=parallel:0"];
int main(string[] args) {
    auto argv = new char*[args.length + 1];
    foreach (i, arg; args) argv[i] = cast(char*) arg.toStringz;
    const adapter = MfAdapter("antfarm", &start, &frame, &stop);
    return mf_main(cast(int) args.length, argv.ptr, &adapter);
}
