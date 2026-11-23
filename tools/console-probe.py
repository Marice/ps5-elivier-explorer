#!/usr/bin/env python3
# EliVier Explorer - One probe run on a console: install, probe, collect the report.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
"""Runs console probes (src/platform/ps5/probes.cpp) and brings the report back.

usage: PS5_HOST=<address> tools/console-probe.py <results dir> <probe name>

Probe names, joined with '+': memory, upload, egl, decoder-load, or safe (all four, which only
read and measure). hdr:pq10, hdr:hlg10 and hdr:fp16 change the display and run alone: watch the
TV while the bars are shown (12 s) and say whether it switched to HDR. There is no default: a
probe is always named.

The steps are those of tools/console-tour.py, which this script borrows its console access from:
check the console's services; refuse if the title is running; upload dist/<TITLE_ID> (every file
verified by hash); put the probe request beside it; wait for the title's registration; record the
kernel log; launch; wait for the report; download it and the app log; wait for the app to close
itself; remove the request. It never kills the app and never retries. Read
docs/CONSOLE_VALIDATION.md first, and ask the console's owner before each run.

Environment: as console-tour.py, and PROBE_TIMEOUT (seconds to wait for the report, default 300).

PS5_MANUAL_LAUNCH=1 leaves the launch to a person: the script uploads, writes the request, waits
for the title's registration and then asks you to start the title from the home screen (LAUNCH_TIMEOUT
seconds to do so, default 300). It needs neither the ELF loader nor the launch helper, and nothing
but the title itself runs on the console. Use it for a title whose ID is not PPSA plus five digits,
which the helper refuses.
"""

import hashlib
import importlib.util
import json
import os
import re
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
_SPEC = importlib.util.spec_from_file_location("console_tour", Path(__file__).with_name("console-tour.py"))
tour = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(tour)

# A probe result may legitimately say "failed" (an allocation that did not fit is an answer), so
# only these end a run as a problem.
BAD_PROBE_LOG_WORDS = ("fatal", "assertion")


def valid_probe_name(name):
    """The shape the app accepts; it checks the words itself and reports a refusal."""
    return re.fullmatch(r"[a-z0-9:+-]{1,64}", name) is not None


class ProbeConsole(tour.Console):
    """The tour's console access, with one change: a folder that is there is not created again.

    The tour sends MKD for every part of a path, /data and /data/homebrew included. Some FTP
    servers for the console do not answer MKD for a folder that exists, and the upload hangs.
    """

    def make_dirs(self, path):
        current = ""
        for part in path.strip("/").split("/"):
            current += "/" + part
            try:
                self.ftp.cwd(current)
                continue
            except tour.error_perm:
                pass
            try:
                self.ftp.mkd(current)
            except tour.error_perm:
                pass
        try:
            self.ftp.cwd("/")
        except tour.all_errors:
            pass


def wants_manual_launch(environ):
    return environ.get("PS5_MANUAL_LAUNCH") == "1"


def request_line(name, token):
    return f"probe {name} {token}\n"


