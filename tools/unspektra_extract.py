#!/usr/bin/env python3
"""Extract a frame from an UNSPKTRA v2 file as little-endian 16-bit CFA samples.

The UFR1 decoder was derived from the Unspektrawesome 1.2.3 release binary.
Output raster dimensions come from UFR1; recipe dimensions describe orientation.
"""

import argparse
import json
import struct
import zlib
from pathlib import Path


def record(file, offset, limit):
    if offset < 20 or offset + 12 > limit:
        raise ValueError("record offset outside file")
    file.seek(offset)
    size, kind, checksum = struct.unpack(">III", file.read(12))
    if kind != 0 or offset + 12 + size > limit:
        raise ValueError("invalid record header")
    payload = file.read(size)
    if zlib.crc32(payload) != checksum:
        raise ValueError("record CRC32 mismatch")
    return payload


def decode_ufr1(data):
    if len(data) < 16 or data[:4] != b"UFR1":
        raise ValueError("not a UFR1 frame")
    width, height, mode = struct.unpack_from("<III", data, 4)
    if not 2 <= width <= 16384 or not 2 <= height <= 16384 or width % 2 or height % 2 or mode not in (2, 4, 6, 8):
        raise ValueError("unsupported UFR1 dimensions or mode")
    pixels = bytearray(width * height * 2)
    position = 16
    for tile_y in range(0, height, 16):
        for tile_x in range(0, width, 16):
            for parity_y, parity_x in ((0, 0), (0, 1), (1, 0), (1, 1)):
                if position + 3 > len(data):
                    raise ValueError("truncated UFR1 block")
                bits = data[position]
                reference = struct.unpack_from("<H", data, position + 1)[0]
                if bits > 16:
                    raise ValueError("invalid UFR1 bit width")
                packed_size = 8 * bits  # 64 samples, each 'bits' wide
                start = position + 3
                end = start + packed_size
                if end > len(data):
                    raise ValueError("truncated UFR1 samples")
                packed = int.from_bytes(data[start:end], "little")
                mask = (1 << bits) - 1
                for i in range(64):
                    x = tile_x + parity_x + 2 * (i % 8)
                    y = tile_y + parity_y + 2 * (i // 8)
                    if x >= width or y >= height:
                        continue
                    value = reference + ((packed >> (i * bits)) & mask)
                    if value > 65535:
                        raise ValueError("UFR1 sample overflow")
                    struct.pack_into("<H", pixels, 2 * (y * width + x), value)
                position = end
    if position != len(data):
        raise ValueError(f"{len(data) - position} trailing UFR1 bytes")
    return width, height, pixels


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("frame", type=int)
    parser.add_argument("output", type=Path, help="output .raw path")
    args = parser.parse_args()
    with args.source.open("rb") as file:
        header = file.read(20)
        if len(header) != 20 or header[:8] != b"UNSPKTRA" or struct.unpack_from(">I", header, 8)[0] != 2:
            raise ValueError("not an UNSPKTRA v2 file")
        file.seek(0, 2)
        limit = file.tell()
        index_offset = struct.unpack_from(">Q", header, 12)[0]
        index = json.loads(record(file, index_offset, limit))
        entry = index["frames"][args.frame]
        recipe = json.loads(zlib.decompress(record(file, entry["recipe"], limit)))
        width, height, pixels = decode_ufr1(record(file, entry["raw"], limit))
    args.output.write_bytes(pixels)
    metadata_path = args.output.with_suffix(".json")
    metadata_path.write_text(json.dumps({"rasterWidth": width, "rasterHeight": height,
                                         "timestamp": entry["timestamp"], "recipe": recipe}, indent=2))
    print(f"Wrote {width}x{height} uint16 LE CFA samples to {args.output}")
    print(f"Wrote recipe and frame metadata to {metadata_path}")


if __name__ == "__main__":
    main()
