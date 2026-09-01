// The DLSS contract, as a table of keys that can each be PRESENT or ABSENT.
//
// THE RULE THIS FILE EXISTS FOR, and it is the reason not to use the SDK's own
// helper macros: NGX_D3D11_CREATE_DLSS_EXT and NGX_D3D11_EVALUATE_DLSS_EXT write
// every scalar unconditionally, and substitute neutral values for zeros. That makes
// "the game declared nothing" unreachable -- and an absent key is an entire class of
// real defect. The bridge under test has a sentinel value for unset create flags, a
// fallback for an absent MV.Scale, and a note it prints when a game omits something;
// none of those paths can be reached by a host that always sets everything.
//
// So: no defaults, ever. A key is written only when its has_ flag is set. Anyone
// adding one convenience default deletes the reason this repository exists.
#pragma once

#include <cstring>

struct Key { bool has; unsigned int u; float f; };

inline void SetU(Key *k, unsigned int v) { k->has = true; k->u = v; }
inline void SetF(Key *k, float v)        { k->has = true; k->f = v; }

// Everything a create can state. Names are NGX's own, spelled as the bridge reads
// them -- it looks these up by string, so a typo here is a silent absence there.
struct CreateContract
{
    Key width, height, out_width, out_height;
    Key perf_quality;
    Key create_flags;
    Key output_subrects;
    Key node_mask_creation, node_mask_visibility;
};

// Everything an evaluate can state, minus the four resources, which are set
// separately because their types differ per API.
struct EvalContract
{
    Key jitter_x, jitter_y;
    Key mv_scale_x, mv_scale_y;
    Key sharpness;
    Key pre_exposure, exposure_scale;
    Key reset;
    Key subrect_w, subrect_h;
    Key in_color_x, in_color_y, in_depth_x, in_depth_y, in_mv_x, in_mv_y;
    Key out_x, out_y;
};

template <typename P> void ApplyCreate(P *p, const CreateContract &c)
{
    if (c.width.has)                p->Set("Width",                        c.width.u);
    if (c.height.has)               p->Set("Height",                       c.height.u);
    if (c.out_width.has)            p->Set("OutWidth",                     c.out_width.u);
    if (c.out_height.has)           p->Set("OutHeight",                    c.out_height.u);
    if (c.perf_quality.has)         p->Set("PerfQualityValue",             c.perf_quality.u);
    if (c.create_flags.has)         p->Set("DLSS.Feature.Create.Flags",    c.create_flags.u);
    if (c.output_subrects.has)      p->Set("DLSS.Enable.Output.Subrects",  c.output_subrects.u);
    if (c.node_mask_creation.has)   p->Set("CreationNodeMask",             c.node_mask_creation.u);
    if (c.node_mask_visibility.has) p->Set("VisibilityNodeMask",           c.node_mask_visibility.u);
}

template <typename P> void ApplyEval(P *p, const EvalContract &e)
{
    if (e.jitter_x.has)       p->Set("Jitter.Offset.X",                        e.jitter_x.f);
    if (e.jitter_y.has)       p->Set("Jitter.Offset.Y",                        e.jitter_y.f);
    if (e.mv_scale_x.has)     p->Set("MV.Scale.X",                             e.mv_scale_x.f);
    if (e.mv_scale_y.has)     p->Set("MV.Scale.Y",                             e.mv_scale_y.f);
    if (e.sharpness.has)      p->Set("Sharpness",                              e.sharpness.f);
    if (e.pre_exposure.has)   p->Set("DLSS.Pre.Exposure",                      e.pre_exposure.f);
    if (e.exposure_scale.has) p->Set("DLSS.Exposure.Scale",                    e.exposure_scale.f);
    if (e.reset.has)          p->Set("Reset",                                  static_cast<int>(e.reset.u));
    if (e.subrect_w.has)      p->Set("DLSS.Render.Subrect.Dimensions.Width",   e.subrect_w.u);
    if (e.subrect_h.has)      p->Set("DLSS.Render.Subrect.Dimensions.Height",  e.subrect_h.u);
    if (e.in_color_x.has)     p->Set("DLSS.Input.Color.Subrect.Base.X",        e.in_color_x.u);
    if (e.in_color_y.has)     p->Set("DLSS.Input.Color.Subrect.Base.Y",        e.in_color_y.u);
    if (e.in_depth_x.has)     p->Set("DLSS.Input.Depth.Subrect.Base.X",        e.in_depth_x.u);
    if (e.in_depth_y.has)     p->Set("DLSS.Input.Depth.Subrect.Base.Y",        e.in_depth_y.u);
    if (e.in_mv_x.has)        p->Set("DLSS.Input.MV.Subrect.Base.X",           e.in_mv_x.u);
    if (e.in_mv_y.has)        p->Set("DLSS.Input.MV.Subrect.Base.Y",           e.in_mv_y.u);
    if (e.out_x.has)          p->Set("DLSS.Output.Subrect.Base.X",             e.out_x.u);
    if (e.out_y.has)          p->Set("DLSS.Output.Subrect.Base.Y",             e.out_y.u);
}

// Halton, for the sub-pixel jitter DLSS requires. Without it the temporal
// accumulation has nothing new to accumulate and the result is soft -- and, more to
// the point here, the bridge reads these two keys and a host that never sets them
// exercises a different path from every real game.
inline float Halton(int i, int b)
{
    float f = 1.0f, r = 0.0f;
    while (i > 0) { f /= static_cast<float>(b); r += f * static_cast<float>(i % b); i /= b; }
    return r;
}
