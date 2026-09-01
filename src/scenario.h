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
    STEP_DLSS, STEP_HDR, STEP_TRANSPOSE, STEP_OMIT, STEP_EXPOSURE, STEP_STALE
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
};

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
        char verb[32] = {}, a1[32] = {}, a2[32] = {};
        const int n = sscanf_s(line, "%31s %31s %31s",
                               verb, static_cast<unsigned>(sizeof(verb)),
                               a1,   static_cast<unsigned>(sizeof(a1)),
                               a2,   static_cast<unsigned>(sizeof(a2)));
        if (n < 1 || verb[0] == '#') continue;

        if      (_stricmp(verb, "frames")   == 0 && n >= 2) ScenarioAdd(s, STEP_FRAMES, atoi(a1));
        else if (_stricmp(verb, "resize")   == 0 && n >= 3) ScenarioAdd(s, STEP_RESIZE, atoi(a1), atoi(a2));
        else if (_stricmp(verb, "preset")   == 0 && n >= 2) ScenarioAdd(s, STEP_PRESET, atoi(a1));
        else if (_stricmp(verb, "recreate") == 0)           ScenarioAdd(s, STEP_RECREATE);
        else if (_stricmp(verb, "mode")     == 0 && n >= 2)
        {
            if      (_stricmp(a1, "windowed")   == 0) ScenarioAdd(s, STEP_MODE, MODE_WINDOWED);
            else if (_stricmp(a1, "borderless") == 0) ScenarioAdd(s, STEP_MODE, MODE_BORDERLESS);
            else if (_stricmp(a1, "exclusive")  == 0) ScenarioAdd(s, STEP_MODE, MODE_EXCLUSIVE);
            else { printf("scenario %s:%d: unknown mode '%s'\n", path, no, a1); ok = false; }
        }
        else if (_stricmp(verb, "dlss") == 0 && n >= 2)
            ScenarioAdd(s, STEP_DLSS, _stricmp(a1, "on") == 0 ? 1 : 0);
        else if (_stricmp(verb, "hdr") == 0 && n >= 2)
            ScenarioAdd(s, STEP_HDR, _stricmp(a1, "on") == 0 ? 1 : 0);
        else if (_stricmp(verb, "transpose") == 0 && n >= 2)
            ScenarioAdd(s, STEP_TRANSPOSE, _stricmp(a1, "on") == 0 ? 1 : 0);
        else if (_stricmp(verb, "stale") == 0 && n >= 2)
            ScenarioAdd(s, STEP_STALE, _stricmp(a1, "on") == 0 ? 1 : 0);
        else if (_stricmp(verb, "exposure") == 0 && n >= 2)
            ScenarioAdd(s, STEP_EXPOSURE, _stricmp(a1, "on") == 0 ? 1 : 0);
        else if (_stricmp(verb, "omit") == 0 && n >= 2)
        {
            if      (_stricmp(a1, "mvscale") == 0) ScenarioAdd(s, STEP_OMIT, OMIT_MVSCALE);
            else if (_stricmp(a1, "flags")   == 0) ScenarioAdd(s, STEP_OMIT, OMIT_FLAGS);
            else if (_stricmp(a1, "jitter")  == 0) ScenarioAdd(s, STEP_OMIT, OMIT_JITTER);
            else if (_stricmp(a1, "quality") == 0) ScenarioAdd(s, STEP_OMIT, OMIT_QUALITY);
            else if (_stricmp(a1, "none")    == 0) ScenarioAdd(s, STEP_OMIT, OMIT_NONE);
            else { printf("scenario %s:%d: unknown omit '%s'\n", path, no, a1); ok = false; }
        }
        else { printf("scenario %s:%d: unknown verb '%s'\n", path, no, verb); ok = false; }
    }
    fclose(f);

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
    case STEP_TRANSPOSE: return s.a ? "transpose on" : "transpose off";
    case STEP_STALE:     return s.a ? "stale on" : "stale off";
    case STEP_OMIT:      return "omit";
    case STEP_EXPOSURE:  return s.a ? "exposure on" : "exposure off";
    }
    return "?";
}
