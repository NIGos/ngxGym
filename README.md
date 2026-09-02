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

Not tested here; written for a reporter on Linux, and reports are welcome. The
D3D11 host is a plain Windows program and runs under umu/Proton; `run.ps1`
runs under PowerShell for Linux (`pwsh`). ReShade goes in as `dxgi.dll` with a
DLL override, the way it is installed in a Proton game, and the host is
started through the launcher:

```
pwsh ./run.ps1 -Scenario synth-nodlss-modes -Launcher umu-run -Proxy dxgi \
     -Shade /path/to/ReShade64.dll -Addon /path/to/dlss5-bridge.addon64
```

`-Launcher` prefixes the host command and sets `WINEDLLOVERRIDES=dxgi=n,b`;
`-StageOnly` assembles the run folder and stops, for running the host by hand.
The verdict reads the same log lines as on Windows. The Vulkan host needs
ReShade's Vulkan layer inside the prefix and is not covered by the runner.

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
| `nodlss` | never initialise NGX or create a feature: a game without DLSS. Render sizes then follow the published DLSS ratios |
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
| `# host-may-fail` | the host's own NGX evaluates may fail without failing the run; `-d3d11` / `-vk` for one backend |
| `# cfg: key=value` | appended to the generated `dlss5-bridge.cfg` |
| `# cfg-old-file` | leave the settings file unstamped, to test its replacement |
| `# nofast` | run in full under `-Fast` |
| `# d3d11-only` | the suite skips the scenario on the Vulkan host |
| `# vk-only` | the suite skips the scenario on the D3D11 host |
| `# proxy: d3d11\|dxgi\|d3d12` | the name ReShade is staged under for this scenario; `d3d12` is the arrangement of a D3D11 game that imports d3d12.dll |

A verb the parser accepts and the executor drops fails the run rather than
passing it.

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

## Third-party

`reshade-fx\ReShade.fxh` is from the ReShade shader repository, CC0. The NGX
headers and library come from NVIDIA's DLSS SDK under its own licence and are
not included. Everything else is MIT.
