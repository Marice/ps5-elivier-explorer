#!/usr/bin/env python3
# EliVier Explorer - Makes the small ZIP fixtures tests/unit/archive_source_test.cpp reads.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# usage: tools/make-archive-fixtures.py
#
# Every file is invented content (no third-party material). "split.zip"/"split.z01" are built by
# hand, byte by byte, since Python's zipfile module cannot write a multi-volume archive; the other
# fixtures come from the standard library's own writer. Run again only when a fixture's content
# has to change: the bytes are not guaranteed to come out the same twice (timestamps), so a
# regenerated file is committed deliberately, not as a side effect of running this.

import os
import struct
import zipfile
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "tests", "fixtures", "archives")
os.makedirs(OUT, exist_ok=True)


def basic():
    path = os.path.join(OUT, "basic.zip")
    with zipfile.ZipFile(path, "w") as zf:
        zf.writestr("top.txt", b"top-level file\n", zipfile.ZIP_STORED)
        zf.writestr("folder/inside.txt", b"a file in a folder\n", zipfile.ZIP_STORED)
        zf.writestr("folder/nested/deep.txt", b"two folders deep\n", zipfile.ZIP_STORED)
        zf.writestr("folder/empty/.keep", b"", zipfile.ZIP_STORED)
    print(path)


def mixed_compression():
    path = os.path.join(OUT, "mixed.zip")
    with zipfile.ZipFile(path, "w") as zf:
        zf.writestr("stored.txt", b"this one is stored\n", zipfile.ZIP_STORED)
        zf.writestr("deflated.txt", b"this one is deflated, so it cannot be read\n" * 20,
                   zipfile.ZIP_DEFLATED)
    print(path)


def zip64_forced():
    path = os.path.join(OUT, "zip64.zip")
    with zipfile.ZipFile(path, "w") as zf:
        with zf.open(zipfile.ZipInfo("forced.txt"), "w", force_zip64=True) as member:
            member.write(b"small file, but written with 64-bit fields\n")
    print(path)


# A multi-volume, stored-only ZIP, built by hand: entry "a.txt" lives entirely in split.z01,
# entry "b.txt" and the whole central directory live in split.zip. Disk numbers are 0 (z01) and
# 1 (zip). This is the layout tools/console validation and PKWARE's APPNOTE.TXT describe for an
# old-style split; it is not what any single real tool necessarily produces byte for byte, but it
# is a faithful one.
def split():
    def crc(data: bytes) -> int:
        return zlib.crc32(data) & 0xFFFFFFFF

    def local_header(name: bytes, data: bytes):
        return struct.pack("<IHHHHHIIIHH", 0x04034B50, 20, 0, 0, 0, 0, crc(data), len(data),
                           len(data), len(name), 0) + name

    def central_header(name: bytes, data: bytes, disk: int, local_offset: int):
        return struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 20, 20, 0, 0, 0, 0, crc(data),
                           len(data), len(data), len(name), 0, 0, disk, 0, 0,
                           local_offset) + name

    a_name, a_data = b"a.txt", b"the first volume's own file\n"
    b_name, b_data = b"b.txt", b"the second volume's own file\n"

    z01 = local_header(a_name, a_data) + a_data
    zip_part_start = local_header(b_name, b_data) + b_data
    # b.txt's local header sits at offset 0 within disk 1 (split.zip), not at the central
    # directory's own offset: a local_offset is always relative to the start of its own volume.
    central = central_header(a_name, a_data, 0, 0) + central_header(b_name, b_data, 1, 0)
    eocd = struct.pack("<IHHHHIIH", 0x06054B50, 1, 1, 2, 2, len(central), len(zip_part_start), 0)
    zip_part = zip_part_start + central + eocd

    with open(os.path.join(OUT, "split.z01"), "wb") as f:
        f.write(z01)
    with open(os.path.join(OUT, "split.zip"), "wb") as f:
        f.write(zip_part)
    print(os.path.join(OUT, "split.z01"), os.path.join(OUT, "split.zip"))


basic()
mixed_compression()
zip64_forced()
split()