def problems_in(app_log, klog, title):
    problems = []
    for line in app_log.splitlines():
        if any(word in line.lower() for word in BAD_PROBE_LOG_WORDS):
            problems.append(f"app.log: {line.strip()}")
    if "probe run finished" not in app_log:
        problems.append("app.log: no 'probe run finished' line")
    for line in klog.splitlines():
        if title in line and any(word in line.lower() for word in tour.BAD_KLOG_WORDS):
            problems.append(f"klog: {line.strip()[:200]}")
    return problems


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    results = Path(sys.argv[1])
    name = sys.argv[2]
    if not valid_probe_name(name):
        tour.stop(f"'{name}' is not a probe name (lower case words joined with '+'; run without arguments for the list)")
    host = os.environ.get("PS5_HOST") or tour.stop("set PS5_HOST")
    ftp_port = int(os.environ.get("FTP_PORT", "2121"))
    klog_port = int(os.environ.get("KLOG_PORT", "3232"))
    elf_port = int(os.environ.get("ELF_PORT", "9021"))
    timeout = int(os.environ.get("PROBE_TIMEOUT", "300"))
    register_wait = int(os.environ.get("REGISTER_TIMEOUT", "120"))
    manual = wants_manual_launch(os.environ)
    launch_wait = int(os.environ.get("LAUNCH_TIMEOUT", "300")) if manual else 90
    protocol = Path(os.environ.get("PS5_PROTOCOL", ROOT.parent / "ps5-homebrew-dev-protocol"))
    sdk = os.environ.get("PS5_PAYLOAD_SDK", str(ROOT / ".deps/native/ps5-payload-sdk"))
    sender = protocol / "scripts/send-controller.sh"
    title = json.loads((ROOT / "sce_sys/param.json").read_text())["titleId"]
    package = ROOT / "dist" / title
    if not (package / "eboot.bin").is_file():
        tour.stop(f"{package} is not built (run make)")
    if not manual and not sender.is_file():
        tour.stop(f"launch helper not found: {sender} (set PS5_PROTOCOL, or PS5_MANUAL_LAUNCH=1)")
    results.mkdir(parents=True, exist_ok=True)

    tour.say(f"Checking the console's services at {host}")
    if not tour.port_open(host, ftp_port) or (not manual and not tour.port_open(host, elf_port)):
        tour.stop("FTP or the ELF loader does not answer")
    console = ProbeConsole(host, ftp_port)
    active = tour.running_titles(console)
    if title in active:
        tour.stop(f"{title} is running on the console; close it first")
    if active:
        tour.say(f"Other titles running (left alone): {', '.join(active)}")

    remote = f"/data/homebrew/{title}"
    first_install = console.names(remote) is None
    files = sorted(p for p in package.rglob("*") if p.is_file())
    # The executable and the metadata go last: the title is complete when they land.
    files.sort(key=lambda p: (p.name in ("eboot.bin", "param.json"), str(p)))
    uploaded = 0
    for path in files:
        relative = path.relative_to(package).as_posix()
        data = path.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        installed = console.read(f"{remote}/{relative}")
        if installed is not None and hashlib.sha256(installed).hexdigest() == digest:
            continue
        console.write(f"{remote}/{relative}", data)
        uploaded += 1
    tour.say(f"Installed {title}: {len(files)} files verified, {uploaded} uploaded")

    # The request goes into the install folder (the app reads it as /app0/dev/request.txt).
    token = str(int(time.time()))
    console.write(f"{remote}/dev/request.txt", request_line(name, token).encode())
    uploaded += 1
    console.close()

    waited = 0
    while True:
        console = ProbeConsole(host, ftp_port)
        registered = "mount.lnk" in (console.names(f"/user/app/{title}") or [])
        console.close()
        if registered:
            break
        if waited >= register_wait:
            tour.stop(f"{title} was not registered after {register_wait} s "
                      "(is ShadowMount Plus running?); nothing was launched")
        if waited == 0:
            tour.say("Waiting for the console to register the title")
        time.sleep(5)
        waited += 5
    if first_install or uploaded:
        time.sleep(15)

    done = threading.Event()
    klog = None
    if tour.port_open(host, klog_port):
        klog = threading.Thread(target=tour.record_klog,
                                args=(host, klog_port, results / "klog.txt", done), daemon=True)
        klog.start()
        time.sleep(1.0)

    if name.startswith("hdr:"):
        tour.say("This is an HDR attempt: watch the TV for the bars and any HDR badge")
    started = time.time()
    if manual:
        tour.say(f"NOW: start {title} (EliVier Explorer) from the console's home screen "
                 f"(waiting up to {launch_wait} s for probe '{name}')")
    else:
        tour.say(f"Launching {title} with probe '{name}'")
        launch = subprocess.run(["bash", str(sender), "launch", title, host, str(elf_port)],
                                env={**os.environ, "PS5_PAYLOAD_SDK": sdk}, capture_output=True,
                                text=True, check=False)
        if launch.returncode != 0:
            done.set()
            tour.stop(f"the launch helper failed: {launch.stderr.strip() or launch.stdout.strip()}")

    dev = f"/mnt/sandbox/{title}_000/download0/hui/dev"
    report_path = f"{dev}/probe/report.txt"

    def connect():
        try:
            return ProbeConsole(host, ftp_port)
        except tour.all_errors:
            done.set()
            tour.stop("the console stopped answering during the run; do not retry, look at klog.txt")

    taken = False
    while time.time() - started < launch_wait:
        time.sleep(3)
        console = connect()
        taken = (console.read(f"{dev}/handled.txt") or b"").decode().strip() == token
        console.close()
        if taken:
            break
    if not taken:
        done.set()
        tour.stop("the app did not take the probe request (it may be running: close it by hand)")
    tour.say("The app is up and probing")

    finished = False
    while time.time() - started < timeout:
        time.sleep(5)
        console = connect()
        active = tour.running_titles(console)
        reported = console.stamp(report_path) is not None
        console.close()
        if title not in active:
            done.set()
            tour.stop("the title closed before its probe reported; look at klog.txt")
        if reported:
            finished = True
            break
    if not finished:
        done.set()
        tour.stop(f"no report after {timeout} s; the app was left as it is")
    tour.say(f"Probe finished after {time.time() - started:.0f} s; collecting")

    console = connect()
    data = console.read(report_path)
    if data is None:
        done.set()
        tour.stop("report.txt could not be read back; the app closes by itself in a few minutes")
    (results / "report.txt").write_bytes(data)
    app_log = console.read(f"{dev}/app.log") or b""
    (results / "app.log").write_bytes(app_log)
    console.close()
    tour.say(f"Downloaded the report and the log to {results}")

    tour.say("Waiting for the app to close itself")
    closed = False
    for _ in range(48):
        time.sleep(5)
        console = connect()
        closed = title not in tour.running_titles(console)
        console.close()
        if closed:
            break
    time.sleep(3)
    done.set()
    if klog is not None:
        klog.join(timeout=5)
    tour.say("The app closed itself" if closed else "The app is still running: close it from the console")
    if closed:
        console = connect()
        try:
            console.ftp.sendcmd(f"DELE {remote}/dev/request.txt")
            console.ftp.sendcmd(f"RMD {remote}/dev")
        except tour.all_errors:
            pass
        console.close()

    klog_file = results / "klog.txt"
    problems = problems_in(app_log.decode("utf-8", "replace"),
                           klog_file.read_text(errors="replace") if klog_file.is_file() else "",
                           title)
    if not closed:
        problems.append("the app did not close after its evidence window")
    services = (ftp_port,) if manual else (ftp_port, elf_port)
    if not all(tour.port_open(host, port) for port in services):
        problems.append("the console's services do not all answer after the run")
    print((results / "report.txt").read_text().strip())
    if problems:
        print("\n".join(problems))
        tour.stop(f"{len(problems)} problem(s); see {results}")
    tour.say("DONE: the report is above and in report.txt; the console is healthy")


if __name__ == "__main__":
    try:
        main()
    except tour.all_errors as error:
        tour.stop(f"the console stopped answering ({error}); nothing was retried and nothing was "
                  "deleted: look at the console before running again")
