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
# `data_size` overrides the data area's own declared size (defaults to len(data)): the one field
# a split entry's own volume gets wrong on purpose, since each volume reports only its own chunk
# (rarlab.com/technote.htm: Data CRC32 "contains CRC32 of file packed data contained in current
# volume ... for all file parts except the last" - the same per-volume scoping applies to Data
# size, the field actually read here). `unpacked_size` overrides the file's own total size, so a
# continuation volume's header can still carry the entry's full size as its spec requires, not
# just what is left; `header_flags` lets a split's own continuation bits (0x0008 "continuing from
# previous volume", 0x0010 "continuing in next volume") be set on top of the default 0x0002 "data
# area present".
def rar5_file_header(name: bytes, data: bytes, directory: bool, extra_flags: int = 0,
                     header_flags: int = None, data_size: int = None,
                     unpacked_size: int = None) -> bytes:
    file_flags = 0x0001 if directory else 0
    file_flags |= 0x0002  # mtime present
    if unpacked_size is None:
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
    if header_flags is None:
        header_flags = 0 if directory else 0x0002  # data area present only for a real file
    if data_size is None:
        data_size = 0 if directory else len(data)
    data_size_field = vint(data_size)
    type_and_flags = vint(2) + vint(header_flags)
    payload = type_and_flags + (data_size_field if (header_flags & 0x0002) else b"") + body
    header_size = vint(len(payload))
    header = struct.pack("<I", 0) + header_size + payload
    return header + data


def rar5_main_header(volume_number: int = None) -> bytes:
    archive_flags = vint(0x0001 if volume_number is not None else 0)  # 0x0001 = "is volume"
    body = archive_flags + (vint(volume_number) if volume_number is not None else b"")
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


# ---------------------------------------------------------------------------
# RAR5: new-style multi-volume set (basic5.part01.rar / basic5.part02.rar), one file whose data
# starts in the first volume (header flags 0x0002 data | 0x0010 continues-in-next) and continues
# into the second (its own header, same name, header flags 0x0002 data | 0x0008 continues-from-
# previous; the true unpacked_size is repeated on both, per-volume data_size only on each).
# ---------------------------------------------------------------------------
def rar5_split():
    full_data = b"split across two rar5 volumes, over partNN.rar!\n"
    part0 = full_data[:30]
    part1 = full_data[30:]
    name = b"movie.bin"

    header0 = rar5_file_header(name, part0, False, header_flags=0x0002 | 0x0010,
                               data_size=len(part0), unpacked_size=len(full_data))
    path0 = os.path.join(OUT, "basic5.part01.rar")
    with open(path0, "wb") as f:
        f.write(RAR5_SIGNATURE)
        f.write(rar5_main_header())
        f.write(header0)
    print(path0)

    header1 = rar5_file_header(name, part1, False, header_flags=0x0002 | 0x0008,
                               data_size=len(part1), unpacked_size=len(full_data))
    path1 = os.path.join(OUT, "basic5.part02.rar")
    with open(path1, "wb") as f:
        f.write(RAR5_SIGNATURE)
        f.write(rar5_main_header(volume_number=1))
        f.write(header1)
    print(path1)


# A RAR5 archive whose one entry promises to continue in a next volume that never comes (a
# truncated set, or a single file that was never joined to its other parts): the data area here is
# real, but incomplete.
def rar5_truncated_split():
    part0 = b"the rest of this never arrives\n"
    header = rar5_file_header(b"orphan.bin", part0, False, header_flags=0x0002 | 0x0010,
                              data_size=len(part0), unpacked_size=len(part0) + 1000)
    path = os.path.join(OUT, "truncated5.rar")
    with open(path, "wb") as f:
        f.write(RAR5_SIGNATURE)
        f.write(rar5_main_header())
        f.write(header)
    print(path)


rar4_basic()
rar4_split()
rar5_basic()
rar5_split()
rar5_truncated_split()
