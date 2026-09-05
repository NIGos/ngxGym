"""Check synth-colour's actual HDR10 input and output, in nits, not PQ averages.

Usage: python check-colour.py run/synth-colour/dlss5-bridge.log [--consumer]
No consumer: transport + DLSS should preserve stationary patch centres.
With consumer: allow intentional neural changes, but reject a washed-out gamut.
"""
import math
import re
import sys
from pathlib import Path

PATCHES = [(v, v, v) for v in (1, 10, 100, 203, 400, 1000, 0, 50)] + [
    (203, 0, 0), (0, 203, 0), (0, 0, 203), (203, 203, 0),
    (0, 203, 203), (203, 0, 203), (150, 80, 40), (20, 120, 180)]


def nits(pq):
    p = max(pq, 0) ** (1 / 78.84375)
    return (max(p - .8359375, 0) / (18.8515625 - 18.6875 * p)) ** (1 / .1593017578125) * 10000


def check(path, consumer=False):
    text = Path(path).read_text(errors='replace')
    pattern = r'(input|output) patch (\d+) after evaluate (\d+): R=([\d.e+-]+) G=([\d.e+-]+) B=([\d.e+-]+) \(([^)]+)\)'
    readings = {}
    for side, index, frame, r, g, b, fmt in re.findall(pattern, text):
        if fmt != 'R10G10B10A2_UNORM':
            raise AssertionError(f'Expected HDR10 R10G10B10A2, received {fmt}')
        key = (side, int(index))
        if key in readings:
            raise AssertionError('Use a single-build chart run; duplicate patch reading')
        readings[key] = tuple(nits(float(x)) for x in (r, g, b))
    if len(readings) != 32:
        raise AssertionError(f'Expected 16 input + 16 output patches, got {len(readings)}')
    errors = []
    worst = 0
    for i, expected in enumerate(PATCHES):
        before, after = readings['input', i], readings['output', i]
        for channel, (target, src, dst) in enumerate(zip(expected, before, after)):
            if not all(math.isfinite(x) for x in (src, dst)):
                errors.append(f'patch {i} channel {channel}: non-finite value')
            # Includes 10-bit PQ quantisation and fp16 in the Vulkan host.
            if abs(src - target) > max(.02, target * .012):
                errors.append(f'patch {i} input {src:.3f} != known {target:.3f} nits')
            tolerance = max(5 if consumer else .12, src * (.30 if consumer else .015))
            worst = max(worst, abs(dst - src))
            if abs(dst - src) > tolerance:
                errors.append(f'patch {i} channel {channel}: {src:.3f} -> {dst:.3f} nits (limit {tolerance:.3f})')
    if errors:
        raise AssertionError('\n'.join(errors))
    print(f'PASS: 16 known HDR10 patches, worst channel change {worst:.3f} nits; consumer={consumer}')


if __name__ == '__main__':
    try:
        check(sys.argv[1], '--consumer' in sys.argv[2:])
    except (AssertionError, OSError, IndexError) as e:
        print(f'FAIL: {e}')
        sys.exit(1)
