# renodx-dlss5: NR writes nothing when the DLSS host is not native D3D12

Written 2026-09-02 from ngxGym. Everything below is reproducible in about
thirty seconds with no game running.

## Symptom

The add-on's own panel reports NR active. The picture is unchanged. Toggling NR
off and on does not recover it.

## Which build

Two builds are in circulation declaring the same file version `0.2026.0828.0517`:

| bytes | sha256 (first 12) | behaviour |
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

Scenario `consumer`, 120 frames, D3D11 host:

| DLSS 5 add-on | output hash | NR evaluations | pool exhausted |
| --- | --- | --- | --- |
| none staged | `D59F57A020169204` | 0 | 0 |
| 1,703,424 | `07D332781681655C` | 2 | 0 |
| 1,732,608 | `D59F57A020169204` | 1 | 1 |

The newer build's output is byte-identical to having no DLSS 5 add-on installed
at all.

The instrument was validated first: changing the bridge's create flags from
`-1` to `108` moves the hash, so a hash that does not move means the output did
not, rather than that nothing was looked at.

## Cause, from the add-on's own log lines

Present with the newer build:

    GPU-safe NR workset pool active: up to 4 scratch generations; exact queue
    fences recycle only completed submissions; ExecuteCommandLists fast path
    stays lock-free when no NR work awaits submission
    NR workset pool exhausted; preserving game output for this evaluation

Absent with the older build, which never prints either.

The binary carries exactly two messages about the tracker that pool depends on:

    native D3D12 queue submission tracker installed for GPU-safe NR workset recycling
    failed to install native D3D12 queue submission tracker; NR pool will fail
    closed when all worksets are busy

**Neither appears.** Not the success, not the failure. The install is not
attempted, so the pool fails closed on its first evaluation and every one after.

The likely reason is in the name: the host game here is D3D11 or Vulkan, and the
only D3D12 device and queue in the process belong to another ReShade add-on
rather than to the game. There is no native D3D12 queue to track.

## What was tried from the bridge's side, and did not help

- **A full CPU drain of the private D3D12 queue after every evaluate.** The
  transport normally signals a shared fence and lets the game's queue wait on it
  on the GPU, so nothing outside observes a submission retire at a point the CPU
  can see. Draining it made no difference: same hash, same single evaluation,
  same exhaustion.
- **`EnableHooks=1`** in `[RenoDX.DLSS5]`. No difference, and the tracker still
  never appears. That setting governs Streamline hooks, not this.

## Reproducing it

    .\vault.ps1 -Use <sha> -To <run folder>
    .\suite.ps1 -Fast -Only d3d11

The suite's `consumer` check runs the scenario twice, with the add-on staged and
without, and fails when the two hashes match.
