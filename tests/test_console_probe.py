#!/usr/bin/env python3
# EliVier Explorer - Tests for the pure parts of tools/console-probe.py.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Nothing here touches a console: the name check, the request line and the check of the logs.

import importlib.util
from pathlib import Path
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
