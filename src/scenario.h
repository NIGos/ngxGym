// A scenario is a list of things a game does, in order, one per line.
//
// The whole point is that a line here replaces a minute of playing a real game to a
// particular state. "go borderless, then exclusive fullscreen, then change the DLSS
// preset" is three lines and about ten seconds, and it is the sequence that produced
// four of the defects this repository exists to catch.
//
//   # a comment
//   frames 300          run 300 frames
//   mode borderless     windowed | borderless | exclusive
//   resize 2560 1440    change the output size; the feature is rebuilt
//   preset 5            PerfQualityValue: 0 MaxPerf .. 2 MaxQuality .. 5 DLAA
//   recreate            destroy and create the feature at an UNCHANGED shape
//   dlss off            stop evaluating, keep presenting -- the game's menu toggle
//   dlss on             start again
//   hdr on|off          swapchain format and colour space, and the IsHDR create flag
//
// Deliberate misbehaviour, which a real game cannot be asked to perform on demand
// and a test host can. Each one reproduces something measured in a real title:
//
//   transpose on        swap Width/Height with OutWidth/OutHeight in the EVALUATE
//                       block only, leaving the images honest. Baldur's Gate 3 after
//                       a borderless <-> fullscreen change, 2026-09-01.
//   omit mvscale        stop setting MV.Scale.X/Y at all.
//   omit flags          stop setting DLSS.Feature.Create.Flags at all.
//   omit jitter         stop setting Jitter.Offset.X/Y at all.
//   omit quality        stop setting PerfQualityValue on the evaluate, which is the
//                       shape every real title on record has: the add-on then keeps
//                       its own and says so.
//   exposure on         supply a 1x1 ExposureTexture while NOT setting the
//                       AutoExposure create flag. Mount & Blade II: Bannerlord and
//                       Red Dead Redemption 2 both do this; the second one stood the
//                       Vulkan mirror down for a whole session until 1.4.0.
#pragma once

#include <cstdio>
#include <cstring>
#include <cstdlib>

enum StepKind
{
    STEP_FRAMES, STEP_MODE, STEP_RESIZE, STEP_PRESET, STEP_RECREATE,
    STEP_DLSS, STEP_HDR, STEP_TRANSPOSE, STEP_OMIT, STEP_EXPOSURE, STEP_STALE, STEP_SDR, STEP_SCRGB, STEP_DEPTHCOLOR, STEP_PAD
};

enum Mode { MODE_WINDOWED, MODE_BORDERLESS, MODE_EXCLUSIVE };
enum Omit { OMIT_NONE, OMIT_MVSCALE, OMIT_FLAGS, OMIT_JITTER, OMIT_QUALITY };

struct Step
{
    StepKind kind;
    int a, b;
};

struct Scenario
{
    Step steps[64];
    int  count = 0;
    char name[64] = "inline";
    // "nodlss": the host never creates an NGX feature. A game that has no DLSS
    // at all, which is the case the substitute contract's pre-arm path is for.
    bool nodlss = false;
};

// "on" or "off" and nothing else. "onn" used to read as off, and the
// misbehaviour the line asked for was never applied while the run went green.
inline bool OnOff(const char *w, int *out)
{
    if (_stricmp(w, "on") == 0)  { *out = 1; return true; }
    if (_stricmp(w, "off") == 0) { *out = 0; return true; }
    return false;
}

// A whole number and nothing else. atoi read "3OO" as 0 frames.
inline bool Num(const char *w, int *out)
{
    char *end = nullptr;
    const long v = strtol(w, &end, 10);
    if (end == w || *end != 0) return false;
    *out = static_cast<int>(v);
    return true;
}

inline bool ScenarioAdd(Scenario *s, StepKind k, int a = 0, int b = 0)
{
    if (s->count >= 64) return false;
    s->steps[s->count].kind = k;
    s->steps[s->count].a = a;
    s->steps[s->count].b = b;
    ++s->count;
    return true;
}

