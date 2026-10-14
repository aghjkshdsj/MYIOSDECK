#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regression cases for exact-fault analysis and streaming symbol coverage."""
import io
import pathlib
import subprocess
import sys
import tempfile
import unittest

import precise


def handler(name, start, body):
    return f'{start:016x} <{name}>:\n' + ''.join(
        f' {start + 4*i:x}:\t{insn}\n' for i, insn in enumerate(body)) + '\n'


class PrecisionTests(unittest.TestCase):
    def check_body(self, body, unsafe, accesses=1):
        entries = list(precise.iter_funcs(io.StringIO(handler('p_case', 0x100, body))))
        self.assertEqual(len(entries), 1)
        violations, guest = precise.analyse(entries[0][1])
        self.assertEqual(bool(violations), unsafe)
        self.assertEqual(guest, accesses)

    def test_faulting_load_has_not_written_its_destination(self):
        self.check_body(['ldr x22, [x1]', 'br x8'], False)

    def test_register_changed_before_fault(self):
        self.check_body(['mov x22, #0', 'ldr x8, [x1]', 'br x8'], True)

    def test_restored_entry_value_before_fault(self):
        self.check_body(['mov x15, x22', 'mov x22, #0', 'mov x22, x15',
                         'ldr x8, [x1]', 'br x8'], False)

    def test_cpu_store_before_fault(self):
        self.check_body(['str x22, [x10]', 'ldr x8, [x1]', 'br x8'], True)

    def test_one_unsafe_branch_path_is_rejected(self):
        self.check_body(['cbz x22, 108', 'mov x23, #0', 'ldr x8, [x1]', 'br x8'], True)

    def test_internal_uop_load_is_not_guest_memory(self):
        self.check_body(['mov x12, #0', 'ldr x8, [x21]', 'br x8'], False, 0)

    def test_duplicate_local_names_are_all_audited(self):
        # Linked replica translation units can have identical local names.
        # An unsafe first entry must survive a later safe entry of that name.
        text = handler('p_duplicate', 0x100, ['mov x22, #0', 'ldr x8, [x1]', 'br x8'])
        text += handler('p_duplicate', 0x200, ['ldr x8, [x1]', 'br x8'])
        entries = list(precise.iter_funcs(io.StringIO(text)))
        self.assertEqual([name for name, _ in entries], ['p_duplicate', 'p_duplicate'])
        self.assertEqual([insns[0][0] for _, insns in entries], [0x100, 0x200])
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / 'handlers.txt'
            path.write_text(text)
            result = subprocess.run([sys.executable, str(pathlib.Path(precise.__file__)), str(path)],
                                    capture_output=True, text=True)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('2 pinned handlers, 2 touch guest memory, 1 may fault', result.stdout)
        self.assertIn('1 of them used in Windows mode', result.stdout)


if __name__ == '__main__':
    unittest.main()
