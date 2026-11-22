#!/usr/bin/env python3
"""Add the ROM digest to a C5 Simple Boot image without moving its XIP data."""

import hashlib
import struct
import sys
from pathlib import Path


def add_digest(image):
    if len(image) < 24 or image[0] != 0xE9 or image[12:14] != b'\x17\x00':
        raise ValueError("expected an ESP32-C5 image")
    end = 24
    for _ in range(image[1]):
        _, size = struct.unpack_from("<II", image, end)
        end += 8 + size
    end = (end // 16 + 1) * 16  # ROM checksum occupies the last byte.
    if image[23] == 1 and image[end:end + 32] == hashlib.sha256(image[:end]).digest():
        return image
    address, size = struct.unpack_from("<II", image, end)
    if image[23] or address or size < 32 or end + 8 + size > len(image):
        raise ValueError("missing Simple Boot alignment padding")
    if any(image[end + 8:end + 8 + size]):
        raise ValueError("expected zero-filled alignment padding")
    image[23] = 1
    image[end:end + 32] = hashlib.sha256(image[:end]).digest()
    struct.pack_into("<II", image, end + 32, 0, size - 32)
    return image


if __name__ == "__main__":
    path = Path(sys.argv[1])
    path.write_bytes(add_digest(bytearray(path.read_bytes())))
