#!/usr/bin/env python3
"""Merges Xenia shader (.xsh) or pipeline (.xpso) storage files.

Storage records what a title used - guest shader microcode and pipeline
descriptions - not anything about the host GPU, so one machine's files warm
up another's: at startup Xenia translates and creates everything stored,
instead of while the game draws, when an unready pipeline skips its draw for
a frame or more. The host-specific VkPipelineCache (shaders/local/*.vk.bin)
isn't merged; the driver rebuilds it from what this stores.

  merge_shader_storage.py OUTPUT INPUT [INPUT...]

INPUTs must have the same header (format version, API, render target path),
which their names give: <title id>.xsh, <title id>.<fbo|fsi>.vk.xpso. Entries
keep the first input's order, then each next input's new ones. An entry whose
hash doesn't match its data ends that file, as when Xenia loads it.
"""

import ctypes
import struct
import sys

try:
    from xxhash import xxh3_64_intdigest
except ImportError:
    # The xxHash library most distributions have, without the Python module.
    _libxxhash = ctypes.CDLL('libxxhash.so.0')
    _libxxhash.XXH3_64bits.restype = ctypes.c_uint64
    _libxxhash.XXH3_64bits.argtypes = [ctypes.c_char_p, ctypes.c_size_t]

    def xxh3_64_intdigest(data):
        return _libxxhash.XXH3_64bits(bytes(data), len(data))

SHADER_MAGIC = 0x48534558  # 'XESH'
PIPELINE_MAGIC = 0x53504558  # 'XEPS'
SHADER_ENTRY_HEADER = struct.Struct('<QI')  # ucode hash, dword count | type << 31
PIPELINE_HEADER_SIZE = 12  # magic, API magic, version
PIPELINE_HASH = struct.Struct('<Q')


def read_shader_storage(path):
    data = open(path, 'rb').read()
    header, entries, offset = data[:8], [], 8
    if struct.unpack_from('<I', header)[0] != SHADER_MAGIC:
        raise ValueError(f'{path} is not shader storage')
    while offset + SHADER_ENTRY_HEADER.size <= len(data):
        ucode_hash, count_and_type = SHADER_ENTRY_HEADER.unpack_from(data, offset)
        end = offset + SHADER_ENTRY_HEADER.size + (count_and_type & 0x7FFFFFFF) * 4
        ucode = data[offset + SHADER_ENTRY_HEADER.size:end]
        if end > len(data) or xxh3_64_intdigest(ucode) != ucode_hash:
            break
        entries.append((ucode_hash, data[offset:end]))
        offset = end
    return header, entries


def read_pipeline_storage(path, record_size):
    data = open(path, 'rb').read()
    header, entries = data[:PIPELINE_HEADER_SIZE], []
    if struct.unpack_from('<I', header)[0] != PIPELINE_MAGIC:
        raise ValueError(f'{path} is not pipeline storage')
    for offset in range(PIPELINE_HEADER_SIZE, len(data) - record_size + 1, record_size):
        record = data[offset:offset + record_size]
        description_hash = PIPELINE_HASH.unpack_from(record)[0]
        if xxh3_64_intdigest(record[PIPELINE_HASH.size:]) != description_hash:
            break
        entries.append((description_hash, record))
    return header, entries


def pipeline_record_size(paths):
    # Every file holds whole records after the header; the record size is the
    # one that also makes each record's hash match its description.
    lengths = [len(open(p, 'rb').read()) - PIPELINE_HEADER_SIZE for p in paths]
    for size in range(PIPELINE_HASH.size + 8, 4097):
        if all(n % size == 0 for n in lengths if n):
            data = open(paths[0], 'rb').read()
            if len(data) <= PIPELINE_HEADER_SIZE:
                continue
            record = data[PIPELINE_HEADER_SIZE:PIPELINE_HEADER_SIZE + size]
            if xxh3_64_intdigest(record[PIPELINE_HASH.size:]) == \
                    PIPELINE_HASH.unpack_from(record)[0]:
                return size
    raise ValueError('could not find the pipeline description size')


def main(argv):
    if len(argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    output, inputs = argv[1], argv[2:]
    if output.endswith('.xsh'):
        files = [read_shader_storage(p) for p in inputs]
    else:
        size = pipeline_record_size(inputs)
        files = [read_pipeline_storage(p, size) for p in inputs]
    header = files[0][0]
    for path, (other_header, _) in zip(inputs, files):
        if other_header != header:
            raise ValueError(f'{path} has another format version or API than {inputs[0]}')
    seen, merged = set(), []
    for path, (_, entries) in zip(inputs, files):
        added = 0
        for entry_hash, blob in entries:
            if entry_hash not in seen:
                seen.add(entry_hash)
                merged.append(blob)
                added += 1
        print(f'{path}: {len(entries)} entries, {added} new')
    with open(output, 'wb') as out:
        out.write(header)
        for blob in merged:
            out.write(blob)
    print(f'{output}: {len(merged)} entries')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
