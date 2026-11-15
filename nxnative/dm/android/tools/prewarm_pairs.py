#!/usr/bin/env python3
"""Make a Vulkan shader-pair warmup manifest from the source pairs and DMSV."""
import argparse
from collections import Counter
from functools import lru_cache
from pathlib import Path
import struct

from dmsv import Bundle


def io_signature(data, stage, storage, locations):
    types, pointers, variables, decorations, members = {}, {}, [], {}, {}
    words = struct.unpack(f'<{len(data) // 4}I', data)
    if words[0] != 0x07230203:
        raise ValueError('invalid SPIR-V magic')
    offset = 5
    models = set()
    while offset < len(words):
        size, op = divmod(words[offset], 65536)
        if not size or offset + size > len(words):
            raise ValueError('invalid SPIR-V instruction')
        if op in (15, 21, 22, 23, 30, 32, 59, 71, 72):
            args = words[offset + 1:offset + size]
            if op == 15:
                models.add(args[0])
            elif op in (21, 22, 23, 30):
                types[args[0]] = (op, *args[1:])
            elif op == 32 and args[1] == storage:
                pointers[args[0]] = args[2]
            elif op == 59 and args[2] == storage:
                variables.append(args[:2])
            elif op == 71 and len(args) >= 3:
                decorations.setdefault(args[0], {})[args[1]] = args[2:]
            elif op == 72 and len(args) >= 4:
                members.setdefault(args[:2], {})[args[2]] = args[3:]
        offset += size
    if models != {stage}:
        raise ValueError('unexpected SPIR-V execution model')

    signature, depth = 0, False
    for pointer, ident in variables:
        dec = decorations.get(ident, {})
        if 11 in dec:  # BuiltIn
            depth |= dec[11][0] == 22  # FragDepth
            continue
        type_id = pointers[pointer]
        op, *args = types.get(type_id, (0,))
        if op == 30 and all(11 in members.get((type_id, i), {}) for i in range(len(args))):
            depth |= any(members[type_id, i][11][0] == 22 for i in range(len(args)))
            continue
        if op == 23:
            op, *args = types[args[0]]
        if op not in (21, 22) or args[0] != 32:
            raise ValueError('unsupported shader I/O type')
        if 30 not in dec or dec.get(32, (0,)) != (0,):
            raise ValueError('shader I/O missing location or uses a dual-source index')
        loc = dec[30][0]
        if loc >= locations:
            raise ValueError('shader I/O exceeds host location limit')
        kind = 1 if op == 22 else 2 + args[1]  # float, uint, sint
        prior = (signature >> (2 * loc)) & 3
        if prior and prior != kind:
            raise ValueError('mixed numeric classes at one shader I/O location')
        signature |= kind << (2 * loc)
    return signature, depth


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bundle', type=Path)
    parser.add_argument('pairs', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    pairs = sorted({tuple(int(key, 16) for key in line.split())
                    for line in args.pairs.read_text().splitlines()
                    if line.strip() and not line.startswith('#')})
    counts = Counter()
    records = []
    with Bundle(args.bundle) as bundle:
        entries = {entry[0]: entry for entry in bundle.entries}

        @lru_cache(maxsize=None)
        def signature(key, stage):
            entry = entries.get(key)
            if not entry:
                raise ValueError('shader key missing from bundle')
            if entry[1] != stage:
                raise ValueError('unexpected bundle stage')
            return io_signature(bundle.spv(entry), 0 if stage == 0 else 4,
                                1 if stage == 0 else 3, 16 if stage == 0 else 8)

        for vertex, fragment in pairs:
            try:
                inputs, _ = signature(vertex, 0)
                colors, depth = signature(fragment, 1)
            except ValueError as err:
                counts[str(err)] += 1
                print(f'{vertex:016x} {fragment:016x}: {err}')
                continue
            records.append(struct.pack('<QQII', vertex, fragment, inputs, colors | (depth << 16)))
    args.output.write_bytes(struct.pack('<4sI', b'PWA1', len(records)) + b''.join(records))
    print(f'{len(records)}/{len(pairs)} pairs, {len(pairs) - len(records)} skipped: {dict(counts)}')


if __name__ == '__main__':
    main()
