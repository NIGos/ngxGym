"""Check the real Vulkan scene-probe stimulus.

Usage: python check-scene-probe.py MATCH_LOG INVALID_BINDING_LOG
The host writes a descriptor, copies it, then writes it directly again. Its
float image is deliberately unused by the shader and must never be certified.
"""
import re
import sys
from pathlib import Path


def check(matched, invalid):
    records = re.findall(r'source_metadata=([^\r\n]+)', matched)
    assert len(records) == 4
    assert '1280x720' in records[0] and records[1] == 'unavailable'
    assert all('1280x720' in r for r in records[2:])
    assert matched.count('contract=unverified') == 4
    assert 'dispatch compute=1A84E242 in_render_pass=0' in matched
    assert matched.count('table_bound_before_pass=1') == 4
    assert invalid.count('source_metadata=unavailable') == 4
    assert 'source_metadata=1280x720' not in invalid
    print('PASS: real descriptor lifecycle, invalid binding and no false certification')


if __name__ == '__main__':
    check(*(Path(p).read_text(encoding='utf-8') for p in sys.argv[1:]))
