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

The last one is the point: this measures the add-on, and it can be wrong too.
Anything it reports is worth confirming against the log before acting on it.
