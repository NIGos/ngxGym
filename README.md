# ngxGym

A scriptable DLSS host for testing ReShade add-ons that sit on the NGX path,
built for [dlss5-bridge](https://github.com/NIGos/dlss5-bridge). Two small
executables create a real D3D11 or Vulkan device, call real NVIDIA NGX, run a
real DLSS feature over a moving scene under ReShade with the add-on attached,
and walk through a scenario: display-mode changes, resizes, preset changes,
feature recreation, DLSS switched off and on, malformed parameter blocks. A run
takes seconds and ends in a verdict read from the add-on's own log.

If it is useful to you, you can help cover the AI tooling used in its
development:

[![Support on Ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/nigos)

## Requirements

- Windows, an NVIDIA GPU with DLSS, and a current driver.
- ReShade 6.x with add-on support, installed the normal way: `run.ps1` stages
  `C:\ProgramData\ReShade\ReShade64.dll` as `d3d11.dll` beside the D3D11 host;
  the Vulkan host uses ReShade's machine-wide Vulkan layer.
- To run: `nvngx_dlss.dll` 3.1.13 or newer in `snippets\` (from any game that
  ships DLSS). Optionally, beside it, a DLSS 5 neural rendering add-on
  (`renodx-dlss5*.addon64`) with its `nvngx_dlssnr.dll`; both are then staged
  and the add-on is the consumer under test. The add-on under test,
  `dlss5-bridge.addon64`, goes beside the scripts or in a sibling `ngxbridge\`
  checkout, or is passed with `-Addon`.
- To build: Visual Studio Build Tools with `cl.exe`, NVIDIA's DLSS SDK
  ([github.com/NVIDIA/DLSS](https://github.com/NVIDIA/DLSS)) as `dlss-sdk\` or
  in `NGX_SDK`, and the Vulkan SDK in `VULKAN_SDK` for the Vulkan host. Without
  the Vulkan SDK the D3D11 host still builds. Prebuilt hosts are attached to
  each release.

## Usage

```
.\build.cmd                          both hosts
.\suite.ps1                          every scenario, both backends
.\suite.ps1 -Fast -Background        a fraction of the frames, no window takes focus
.\suite.ps1 -Only vk -Repeat 4       one backend, four times each
.\suite.ps1 -Validate                Vulkan runs with the Khronos validation layer
.\run.ps1    -Scenario modes         one D3D11 scenario
.\run-vk.ps1 -Scenario display       one Vulkan scenario
.\run-vk.ps1 -Register               once, if the Vulkan layer never attaches
```

Every run stages a disposable folder under `run\` with the host, the add-on,
the snippet, a ReShade configuration and one trivial effect. Nothing outside it
is written, except `-Register`, which appends the Vulkan host to
`C:\ProgramData\ReShade\ReShadeApps.ini` with a timestamped backup.

`-Fast` divides every frame count by eight and covers contracts rather than
cadence; a scenario that depends on the wall clock opts out with `# nofast`.
`-Background` shows the host without activating it; exclusive fullscreen is
refused in that mode. `vault.ps1` keeps hashed copies of the third-party files
a run stages, recorded in `vault.tsv`, so an A/B between two builds of a
neighbour add-on is one command: `.\vault.ps1 -Use <label> -To <folder>`.

### Under Proton

Run there by a reporter on Linux (dlss5-bridge #22), whose notes this section
follows. The D3D11 host is a plain Windows program and runs under umu/Proton;
`run.ps1` runs under PowerShell for Linux (`pwsh`). The release binaries from
v1.1.0 on carry every verb below; v1.0.0 predates several. ReShade goes in as
`dxgi.dll` with a DLL override, the way it is installed in a Proton game, and
the host is started through the launcher:

```
pwsh ./run.ps1 -Scenario synth-nodlss-modes -Launcher umu-run -Proxy dxgi \
     -Shade /path/to/ReShade64.dll -Addon /path/to/dlss5-bridge.addon64
```

`-Launcher` prefixes the host command and sets `WINEDLLOVERRIDES=dxgi=n,b`;
`-StageOnly` assembles the run folder and stops, for running the host by hand.
The verdict reads the same log lines as on Windows. The Vulkan host needs
ReShade's Vulkan layer inside the prefix and is not covered by the runner.

Three things differ under Proton:

- The driver's optical flow does not open there, so the substitute contract
  needs a `texMotionVectors` provider. With `-Launcher` the runner enables
  `ngxGym_mv.fx`, a zero-motion stub, alongside the probe.
- The host renders the scene at several hundred frames per second, and the
  substitute arms only after 10 s of quiet. Do not use `-Fast`, and raise the
  `frames` counts of a synth scenario (6000 per step is known to work) if a
  run ends before the arming line appears.
- Wine's built-in `d3dcompiler_47` rejects `[fastopt]`, which motion-estimation
  effects use. A native `d3dcompiler_47.dll` in the prefix with
  `WINEDLLOVERRIDES=d3dcompiler_47=n,b` compiles them; the stub above needs
  neither.

## Scenarios

A scenario is a text file of verbs in `scenarios\`, one per line:

| Verb | Effect |
| --- | --- |
| `frames N` | render N frames |
| `mode windowed\|borderless\|exclusive` | display mode |
| `resize W H` | output size |
| `preset 0..5` | DLSS quality; the render size follows. `5` is DLAA |
| `recreate` | create the feature again at an unchanged shape |
| `dlss on\|off` | stop and restart calling DLSS. Off, no feature is created on a mode change either |
| `nodlss` | never initialise NGX or create a feature: a game without DLSS. The textures are output-sized |
| `hdr on\|off` | HDR10: `R10G10B10A2`, PQ colour space, the scene PQ-encoded as nits up to 1000, IsHDR set. Off is the default float swapchain |
| `scrgb on\|off [nits] [g22]` | scRGB HDR: float swapchain, linear colour space, IsHDR set, the scene's brightest pixel at `nits` (640 by default); `g22` leaves the gamma-2.2 colour space on the swapchain, as a game that never sets one does. D3D11 host only |
| `sdr on\|off` | 8-bit SDR: `R8G8B8A8`, sRGB colour space. D3D11 host only |
| `depthcolor on\|off` | hand NGX the depth as an `R32_SFLOAT` colour image, as RTX Remix does. Vulkan host only |
| `pad N` | allocate every texture N rows taller than the contract, as a game padded for dynamic resolution does. D3D11 host only |
| `exposure on\|off` | supply an ExposureTexture |
| `transpose on\|off` | declare the contract the wrong way round |
| `stale on\|off` | evaluate with another feature's four scalars |
| `omit flags\|jitter\|mvscale\|quality` | leave a key out of the block |

Directives in comments:

| Line | Effect |
| --- | --- |
| `# expect: text` | the add-on's log must contain it. `# expect-d3d11:` and `# expect-vk:` for one backend |
| `# expect-after: A :: B` | some B after the last A. `# expect-after-d3d11:` and `# expect-after-vk:` for one backend |
| `# host-may-fail` | the host's own NGX evaluates may fail without failing the run; `-d3d11` / `-vk` for one backend. Alone on its line |
| `# cfg: key=value` | appended to the generated `dlss5-bridge.cfg` |
| `# nr: key=value` | written to the consumer's `[RenoDX.DLSS5]` settings for a reproducible neural profile |
| `# diagnostic-only` | run explicitly with the runner; excluded from the release suite (intentional failure reproducers and A/B experiments) |
| `# cfg-old-file` | leave the settings file unstamped, to test its replacement |
| `# nofast` | run in full under `-Fast` |
| `# d3d11-only` | the suite skips the scenario on the Vulkan host |
| `# vk-only` | the suite skips the scenario on the D3D11 host |
| `# proxy: d3d11\|dxgi\|d3d12` | the name ReShade is staged under for this scenario; `d3d12` is the arrangement of a D3D11 game that imports d3d12.dll |

A verb the parser accepts and the executor drops fails the run rather than
passing it.

`colourchart` replaces the moving scene with 16 stationary patches. The
`synth-colour` scenario uses known HDR10 luminances (1–1000 nits), saturated
BT.2020 primaries and mixed colours. Both runners invoke `check-colour.py`
(Python 3, standard library only) for this scenario: it checks the actual input
against those known values, then compares every output channel in nits.
Use `-NoConsumer` to isolate the bridge and DLSS. With the neural add-on present,
the check permits its intentional changes, but rejects broad gamut contamination.
`hash_out=2` enables the patch-centre readback; `hash_out=1` retains ordinary
means and hashes. Run with the display already in HDR mode.

The HDR10 DLSS-off scene is rendered in the back buffer's format on D3D11;
Vulkan explicitly PQ-encodes the scene before its present blit. Earlier versions
copied incompatible D3D11 formats and presented unencoded Vulkan values, so their
HDR log-only checks did not establish colour fidelity.
The generated ReShade configuration declares normal depth, matching the host's
own DLSS contract and shader, instead of inheriting ReShade's reversed-depth default.

`synth-colour-bg3*` are explicit diagnostic runs with the reported BG3 neural
profile. The default profile reproduces colour changes caused by neural Local
Tone; `-local0` isolates that control, while `-intensity0` checks the consumer's
colour codec without a requested neural change. These diagnostics are excluded
from the release suite: a configured artistic change is not a transport assertion.

## Verdict

A run passes when the host exits 0, the add-on registered with ReShade,
recorded no crash, built at least one feature, delivered or recorded at least
one frame, never stood down, and every `# expect` line holds. On Vulkan the
mirror also has to come back after every effect-runtime teardown, and the
host's own DLSS evaluates all have to succeed. Where the substitute contract
holds the session, a delivered-count line has to appear after it armed, and
the scenarios with teardowns require one after the last runtime ReShade
created.

The suite adds one check that measures the neighbour rather than the add-on:
`consumer` runs twice, with and without the DLSS 5 add-on staged, and the
add-on's own output hash must differ. It exists because a build of that add-on
reported active and wrote nothing, and nothing else here could tell.

A second one measures brightness: `brightness` builds the feature on the four
presentations a game has, plain float, scRGB HDR, HDR10 and 8-bit SDR, and the
add-on logs the mean of its output per channel 60 frames into each. With and
without the DLSS 5 add-on, per presentation, the luma ratio has to stay within
[0.5, 2]. A neural pass that overexposes an HDR frame fails here and nowhere
else. The HDR10 means are PQ code values, not nits; the ratio is still the
same direction.

## Layout

```
run.ps1, run-vk.ps1   stage a folder and run one scenario
suite.ps1             every scenario, both backends, one summary
vault.ps1, vault.tsv  copies and hashes of staged third-party files
scenarios\            the scenarios
src\                  the two hosts, one .cpp each, and the scene shaders
reshade-fx\           the effect that gives ReShade something to run
notes\                measurements worth keeping
```

## HDR processing placement (D3D11 diagnostic)

`scene-pre`, `scene-post` and `scene-ofa` separate scene-linear BT709 colour,
a 1000-nit shoulder, and a later opaque UI. All use full-resolution HDR10.
The first runs native DLAA before tone mapping; the second runs synthetic OFA
on the final display image; the third supplies scene colour/depth directly to
the bridge's experimental OFA entry point, with host DLSS disabled.

Build the third with `../ngxbridge/tests/build-scene.cmd` and pass its
`tests/scene-build/dlss5-bridge.addon64` to `run.ps1 -Addon`. It is excluded from
normal bridge builds. Run each scenario with and without `-NoConsumer`, always
`-Background`, preserving each run's `gym-display.bin` before the next run.
The capture add-on records packed HDR10 at `reshade_finish_effects`, before
Present. `check-placement.py` takes the four pre/post files, then optionally
the two OFA files; its docstring gives their order.

The assertions check plain transport and unchanged UI. Scene differences
between neural paths are reported, not declared errors solely because NR
changes lighting. These patch measurements do not establish temporal or
whole-image equivalence, nor validate a BG3/Vulkan integration.

## Offline shader inspection and Vulkan stage probe

`inspect-scene-shaders.py EXTRACTED_DIR OUTPUT_DIR --sdk-bin SDK/Bin` scans
extracted BSHD containers, validates SPIR-V with `spirv-val`, and records CRC32,
SHA256, entry points and descriptor reflection using `spirv-cross`. It emits a
`scene-probe.cfg` for fragment and compute modules. Extraction of game packages
is separate; the inspector never modifies game files. Fingerprints establish
shader identity, not runtime execution or colour/exposure semantics.

The bridge now includes the probe; `build-scene-probe.cmd` builds an optional
standalone wrapper around the same implementation for older bridge builds.
The probe logs matching Vulkan pipeline creation and up to four matching
fragment pass observations/compute dispatches, including whether a render pass is active.
It does not copy resources, wait on the GPU, change shaders or perform NR.
Remove the probe add-on and profile when the observation is complete.

`run-vk.ps1 -Scenario scene-probe -Background -Validate -SceneProbeProfile PROFILE`
exercises real graphics and compute pipelines. A matching profile must observe
the fragment draw inside a render pass and the compute dispatch outside it;
an unknown fingerprint must produce neither. The `probecompute` scenario verb
is deliberately rejected by the D3D11 host. Profiles are opt-in for Gym runs.
Add `-SceneProbeIntegrated` to stage only the profile, using the bridge's built-in
observer. Profile lines may include `CRC32 SET BINDING`. The diagnostic also
exercises an unused float image descriptor: direct update, copy invalidation,
and fresh direct update. `check-scene-probe.py MATCH_LOG INVALID_BINDING_LOG`
checks those observations and ensures they never certify scene semantics.

## Third-party licences and dependencies

`reshade-fx\ReShade.fxh` is from the ReShade shader repository, CC0. The NGX
headers and library come from NVIDIA's DLSS SDK under its own licence and are
not included. Everything else is MIT.
