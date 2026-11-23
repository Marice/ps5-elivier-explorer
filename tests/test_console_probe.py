#!/usr/bin/env python3
# EliVier Explorer - Tests for the pure parts of tools/console-probe.py.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Nothing here touches a console: the name check, the request line and the check of the logs.

import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
_SPEC = importlib.util.spec_from_file_location("console_probe", ROOT / "tools/console-probe.py")
probe = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(probe)


class ConsoleProbeTests(unittest.TestCase):
    def test_names_the_app_could_accept_pass(self):
        for name in ("safe", "memory", "memory+upload", "decoder-load", "hdr:pq10", "hdr:fp16"):
            self.assertTrue(probe.valid_probe_name(name), name)

    def test_anything_else_is_refused_before_it_reaches_the_console(self):
        for name in ("", "MEMORY", "memory upload", "a" * 65, "../x", "memory;rm", "memory\n",
                     "hdr: pq10"):
            self.assertFalse(probe.valid_probe_name(name), repr(name))

    def test_the_request_is_one_line_in_the_shape_the_app_reads(self):
        self.assertEqual(probe.request_line("memory+upload", "1700000000"),
                         "probe memory+upload 1700000000\n")

    def test_a_folder_that_exists_is_not_created_again(self):
        class FakeFtp:
            def __init__(self, existing):
                self.existing = set(existing)
                self.calls = []

            def cwd(self, path):
                self.calls.append(("CWD", path))
                if path != "/" and path not in self.existing:
                    raise probe.tour.error_perm("550 no such folder")

            def mkd(self, path):
                self.calls.append(("MKD", path))
                self.existing.add(path)

        console = probe.ProbeConsole.__new__(probe.ProbeConsole)
        console.ftp = FakeFtp({"/data", "/data/homebrew"})
        console.make_dirs("/data/homebrew/TITLE/assets")
        made = [path for verb, path in console.ftp.calls if verb == "MKD"]
        self.assertEqual(made, ["/data/homebrew/TITLE", "/data/homebrew/TITLE/assets"])

    def test_a_request_for_a_name_the_app_would_refuse_is_never_written(self):
        for bad in ("param.json", "", "MEMORY", "a b"):
            with self.assertRaises(ValueError):
                probe.request_line(bad, "1")

    def test_install_sends_what_differs_with_the_executable_and_metadata_last(self):
        class FakeConsole:
            def __init__(self, listing):
                self.listing = listing
                self.sent = []

            def sizes(self, directory):
                return self.listing.get(directory)

            def upload(self, path, data):
                self.sent.append(path)

        with tempfile.TemporaryDirectory() as directory:
            package = Path(directory)
            (package / "assets").mkdir()
            (package / "assets/big_same.bin").write_bytes(b"x" * (2 * 1024 * 1024))
            (package / "assets/big_changed.bin").write_bytes(b"x" * (2 * 1024 * 1024))
            (package / "assets/small.txt").write_bytes(b"hello")
            (package / "eboot.bin").write_bytes(b"e" * 10)
            (package / "param.json").write_bytes(b"{}")
            remote = "/data/homebrew/T"
            console = FakeConsole({
                f"{remote}/assets": {"big_same.bin": 2 * 1024 * 1024,
                                      "big_changed.bin": 1, "small.txt": 5},
                remote: {"eboot.bin": 10, "param.json": 2},
            })
            checked, sent = probe.install_package(console, package, remote)
        self.assertEqual(checked, 5)
        self.assertEqual(sent, 5 - 1)  # only the large file of the right size is left alone
        self.assertNotIn(f"{remote}/assets/big_same.bin", console.sent)
        self.assertEqual(console.sent[-2:], [f"{remote}/eboot.bin", f"{remote}/param.json"])

    def test_what_has_to_be_sent_is_judged_by_size(self):
        big = 5 * 1024 * 1024
        self.assertTrue(probe.needs_upload(None, big))            # not on the console
        self.assertTrue(probe.needs_upload(big - 1, big))         # another size
        self.assertFalse(probe.needs_upload(big, big))            # a large file that matches
        self.assertTrue(probe.needs_upload(100, 100))             # small files are always sent

    def test_an_upload_whose_size_does_not_arrive_stops_the_run(self):
        class FakeFtp:
            def __init__(self, stored_size):
                self.stored_size = stored_size

            def cwd(self, path):
                pass

            def mkd(self, path):
                pass

            def storbinary(self, command, stream, blocksize=0):
                stream.read()

            def mlsd(self, facts=None):
                return iter([("file.bin", {"size": str(self.stored_size)})])

        console = probe.ProbeConsole.__new__(probe.ProbeConsole)
        console.ftp = FakeFtp(10)
        console.upload("/data/homebrew/T/file.bin", b"x" * 10)  # arrives whole: no error
        console.ftp = FakeFtp(4)
        with self.assertRaises(SystemExit):
            console.upload("/data/homebrew/T/file.bin", b"x" * 10)

    def test_a_manual_launch_is_only_asked_for_with_the_switch_on(self):
        self.assertTrue(probe.wants_manual_launch({"PS5_MANUAL_LAUNCH": "1"}))
        self.assertFalse(probe.wants_manual_launch({}))
        self.assertFalse(probe.wants_manual_launch({"PS5_MANUAL_LAUNCH": "0"}))
        self.assertFalse(probe.wants_manual_launch({"PS5_MANUAL_LAUNCH": "yes"}))

    def test_a_clean_run_has_no_problems(self):
        log = "[HUI] probe run requested: safe\n[HUI] probe memory t12_16mib_allocate=0x80020012\n" \
              "[HUI] probe run finished lines=12\n"
        self.assertEqual(probe.problems_in(log, "", "PPSA00000"), [])

    def test_a_failed_allocation_is_an_answer_not_a_problem(self):
        log = "[HUI] probe memory t12_1024mib failed to allocate\n[HUI] probe run finished\n"
        self.assertEqual(probe.problems_in(log, "", "PPSA00000"), [])

    def test_fatal_lines_and_a_missing_finish_line_are_problems(self):
        problems = probe.problems_in("[HUI] fatal: display open failed\n", "", "PPSA00000")
        self.assertEqual(len(problems), 2)
        self.assertTrue(any("fatal" in p for p in problems))
        self.assertTrue(any("probe run finished" in p for p in problems))

    def test_a_crash_of_this_title_in_the_kernel_log_is_a_problem(self):
        log = "[HUI] probe run finished\n"
        klog = "other title crashed\nPPSA00000 SIGSEGV in thread\n"
        problems = probe.problems_in(log, klog, "PPSA00000")
        self.assertEqual(len(problems), 1)
        self.assertIn("klog", problems[0])


if __name__ == "__main__":
    unittest.main()
