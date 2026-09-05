"""Inspect extracted BSHD files offline; never modify the package or game.

Uses installed SPIRV-Tools/SPIRV-Cross to validate and reflect candidate modules.
CRC32 is a lookup fingerprint, not proof that a shader executes at runtime.
Usage: python inspect-scene-shaders.py EXTRACTED_DIR OUTPUT_DIR --sdk-bin SDK/Bin
"""
import argparse
import hashlib
import json
import struct
import subprocess
import zlib
from pathlib import Path


def modules(data):
    """Candidate module boundaries; spirv-val is the authoritative validator."""
    start = 0
    while (start := data.find(b'\x03\x02\x23\x07', start)) >= 0:
        end = start + 20
        if end <= len(data):
            while end + 4 <= len(data):
                count = struct.unpack_from('<I', data, end)[0] >> 16
                if not count or end + count * 4 > len(data):
                    break
                end += count * 4
            yield start, data[start:end]
        start += 4


def run(root, output, sdk):
    output.mkdir(parents=True, exist_ok=True)
    records, fragments = [], {}
    for path in sorted(root.rglob('*.bshd')):
        data = path.read_bytes()
        record = {'file': str(path.relative_to(root)),
                  'sha256': hashlib.sha256(data).hexdigest(), 'modules': []}
        # DXBC containers carry their own exact length. Cross-check their CRCs
        # against known source profiles rather than hashing the BSHD envelope.
        pos = 0
        while (pos := data.find(b'DXBC', pos)) >= 0:
            if pos + 32 <= len(data):
                length, chunks = struct.unpack_from('<II', data, pos + 24)
                if 32 + 4 * chunks <= length <= len(data) - pos:
                    record['modules'].append({'kind': 'DXBC', 'offset': pos,
                        'bytes': length, 'crc32': f'{zlib.crc32(data[pos:pos+length]):08X}'})
            pos += 4
        for offset, module in modules(data):
            crc = f'{zlib.crc32(module):08X}'
            dest = output / f'{path.stem}-{crc}.spv'
            dest.write_bytes(module)
            subprocess.run([str(sdk / 'spirv-val.exe'), str(dest)], check=True, capture_output=True)
            reflected = subprocess.run([str(sdk / 'spirv-cross.exe'), str(dest), '--reflect'],
                                       check=True, capture_output=True, text=True)
            reflection = json.loads(reflected.stdout)
            record['modules'].append({'kind': 'SPIR-V', 'offset': offset,
                'bytes': len(module), 'crc32': crc,
                'sha256': hashlib.sha256(module).hexdigest(), 'reflection': reflection})
            if any(e['mode'] in ('frag', 'comp') for e in reflection['entryPoints']):
                images = [i for i in reflection.get('separate_images', []) if i['type'] == 'texture2D']
                # A sole 2D input is useful for observation, not proof of scene
                # colour semantics. Ambiguous inputs leave the binding unset.
                fragments[crc] = f"{crc} {images[0]['set']} {images[0]['binding']}" if len(images) == 1 else crc
        records.append(record)
    if not records or not fragments:
        raise ValueError('No validated fragment/compute modules found')
    (output / 'manifest.json').write_text(json.dumps(records, indent=2), encoding='utf-8')
    (output / 'scene-probe.cfg').write_text('# Validated fragment/compute modules; execution not yet observed\n' +
                                         '\n'.join(fragments[k] for k in sorted(fragments)) + '\n', encoding='utf-8')
    print(f'{len(records)} containers inspected; {len(fragments)} fragment/compute fingerprints')


if __name__ == '__main__':
    args = argparse.ArgumentParser(description=__doc__)
    args.add_argument('extracted', type=Path)
    args.add_argument('output', type=Path)
    args.add_argument('--sdk-bin', required=True, type=Path)
    options = args.parse_args()
    run(options.extracted, options.output, options.sdk_bin)
