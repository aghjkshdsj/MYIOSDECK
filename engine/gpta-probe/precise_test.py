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
    def instructions(self, body, start=0x100):
        return list(precise.iter_funcs(io.StringIO(handler('p_case',start,body))))[0][1]

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

    def test_safe_body_relocation_reuses_exact_certificate(self):
        a = self.instructions(['b.eq 108','ldr x8, [x1]','br x15'])
        b = self.instructions(['b.eq 208','ldr x8, [x1]','br x15'],0x200)
        self.assertEqual(precise.analysis_key(a),precise.analysis_key(b))
        cache = precise.SafeBodies()
        self.assertEqual(cache.check(a),([],1))
        self.assertEqual(cache.check(b),([],1))
        self.assertEqual((cache.hits,cache.fresh),(1,1))

    def test_external_destinations_do_not_change_local_model(self):
        a = self.instructions(['b.eq 2000','ldr x8, [x1]','br x15'])
        b = self.instructions(['b.eq 3000','ldr x8, [x1]','br x15'],0x200)
        self.assertEqual(precise.analysis_key(a),precise.analysis_key(b))
        self.assertEqual(precise.analyse(a),([],1))
        self.assertEqual(precise.analyse(b),([],1))

    def test_changed_local_branch_receives_fresh_analysis(self):
        safe = self.instructions(['mov x22, #0','b 10c','ldr x8, [x1]','br x15'])
        unsafe = self.instructions(['mov x22, #0','b 108','ldr x8, [x1]','br x15'])
        self.assertNotEqual(precise.analysis_key(safe),precise.analysis_key(unsafe))
        cache = precise.SafeBodies()
        self.assertEqual(cache.check(safe),([],0))
        self.assertTrue(cache.check(unsafe)[0])
        self.assertEqual((cache.hits,cache.fresh),(0,2))

    def test_changed_register_cannot_reuse_safe_result(self):
        safe = self.instructions(['mov x8, #0','ldr x15, [x1]','br x15'])
        unsafe = self.instructions(['mov x22, #0','ldr x15, [x1]','br x15'])
        cache = precise.SafeBodies()
        self.assertEqual(cache.check(safe),([],1))
        self.assertTrue(cache.check(unsafe)[0])
        self.assertEqual(cache.hits,0)

    def test_changed_memory_operand_cannot_reuse_safe_result(self):
        safe = self.instructions(['str x22, [sp]','ldr x15, [x1]','br x15'])
        unsafe = self.instructions(['str x22, [x10]','ldr x15, [x1]','br x15'])
        cache = precise.SafeBodies()
        self.assertEqual(cache.check(safe),([],1))
        self.assertTrue(cache.check(unsafe)[0])
        self.assertEqual(cache.hits,0)

    def test_unsafe_bodies_are_always_reanalysed(self):
        cache = precise.SafeBodies()
        for start in (0x100,0x200):
            self.assertTrue(cache.check(self.instructions(['mov x22, #0','ldr x8, [x1]'],start))[0])
        self.assertEqual((cache.hits,cache.fresh,len(cache.entries)),(0,2,0))

    def test_certificate_count_and_byte_limits(self):
        cache = precise.SafeBodies(max_entries=2,max_bytes=10000)
        cases = [self.instructions(['ldr x8, [x%d]' % r]) for r in (1,2,3)]
        for case in cases: self.assertEqual(cache.check(case),([],1))
        self.assertEqual(len(cache.entries),2)
        self.assertLessEqual(cache.bytes,cache.max_bytes)
        self.assertNotIn(precise.analysis_key(cases[0]),cache.entries)
        tiny = precise.SafeBodies(max_entries=2,max_bytes=1)
        tiny.check(cases[0]); tiny.check(cases[0])
        self.assertEqual((tiny.hits,tiny.fresh,len(tiny.entries),tiny.bytes),(0,2,0,0))

    def test_cached_and_direct_analysis_match_across_relocated_variants(self):
        cache = precise.SafeBodies()
        for i in range(256):
            start = 0x1000+0x100*i
            destination = 'x8' if i&1 else 'x22'
            base = 'x1' if i&2 else 'x21'
            target = start + (12 if i&4 else 8)
            body = ['mov %s, #0' % destination,'b.eq %x' % target,
                    'ldr x15, [%s]' % base,'br x15']
            code = self.instructions(body,start)
            self.assertEqual(cache.check(code),precise.analyse(code))
        self.assertGreater(cache.hits,0)


if __name__ == '__main__':
    unittest.main()