// Returns false and says which line it could not read. A scenario that silently
// skips a verb it does not know is a run that proves something other than what its
// file says, which is worse than no run.
inline bool ScenarioLoad(Scenario *s, const char *path)
{
    FILE *f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || f == nullptr)
    { printf("scenario: cannot open %s\n", path); return false; }

    char line[256];
    int  no = 0;
    bool ok = true;
    while (fgets(line, sizeof(line), f) != nullptr)
    {
        ++no;
        char verb[32] = {}, a1[32] = {}, a2[32] = {}, a3[32] = {};
        const int n = sscanf_s(line, "%31s %31s %31s %31s", verb, static_cast<unsigned>(sizeof(verb)), a1, static_cast<unsigned>(sizeof(a1)), a2, static_cast<unsigned>(sizeof(a2)), a3, static_cast<unsigned>(sizeof(a3)));
        if (n < 1 || verb[0] == '#') continue;

        int v = 0, w = 0;
        bool bad = false;
        if      (_stricmp(verb, "frames")   == 0 && n >= 2) { if (Num(a1, &v) && v > 0) ScenarioAdd(s, STEP_FRAMES, v); else bad = true; }
        else if (_stricmp(verb, "resize")   == 0 && n >= 3) { if (Num(a1, &v) && Num(a2, &w) && v > 0 && w > 0) ScenarioAdd(s, STEP_RESIZE, v, w); else bad = true; }
        else if (_stricmp(verb, "preset")   == 0 && n >= 2) { if (Num(a1, &v) && v >= 0 && v <= 5) ScenarioAdd(s, STEP_PRESET, v); else bad = true; }
        else if (_stricmp(verb, "recreate") == 0)           ScenarioAdd(s, STEP_RECREATE);
        else if (_stricmp(verb, "mode")     == 0 && n >= 2)
        {
            if      (_stricmp(a1, "windowed")   == 0) ScenarioAdd(s, STEP_MODE, MODE_WINDOWED);
            else if (_stricmp(a1, "borderless") == 0) ScenarioAdd(s, STEP_MODE, MODE_BORDERLESS);
            else if (_stricmp(a1, "exclusive")  == 0) ScenarioAdd(s, STEP_MODE, MODE_EXCLUSIVE);
            else bad = true;
        }
        else if (_stricmp(verb, "dlss")       == 0 && n >= 2) { if (OnOff(a1, &v)) ScenarioAdd(s, STEP_DLSS, v); else bad = true; }
        else if (_stricmp(verb, "hdr")        == 0 && n >= 2) { if (OnOff(a1, &v)) ScenarioAdd(s, STEP_HDR, v); else bad = true; }
        else if (_stricmp(verb, "sdr")        == 0 && n >= 2) { if (OnOff(a1, &v)) ScenarioAdd(s, STEP_SDR, v); else bad = true; }
        else if (_stricmp(verb, "scrgb")      == 0 && n >= 2)
        {
            // Optional peak in nits for the scene's brightest pixel, 640 by
            // default, and an optional "g22" in either position asking for the
            // gamma-2.2 colour space instead of linear -- what a game that never
            // calls SetColorSpace1 leaves on a float swapchain. Encoded as a
            // negative peak, since Step has two fields.
            int nits = 640, g22 = 1;
            if (!OnOff(a1, &v)) bad = true;
            const char *xs[2] = { a2, a3 };
            for (const char *x : xs)
            {
                if (x[0] == 0) continue;
                if (_stricmp(x, "g22") == 0) g22 = -1;
                else if (!Num(x, &nits) || nits <= 0) bad = true;
            }
            if (!bad) ScenarioAdd(s, STEP_SCRGB, v, nits * g22);
        }
        else if (_stricmp(verb, "depthcolor") == 0 && n >= 2) { if (OnOff(a1, &v)) ScenarioAdd(s, STEP_DEPTHCOLOR, v); else bad = true; }
        else if (_stricmp(verb, "pad")        == 0 && n >= 2) { if (Num(a1, &v) && v >= 0) ScenarioAdd(s, STEP_PAD, v); else bad = true; }
        else if (_stricmp(verb, "transpose")  == 0 && n >= 2) { if (OnOff(a1, &v)) ScenarioAdd(s, STEP_TRANSPOSE, v); else bad = true; }
        else if (_stricmp(verb, "stale")      == 0 && n >= 2) { if (OnOff(a1, &v)) ScenarioAdd(s, STEP_STALE, v); else bad = true; }
        else if (_stricmp(verb, "nodlss")     == 0)           s->nodlss = true;
        else if (_stricmp(verb, "exposure")   == 0 && n >= 2) { if (OnOff(a1, &v)) ScenarioAdd(s, STEP_EXPOSURE, v); else bad = true; }
        else if (_stricmp(verb, "omit") == 0 && n >= 2)
        {
            if      (_stricmp(a1, "mvscale") == 0) ScenarioAdd(s, STEP_OMIT, OMIT_MVSCALE);
            else if (_stricmp(a1, "flags")   == 0) ScenarioAdd(s, STEP_OMIT, OMIT_FLAGS);
            else if (_stricmp(a1, "jitter")  == 0) ScenarioAdd(s, STEP_OMIT, OMIT_JITTER);
            else if (_stricmp(a1, "quality") == 0) ScenarioAdd(s, STEP_OMIT, OMIT_QUALITY);
            else if (_stricmp(a1, "none")    == 0) ScenarioAdd(s, STEP_OMIT, OMIT_NONE);
            else bad = true;
        }
        else bad = true;
        if (bad) { printf("scenario %s:%d: cannot read '%s %s %s %s'\n", path, no, verb, a1, a2, a3); ok = false; }
        if (s->count >= 64) { printf("scenario %s:%d: more than 64 steps\n", path, no); ok = false; break; }
    }
    fclose(f);
    if (s->nodlss)
        for (int i = 0; i < s->count; ++i)
            if (s->steps[i].kind == STEP_DLSS && s->steps[i].a != 0)
            { printf("scenario %s: 'dlss on' with 'nodlss', and the host has no NGX to switch on\n", path); ok = false; }

    const char *leaf = strrchr(path, '\\');
    leaf = leaf ? leaf + 1 : path;
    strncpy_s(s->name, leaf, _TRUNCATE);
    return ok && s->count > 0;
}

