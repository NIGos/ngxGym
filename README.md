# ngxGym

Two small games that are not games. They create a real D3D11 or Vulkan device,
call real NVIDIA NGX, run a real DLSS feature over a moving scene, and put the
upscaled frame on screen -- and they do it under ReShade with the
`dlss5-bridge` add-on attached, so the add-on sees exactly what it sees in a
shipped title.

They exist because the alternative was launching Baldur's Gate 3 or Red Dead
Redemption 2 to test a change, waiting through a loader and a menu, toggling a
display setting by hand, and reading a log. One scenario here takes about ten
seconds and says pass or fail.

## What it needs

- Visual Studio Build Tools with `cl.exe` (the path is at the top of `build.cmd`)
- NVIDIA's DLSS SDK, for `nvsdk_ngx_s.lib` and the headers (path also in `build.cmd`)
- The Vulkan SDK, for `glslc` and the Khronos validation layer. Without it the
  D3D11 half still builds and the Vulkan half is skipped with a message.
- ReShade installed: as `d3d11.dll` beside the executable for the D3D11 host
  (`run.ps1` stages it), and as the machine-wide Vulkan layer for the other.
- An NVIDIA GPU with DLSS. The hosts refuse with a reason if NGX says otherwise.

## Running

```
    .uild.cmd                          both hosts
    .\suite.ps1                          every scenario on both backends
    .\suite.ps1 -Fast                    the same coverage on a fraction of the frames
    .\suite.ps1 -Fast -Background        and without taking focus
    .\suite.ps1 -Only vk -Repeat 4       one backend, four times each
    .\suite.ps1 -Validate                Vulkan runs also enable the Khronos layer

    .un.ps1    -Scenario modes         one D3D11 scenario
    .un-vk.ps1 -Scenario display       one Vulkan scenario
    .un-vk.ps1 -Register               once, if the Vulkan layer never attaches
```

Every run stages a disposable folder under `run\` holding the host, the add-on,
a DLSS snippet, a ReShade configuration and one trivial effect. Nothing outside
that folder is written, with one exception: `-Register` appends the Vulkan
host's path to `C:\ProgramData\ReShade\ReShadeApps.ini`, keeping a timestamped
backup. That file has been measured NOT to gate attachment on this machine, so
the switch is there for a machine where it does.

## Scenarios

A scenario is a text file of verbs, one per line, in `scenarios\`:

```
    frames 300          render this many frames
    mode windowed|borderless|exclusive
    resize 2560 1440    change the output size
    preset 0..5         DLSS quality; the render size follows
    recreate            build the feature again at an unchanged shape
    dlss on|off         stop and restart calling DLSS
    hdr on|off          swapchain colour space, and the IsHDR create flag
    transpose on|off    declare the contract the wrong way round
    omit flags|jitter|mvscale|quality      leave a key out of the block
    exposure on|off     supply an ExposureTexture
```

A line starting `# nofast` opts the scenario out of `-Fast`. Use it when the
behaviour under test is driven by the wall clock rather than by the step count:
`dlss-off` waits five seconds and thirty presents for the source latch, and a
fraction of the frames is a fraction of the seconds.

A line starting `# cfg:` is appended to the generated `dlss5-bridge.cfg` for
that run. It exists because a behaviour gated behind a key would otherwise be
untestable: `dlss-off` needs `synth=1` to reach the source-latch release, and
that key is off by default on purpose.

The generated file is stamped `# dlss5-bridge keep` on its first line, because
the add-on replaces a settings file from another version with its defaults at
attach and that would drop every `# cfg:` line. A line starting `# cfg-old-file`
leaves the stamp off, which is how `regen` tests the replacement itself.

A line `nodlss` makes the host create no NGX feature at all: a game that never
had DLSS, which is the substitute contract's pre-arm case. With `dlss off` the
host also creates no feature on a mode change, as a game with DLSS switched off
does not; the bridge counts creates, and one there reads as the game still
having DLSS.

**The substitute contract arms in this gym**, on both backends, since
2026-09-02. Two things had to be true that were not. ReShade's generic depth
add-on ignores a frame in which the only depth-stencil received eight draw calls
or fewer, and skips a depth-stencil that drew three vertices or fewer; the host
drew one fullscreen triangle, so no depth buffer was ever selected and the DEPTH
semantic never bound. The host now draws that triangle nine times. And the
contract is DLAA at back-buffer size, so a scenario that wants it starts with
`preset 5`. `synth-modes` and `synth-nodlss-modes` are the two cases across
display-mode changes; with the Vulkan optical-flow teardown of 1.4.0 put back,
`synth-modes` fails on Vulkan the way Baldur's Gate 3 did.

