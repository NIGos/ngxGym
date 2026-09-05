"""Compare raw final HDR frames for scene-pre/scene-post (all paths explicit).

Usage: python check-placement.py pre-plain.bin post-plain.bin pre-nr.bin post-nr.bin [ofa-plain.bin ofa-nr.bin]
The reference for neural scene appearance is PRE-NR, not an unmodified scene.
UI must remain identical to the plain reference. This measures placement; it
does not claim an artistic neural scene should have zero change.
"""
import json
import math
import struct
import sys
from pathlib import Path


def nits(code):
    p = (code / 1023) ** (1 / 78.84375)
    return (max(p - .8359375, 0) / (18.8515625 - 18.6875 * p)) ** (1 / .1593017578125) * 10000


def read(path):
    data = Path(path).read_bytes()
    magic, w, h, fmt = struct.unpack_from('<4I', data)
    assert magic == 0x31584D47 and fmt == 24 and len(data) == 16 + w * h * 4
    lut = [nits(i) for i in range(1024)]
    points = [((i + .5) / 8, (j + .5) / 2) for j in range(2) for i in range(8)]
    points += [((i + .5) / 4, .9375) for i in range(4)]
    patches = []
    for u, v in points:
        channels = [0., 0., 0.]
        x, y = int(u * w), int(v * h)
        for yy in range(y - 8, y + 8):
            for xx in range(x - 8, x + 8):
                px, = struct.unpack_from('<I', data, 16 + (yy * w + xx) * 4)
                for c in range(3):
                    channels[c] += lut[(px >> (c * 10)) & 1023] / 256
        assert all(math.isfinite(c) for c in channels)
        patches.append(channels)
    return {'path': str(path), 'size': [w, h], 'patches_nits': patches}


def delta(a, b, start, end):
    return max(abs(x - y) for p, q in zip(a['patches_nits'][start:end], b['patches_nits'][start:end])
               for x, y in zip(p, q))


if __name__ == '__main__':
    try:
        frames = [read(p) for p in sys.argv[1:]]
        assert len(frames) in (4, 6) and all(f['size'] == frames[0]['size'] for f in frames)
        pre, post, pre_nr, post_nr = frames[:4]
        result = {
            'plain_pre_vs_post_max_nits': delta(pre, post, 0, 20),
            'pre_nr_ui_max_change_nits': delta(pre, pre_nr, 16, 20),
            'post_nr_ui_max_change_nits': delta(post, post_nr, 16, 20),
            'pre_vs_post_nr_scene_max_nits': delta(pre_nr, post_nr, 0, 16),
            'frames': frames,
        }
        if len(frames) == 6:
            ofa, ofa_nr = frames[4:]
            result.update({
                'ofa_plain_vs_native_max_nits': delta(pre, ofa, 0, 20),
                'ofa_nr_ui_max_change_nits': delta(ofa, ofa_nr, 16, 20),
                'ofa_nr_vs_native_scene_max_nits': delta(pre_nr, ofa_nr, 0, 16),
            })
        print(json.dumps(result, indent=2))
        assert result['plain_pre_vs_post_max_nits'] <= .5, 'Plain transport differs: placement comparison invalid'
        assert result['pre_nr_ui_max_change_nits'] <= .02, 'UI changed before-tone-map reference'
        if len(frames) == 6:
            assert result['ofa_plain_vs_native_max_nits'] <= .5, 'OFA plain transport differs'
            assert result['ofa_nr_ui_max_change_nits'] <= .02, 'OFA scene provider changed UI'
    except (AssertionError, OSError, struct.error) as error:
        print('FAIL:', error, file=sys.stderr)
        sys.exit(1)
