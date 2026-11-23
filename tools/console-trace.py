#!/usr/bin/env python3
# EliVier Explorer - Reads the app's trace and probe report from the console over FTP.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prints the last run of the app's trace (and saves the trace and the probe report).

usage: tools/console-trace.py <host> <ftp port> [save folder]

The app writes /data/elivier-explorer/trace.txt line by line (src/explorer/trace.hpp), so it is
there after the app has closed or crashed. A probe run also leaves probe-report.txt beside it.
Only reads two small text files. With a save folder both are copied there as well.
"""

import io
import sys
from ftplib import FTP, all_errors
from pathlib import Path

FOLDER = "/data/elivier-explorer"


def last_run(text):
    lines = text.splitlines()
    starts = [i for i, line in enumerate(lines) if line.startswith("=== run")]
    return lines[starts[-1]:] if starts else lines


def main():
    if len(sys.argv) not in (3, 4):
        raise SystemExit(__doc__)
    host, port = sys.argv[1], int(sys.argv[2])
    save = Path(sys.argv[3]) if len(sys.argv) == 4 else None
    ftp = FTP()
    ftp.connect(host, port, timeout=15)
    ftp.login("anonymous", "console-trace")
    found = {}
    for name in ("trace.txt", "probe-report.txt"):
        data = io.BytesIO()
        try:
            ftp.retrbinary(f"RETR {FOLDER}/{name}", data.write)
        except all_errors as error:
            print(f"({name}: {error})")
            continue
        found[name] = data.getvalue()
    ftp.close()
    if save is not None:
        save.mkdir(parents=True, exist_ok=True)
        for name, data in found.items():
            (save / name).write_bytes(data)
        print(f"saved to {save}")
    if "trace.txt" in found:
        print("\n".join(line[:220] for line in last_run(found["trace.txt"].decode("utf-8", "replace"))))


if __name__ == "__main__":
    try:
        main()
    except all_errors as error:
        raise SystemExit(f"the console did not answer ({error})")
