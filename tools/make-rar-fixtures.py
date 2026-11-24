#!/usr/bin/env python3
# EliVier Explorer - Makes the small RAR fixtures tests/unit/archive_source_test.cpp reads.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# usage: tools/make-rar-fixtures.py
#
# No RAR-writing tool is installed in this environment, so every fixture here is built by hand,
# byte by byte, following rarlab.com/technote.htm (RAR5) and the LGPL-3.0 unarr project's
# rar/rar.h and rar/parse-rar.c (RAR4, read as a reference for exact byte offsets, nothing of its
# code copied). Header CRC32 fields are left as 0: this project's own RAR reader never checks
# them (matching its ZIP reader, which does not check entry CRCs either), so a real value would
# only be cosmetic here. Every file is invented content (no third-party material).

import os
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "tests", "fixtures", "archives")
os.makedirs(OUT, exist_ok=True)

RAR4_SIGNATURE = b"Rar!\x1a\x07\x00"
RAR5_SIGNATURE = b"Rar!\x1a\x07\x01\x00"


def vint(value: int) -> bytes:
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


# ---------------------------------------------------------------------------
# RAR4: a single-volume archive with one stored file and one folder (implied
# by a FILE_HEAD whose LHD_DIRECTORY bits are all set).
# ---------------------------------------------------------------------------
def rar4_basic():
    def block_header(block_type: int, flags: int, size: int) -> bytes:
        return struct.pack("<HBHH", 0, block_type, flags, size)

    # MAIN_HEAD: 7-byte block header + 6 reserved bytes (posav high/low, misc flags) = 13 bytes.
    main_head = block_header(0x73, 0, 7 + 6) + b"\x00" * 6

    def file_head(name: bytes, data: bytes, flags: int, is_dir: bool) -> bytes:
        size_field = len(data)
        dosdate = 0x4A21_0000  # an arbitrary, valid-looking MS-DOS packed date/time
        fixed = struct.pack("<IBIIBBHI", size_field, 0, 0, dosdate, 20, 0x30, len(name),
                           0x20 if is_dir else 0x20)
        header_size = 7 + 4 + len(fixed) + len(name)  # block header + datasize + fixed + name
        flags |= 1 << 15  # LHD_LONG_BLOCK: FILE_HEAD always carries the extra datasize field
        return block_header(0x74, flags, header_size) + struct.pack("<I", size_field) + fixed + name

    top_data = b"top-level file, rar4\n"
    top_head = file_head(b"top.txt", top_data, 0, False)

    folder_marker = file_head(b"folder/inside.txt", b"a file in a folder, rar4\n", 0, False)

    path = os.path.join(OUT, "basic.rar")
    with open(path, "wb") as f:
        f.write(RAR4_SIGNATURE)
        f.write(main_head)
        f.write(top_head)
        f.write(top_data)
        f.write(folder_marker)
        f.write(b"a file in a folder, rar4\n")
    print(path)


# ---------------------------------------------------------------------------
# RAR4: old-style multi-volume set (basic.rar / basic.r00), one file whose
# data starts in the first volume and continues into the second.
# ---------------------------------------------------------------------------
def rar4_split():
    def block_header(block_type: int, flags: int, size: int) -> bytes:
        return struct.pack("<HBHH", 0, block_type, flags, size)

    LHD_SPLIT_AFTER = 1 << 1
    LHD_SPLIT_BEFORE = 1 << 0
    MHD_VOLUME = 1 << 0

    main_head_v0 = block_header(0x73, MHD_VOLUME, 7 + 6) + b"\x00" * 6
    main_head_v1 = block_header(0x73, MHD_VOLUME, 7 + 6) + b"\x00" * 6

    full_data = b"split across two rar4 volumes, twenty-eight bytes!\n"
    assert len(full_data) < 60
    part0 = full_data[:30]
    part1 = full_data[30:]

    def file_head(name: bytes, datasize: int, flags: int) -> bytes:
        dosdate = 0x4A21_0000
        fixed = struct.pack("<IBIIBBHI", len(full_data), 0, 0, dosdate, 20, 0x30, len(name), 0x20)
        header_size = 7 + 4 + len(fixed) + len(name)
        flags |= 1 << 15
        return block_header(0x74, flags, header_size) + struct.pack("<I", datasize) + fixed + name

    name = b"movie.bin"
    head0 = file_head(name, len(part0), LHD_SPLIT_AFTER)

    path0 = os.path.join(OUT, "split.rar")
    with open(path0, "wb") as f:
        f.write(RAR4_SIGNATURE)
        f.write(main_head_v0)
        f.write(head0)
        f.write(part0)
    print(path0)

    path1 = os.path.join(OUT, "split.r00")
    with open(path1, "wb") as f:
        f.write(RAR4_SIGNATURE)
        f.write(main_head_v1)
        # RAR4's convention: a continued entry's data starts at the very top of the next volume,
        # with no header of its own until the data ends, so this volume is signature + main header
        # + the rest of movie.bin's bytes, with nothing else following.
        f.write(part1)
    print(path1)


# ---------------------------------------------------------------------------
# RAR5: a single-volume archive with one stored file and one folder.
# ---------------------------------------------------------------------------
def rar5_file_header(name: bytes, data: bytes, directory: bool, extra_flags: int = 0) -> bytes:
    file_flags = 0x0001 if directory else 0
    file_flags |= 0x0002  # mtime present
    unpacked_size = 0 if directory else len(data)
    attrs = 0
    mtime = struct.pack("<I", 1700000000)
    compression_info = vint(0)  # method = store, version = 0, not solid
    host_os = vint(1)  # unix
    body = (
        vint(file_flags | extra_flags)
        + vint(unpacked_size)
        + vint(attrs)
        + mtime
        + compression_info
        + host_os
        + vint(len(name))
        + name
    )
    # General block format: header size (vint) covers everything from header type onward,
    # including the data-size vint, which sits right after header flags, before header-type-
    # specific fields like "file flags" (that order is not negotiable: a reader must consume
    # data-size before it even knows this is a file header).
    header_flags = 0 if directory else 0x0002  # data area present only for a real file
    data_size = vint(0 if directory else len(data))
    type_and_flags = vint(2) + vint(header_flags)
    payload = type_and_flags + (data_size if not directory else b"") + body
    header_size = vint(len(payload))
    header = struct.pack("<I", 0) + header_size + payload
    return header + (data if not directory else b"")


def rar5_main_header() -> bytes:
    archive_flags = vint(0)
    body = archive_flags
    type_and_flags = vint(1) + vint(0)
    payload = type_and_flags + body
    header_size = vint(len(payload))
    return struct.pack("<I", 0) + header_size + payload


def rar5_basic():
    top_data = b"top-level file, rar5\n"
    folder_data = b"a file in a folder, rar5\n"
    path = os.path.join(OUT, "basic5.rar")
    with open(path, "wb") as f:
        f.write(RAR5_SIGNATURE)
        f.write(rar5_main_header())
        f.write(rar5_file_header(b"top.txt", top_data, False))
        f.write(rar5_file_header(b"folder/inside.txt", folder_data, False))
    print(path)


rar4_basic()
rar4_split()
rar5_basic()