A line starting `# expect:` names something the add-on's log must contain for
the run to pass. `# expect-d3d11:` and `# expect-vk:` do the same for one
backend, which is what the two logs word differently.

A step the parser accepts and the executor drops is a run that proves something
other than what its file says -- so both hosts fail loudly on one rather than
ignoring it. That happened once, with `exposure`, and the scenario reported
PASS having tested nothing.

## What a verdict means

Passing needs all of: the host exits 0, the add-on wrote a log, it registered
with ReShade, it recorded no crash, it built at least one feature, it delivered
or recorded at least one frame, it never stood down, and every `# expect:` line
is in the log. On Vulkan it also needs the mirror to have come back after every
effect-runtime teardown -- ReShade destroys and recreates its runtime on any
swapchain change, and a mirror that dies at the first resize used to pass.

Every run also reports what the DLSS 5 add-on did with the contract, read from
its own lines in `ReShade.log`. Creating its NR feature and evaluating it at
least once is a gate: the bridge's own verdict proves it *built* a contract and
delivered frames, and cannot prove anybody consumed them — "the bridge is fine
and the picture is unchanged" is the report that costs the most time to triage.
The workset pool running out afterwards is counted and **not** failed: it
happens on every run of every scenario on both backends, and identically under a
1.3.0 bridge built from its own tag, so it is the neighbour add-on's own state.

`-Fast` divides every `frames N` by 8, with a floor that never makes a scenario
longer than it was. It takes the full suite from about twelve minutes to about
three. It covers the contracts and not the cadence, so a latch or timing
regression is outside it — the summary line says so on every fast run.

**The consumer check** is the one test here that measures a NEIGHBOUR rather than
this project. Everything else proves the bridge built a contract and delivered
frames; none of it can tell a DLSS 5 add-on that rewrote the output from one that
attached, logged *active*, and wrote nothing. So `consumer` is run twice, with
the add-on staged and without, and the bridge’s own output hash must differ.
Measured 2026-09-01 at scale 8:

| DLSS 5 add-on | output hash |
| --- | --- |
| none | `D59F57A020169204` |
| renodx 1,703,424 bytes | `C566EBC647461E4B` |
| renodx 1,732,608 bytes | `D59F57A020169204` — identical to no add-on at all |

The instrument was validated before it was trusted: `flags=108` against
`flags=-1` moves the hash, so a hash that does not move means the output did not,
rather than that nothing was looked at. The readback is behind `hash_out=1` in
the add-on’s own config and off everywhere else.

`vault.ps1` keeps a copy of every file this suite stages that this project does
not build — the DLSS 5 add-on, the NGX snippets — hashed and recorded in
`vault.tsv`. It exists because two builds of one add-on, 29 KB apart and both
declaring the same version, cost an afternoon: nothing on disk recorded that two
existed. `-Use` deploys one for an A/B. The binaries are not tracked by git; the
manifest is.

`-Background` shows the window without activating it and sends it to the back, so
a suite can run while somebody works. Not minimised — a minimised window has a
0x0 client area and the Vulkan half correctly refuses to build a swapchain for
one. Exclusive fullscreen is refused in that mode and says so.

`-Validate` adds the Khronos validation layer on the Vulkan side. Its report
separates VUIDs with a known owner from new ones. Four are known: two belong to
ReShade's layer, attributed by running with
`VK_LOADER_LAYERS_DISABLE=VK_LAYER_reshade`, which is clean; two are the
add-on's frame park, which cannot be expressed without them.

## What it has caught

- A 0xC0000005 at Vulkan teardown, 4 runs out of 4, traced to a ReShade export
  called after the Vulkan loader had unloaded ReShade64.dll.
- A parameter block left naming four freed D3D12 resources across a rebuild,
  which faulted inside another add-on's CreateFeature detour about one rebuild
  in three.
- The Vulkan mirror retiring itself for the session on the first display change,
  silently, which is a defect that had been reported from a game and not
  reproduced.
- An imported memory type chosen from the wrong set, accepted by this driver and
  not by the specification.
- Motion vectors wrong by a factor of about 128000, in the host itself, after
  two commits had called them correct by construction.
- A reported regression that was not one: a DLSS 5 add-on that attaches, says it
  is active and writes nothing. Reproduced here with no game and nothing toggled,
  identically under a bridge built from the 1.3.0 tag, and then pinned to one of
  two builds of that add-on which declare the same version and differ by 29 KB.

The last one is the point: this measures the add-on, and it can be wrong too.
Anything it reports is worth confirming against the log before acting on it.
