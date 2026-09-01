# renodx-dlss5: the newer build needs ReShade's proxy on the bridge's D3D12 device

Written 2026-09-02 from ngxGym. Everything below is reproducible in about
thirty seconds with no game running.

## Symptom

The add-on's own panel reports NR active. The picture is unchanged. Toggling NR
off and on does not recover it.

## Which build

Two builds are in circulation declaring the same file version `0.2026.0828.0517`:

| bytes | sha256 (first 12) | behaviour under dlss5-bridge 1.3.0 |
| --- | --- | --- |
| 1,703,424 | `245C06137AD1` | NR changes the picture |
| 1,732,608 | `D5ADF82EB44B` | NR changes nothing |

Both are kept in this repository's vault; `vault.tsv` records them.

## Measurement

ngxGym is a synthetic DLSS host: it creates a real D3D11 or Vulkan device, calls
real NGX, and runs under ReShade with `dlss5-bridge`, which mirrors the contract
onto a private D3D12 device where a DLSS 5 add-on can insert itself. The bridge
reads its D3D12 output texture back once per session and hashes it, so "did the
neighbour change the picture" is a number.

The bridge has a key, `unwrap`, that decides whether NGX is handed the D3D12
device underneath ReShade's proxy (`1`, the shipped default) or the proxy itself
(`0`). Scenario `consumer`, 120 frames, D3D11 host:

| DLSS 5 add-on | unwrap | output hash | NR evaluations | pool exhausted | tracker installed |
| --- | --- | --- | --- | --- | --- |
| none staged | 1 | `169B9C2388DA78D3` | 0 | 0 | — |
| none staged | 0 | `169B9C2388DA78D3` | 0 | 0 | — |
| 1,703,424 | 1 | `07D332781681655C` | 2 | 0 | no |
| 1,703,424 | 0 | `07D332781681655C` | 2 | 0 | no |
| 1,732,608 | 1 | `169B9C2388DA78D3` | 1 | 1 | no |
| 1,732,608 | 0 | `4B12AE0C717764C4` | 2 | 0 | **yes** |

Same on the Vulkan host, scenario `omissions`: with `unwrap=1` the newer build
evaluates once and exhausts; with `unwrap=0` it installs the tracker, evaluates
every frame and never exhausts.

The instrument was validated first: changing the bridge's create flags from
`-1` to `108` moves the hash, so a hash that does not move means the output did
not, rather than that nothing was looked at.

## Cause, from the add-on's own log lines

Present with the newer build:

    GPU-safe NR workset pool active: up to 4 scratch generations; exact queue
    fences recycle only completed submissions; ExecuteCommandLists fast path
    stays lock-free when no NR work awaits submission

and, with the proxy stripped, on every evaluation:

    NR workset pool exhausted; preserving game output for this evaluation

With the proxy kept, once at session open:

    native D3D12 queue submission tracker installed for GPU-safe NR workset recycling

The pool recycles a scratch generation only when it has seen the submission that
used it retire, and it learns about submissions from the queue ReShade announces.
The bridge stripped ReShade's proxy from its private device before NGX ever saw
it (a workaround for a D3D11-only add-on that faulted on D3D12 pipeline events),
so every command list the newer build recorded went down a queue the tracker was
never attached to: the first evaluation took a generation, nothing ever returned
it, and the pool failed closed from the second evaluation on. The older build has
no pool and works either way.

## What the bridge does about it

From 1.4.0 the bridge identifies the DLSS 5 add-on beside it by SHA-256 and, for
a build measured to need the proxy, keeps it regardless of `unwrap=1` in the
file, logging one line:

    [bridge] keeping ReShade's proxy on the D3D12 device: the DLSS 5 add-on
    beside this one is a build measured to need the queue it announces.

For a build not on the list, the log names `unwrap=0` as the first thing to try
when the panel says active and the picture does not change.

## What was tried from the bridge's side first, and did not help

- **A full CPU drain of the private D3D12 queue after every evaluate.** Same
  hash, same single evaluation, same exhaustion. The pool does not watch the
  fence; it watches the queue ReShade announces.
- **`EnableHooks=1`** in `[RenoDX.DLSS5]`. No difference. That setting governs
  Streamline hooks, not this.

## Reproducing it

    .\vault.ps1 -Use <sha> -To <run folder>
    .\suite.ps1 -Fast -Only d3d11

The suite's `consumer` check runs the scenario twice, with the add-on staged and
without, and fails when the two hashes match.
