#!/usr/bin/env python3
# EliVier Explorer - Puts dist/<TITLE_ID> on the console over FTP, with an optional probe request.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
"""The plain way: build, put the folder on the console over FTP, start the app by hand.

usage: tools/console-install.py <host> <ftp port> [probe name]

Sends what differs from the console's copy (judged by size, small files always) to
/data/homebrew/<TITLE_ID> and checks the size of every file that arrives. With a probe name it
also writes dev/request.txt, so that the next start runs that probe instead of the app
(docs/CONSOLE_VALIDATION.md). Without one it removes a request that is still there, so a
probe that was never started does not run by surprise. Read the result with
tools/console-trace.py. Nothing else is touched on the console.
"""

import importlib.util
import json
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
_SPEC = importlib.util.spec_from_file_location("console_probe", Path(__file__).with_name("console-probe.py"))
probe = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(probe)


def main():
    if len(sys.argv) not in (3, 4):
        raise SystemExit(__doc__)
    host, port = sys.argv[1], int(sys.argv[2])
    name = sys.argv[3] if len(sys.argv) == 4 else None
    title = json.loads((ROOT / "sce_sys/param.json").read_text())["titleId"]
    package = ROOT / "dist" / title
    if not (package / "eboot.bin").is_file():
        raise SystemExit(f"{package} is not built (run make)")
    remote = f"/data/homebrew/{title}"
    console = probe.ProbeConsole(host, port)
    checked, sent = probe.install_package(console, package, remote)
    print(f"{checked} files checked by size, {sent} sent to {remote}")
    if name is not None:
        token = str(int(time.time()))
        console.upload(f"{remote}/dev/request.txt", probe.request_line(name, token).encode())
        print(f"request written: probe {name} {token}")
    else:
        try:
            console.ftp.sendcmd(f"DELE {remote}/dev/request.txt")
            print("an old request was removed")
        except probe.tour.all_errors:
            pass
    console.close()


if __name__ == "__main__":
    try:
        main()
    except probe.tour.all_errors as error:
        raise SystemExit(f"the console stopped answering ({error}); nothing was retried")