inline const char *StepName(const Step &s)
{
    switch (s.kind)
    {
    case STEP_FRAMES:    return "frames";
    case STEP_MODE:      return s.a == MODE_WINDOWED ? "mode windowed"
                              : s.a == MODE_BORDERLESS ? "mode borderless" : "mode exclusive";
    case STEP_RESIZE:    return "resize";
    case STEP_PRESET:    return "preset";
    case STEP_RECREATE:  return "recreate";
    case STEP_DLSS:      return s.a ? "dlss on" : "dlss off";
    case STEP_HDR:       return s.a ? "hdr on" : "hdr off";
    case STEP_SDR:       return s.a ? "sdr on" : "sdr off";
    case STEP_SCRGB:     return s.a ? "scrgb on" : "scrgb off";
    case STEP_DEPTHCOLOR: return s.a ? "depthcolor on" : "depthcolor off";
    case STEP_PAD:       return "pad";
    case STEP_TRANSPOSE: return s.a ? "transpose on" : "transpose off";
    case STEP_STALE:     return s.a ? "stale on" : "stale off";
    case STEP_OMIT:      return "omit";
    case STEP_EXPOSURE:  return s.a ? "exposure on" : "exposure off";
    }
    return "?";
}

// How long a 'frames N' step took, printed under it. The counters at the end of a
// run say what the add-on did; they say nothing about what it cost, and a change
// to the bridge's transport is measured in exactly this number. QueryPerformance-
// Counter because a step is seconds long and GetTickCount's 16 ms would be a
// tenth of a short one. Rate rather than time alone: the frame counts differ
// between steps, so only the rate compares across a scenario.
inline void PrintStepRate(const LARGE_INTEGER &t0, int frames)
{
    LARGE_INTEGER t1, f;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    const double ms = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 /
                      static_cast<double>(f.QuadPart);
    printf("  step took %.1f ms, %.1f fps\n", ms,
           ms > 0.0 ? static_cast<double>(frames) * 1000.0 / ms : 0.0);
}
