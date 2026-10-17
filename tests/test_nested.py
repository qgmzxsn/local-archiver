#!/usr/bin/env python3
"""Exercise the public nested API against real archives, including in-place prompts."""
import bz2
import ctypes as C
import gzip
import io
import lzma
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile

LIB, ENGINE, SEVEN = map(lambda s: str(Path(s).resolve()), sys.argv[1:4])
del sys.argv[1:]
lib = C.CDLL(LIB)
CHOOSE = C.CFUNCTYPE(C.c_int, C.POINTER(C.c_char_p), C.c_uint32, C.c_void_p)
PASSWORD = C.CFUNCTYPE(C.c_int, C.c_char_p, C.c_int, C.c_void_p, C.c_uint32, C.c_void_p)
ACCEPTED = C.CFUNCTYPE(None, C.c_char_p, C.c_void_p)
PROGRESS = C.CFUNCTYPE(C.c_int, C.c_uint64, C.c_uint64, C.c_void_p)

class Options(C.Structure):
    _fields_ = [('depth', C.c_uint32), ('bytes', C.c_uint64), ('keep', C.c_int),
                ('all', C.c_int), ('choose', CHOOSE), ('password', PASSWORD),
                ('accepted', ACCEPTED), ('user', C.c_void_p)]

lib.archiver_extract_nested_ex.argtypes = [C.c_char_p, C.c_char_p, C.c_char_p,
    C.POINTER(C.c_char_p), C.c_uint32, C.POINTER(Options), PROGRESS, C.c_void_p,
    C.c_char_p, C.c_uint32]


def zip_bytes(members):
    out = io.BytesIO()
    with zipfile.ZipFile(out, 'w') as z:
        for name, data in members.items():
            z.writestr(name, data)
    return out.getvalue()


class NestedTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.options = Options(32, 64 << 30, 0, 1, CHOOSE(), PASSWORD(), ACCEPTED(), None)
        self.refs = []

    def archive(self, name, members):
        path = self.root / name
        path.write_bytes(zip_bytes(members))
        return path

    def run_extract(self, archive, passwords=(), progress=None):
        dest = self.root / 'out'
        dest.mkdir(exist_ok=True)
        message = C.create_string_buffer(4096)
        views = (C.c_char_p * len(passwords))(*(p.encode() for p in passwords))
        callback = PROGRESS(progress) if progress else PROGRESS()
        code = lib.archiver_extract_nested_ex(os.fsencode(archive), os.fsencode(ENGINE), os.fsencode(dest),
            views, len(passwords), C.byref(self.options), callback, None, message, len(message))
        return code, message.value.decode(), dest

    def encrypted(self, name='secret.7z', password='内层🔑'):
        source = self.root / 'hello.txt'
        source.write_bytes(b'private payload')
        path = self.root / name
        subprocess.run([SEVEN, 'a', '-t7z', '-mhe=on', '-p' + password, str(path), str(source)],
                       check=True, capture_output=True)
        return path.read_bytes()

    def test_all_branches_use_separate_folders(self):
        archive = self.archive('outer.zip', {'a.zip': zip_bytes({'hello.txt': b'A'}),
                                            'b.zip': zip_bytes({'hello.txt': b'B'}), 'note.txt': b'keep'})
        code, _, out = self.run_extract(archive)
        self.assertEqual(code, 0)
        self.assertEqual((out / 'a.zip.unpacked/hello.txt').read_bytes(), b'A')
        self.assertEqual((out / 'b.zip.unpacked/hello.txt').read_bytes(), b'B')
        self.assertFalse((out / 'a.zip').exists())
        self.assertTrue(archive.exists())
        self.assertEqual((out / 'note.txt').read_bytes(), b'keep')

    def test_chosen_depth_is_success(self):
        archive = self.archive('outer.zip', {'inner.zip': zip_bytes({'hello.txt': b'ok'})})
        self.options.depth = 1
        code, _, out = self.run_extract(archive)
        self.assertEqual(code, 0)
        self.assertTrue((out / 'inner.zip').exists())
        self.assertFalse((out / 'hello.txt').exists())

    def test_keep_intermediate(self):
        archive = self.archive('outer.zip', {'inner.zip': zip_bytes({'hello.txt': b'ok'})})
        self.options.keep = 1
        code, _, out = self.run_extract(archive)
        self.assertEqual(code, 0)
        self.assertTrue((out / 'inner.zip').exists())
        self.assertEqual((out / 'hello.txt').read_bytes(), b'ok')

    def test_prompt_retries_current_layer_and_caches_verified_password(self):
        encrypted = self.encrypted()
        archive = self.archive('outer.zip', {'a.7z': encrypted, 'b.7z': encrypted, 'note.txt': b'outer'})
        attempts = []
        accepted = []
        def password(path, rejected, buffer, capacity, user):
            attempts.append((Path(os.fsdecode(path)).name, rejected))
            value = b'wrong' if len(attempts) == 1 else '内层🔑'.encode()
            C.memmove(buffer, value + b'\0', len(value) + 1)
            return 1
        self.options.password = PASSWORD(password)
        self.options.accepted = ACCEPTED(lambda pw, user: accepted.append(pw.decode()))
        code, _, out = self.run_extract(archive, ['saved-wrong'])
        self.assertEqual(code, 0)
        self.assertEqual(attempts, [('a.7z', 0), ('a.7z', 1)])
        self.assertEqual(accepted, ['内层🔑'])
        self.assertEqual((out / 'note.txt').read_bytes(), b'outer')
        self.assertEqual((out / 'b.7z.unpacked/hello.txt').read_bytes(), b'private payload')

    def test_saved_passwords_precede_prompt(self):
        archive = self.archive('outer.zip', {'secret.7z': self.encrypted()})
        called = []
        self.options.password = PASSWORD(lambda *args: called.append(True) or 0)
        code, _, out = self.run_extract(archive, ['wrong', '内层🔑'])
        self.assertEqual(code, 0)
        self.assertEqual(called, [])
        self.assertTrue((out / 'hello.txt').exists())

    def test_cancel_password_preserves_current_layer(self):
        archive = self.archive('outer.zip', {'secret.7z': self.encrypted(), 'note.txt': b'outer'})
        self.options.password = PASSWORD(lambda *args: 0)
        code, _, out = self.run_extract(archive)
        self.assertEqual(code, 9)
        self.assertTrue((out / 'secret.7z').exists())
        self.assertEqual((out / 'note.txt').read_bytes(), b'outer')

    def test_decision_can_stop_or_choose_one(self):
        archive = self.archive('outer.zip', {'a.zip': zip_bytes({'x.txt': b'A'}), 'b.zip': zip_bytes({'x.txt': b'B'})})
        seen = []
        def choose(paths, count, user):
            seen.extend(paths[i].decode() for i in range(count))
            return 1
        self.options.choose = CHOOSE(choose)
        code, _, out = self.run_extract(archive)
        self.assertEqual(code, 0)
        self.assertEqual(seen, ['a.zip', 'b.zip'])
        self.assertTrue((out / 'a.zip').exists())
        self.assertEqual((out / 'b.zip.unpacked/x.txt').read_bytes(), b'B')

    def test_stop_keeps_unopened_archive(self):
        archive = self.archive('outer.zip', {'inner.zip': zip_bytes({'x': b'X'})})
        self.options.choose = CHOOSE(lambda *args: -1)
        code, _, out = self.run_extract(archive)
        self.assertEqual(code, 0)
        self.assertTrue((out / 'inner.zip').exists())
        self.assertFalse((out / 'x').exists())

    def test_only_new_entries_are_examined(self):
        out = self.root / 'out'
        out.mkdir()
        (out / 'unrelated.zip').write_bytes(zip_bytes({'unexpected': b'no'}))
        archive = self.archive('outer.zip', {'readme.txt': b'yes'})
        code, _, out = self.run_extract(archive)
        self.assertEqual(code, 0)
        self.assertFalse((out / 'unexpected').exists())
        self.assertTrue((out / 'unrelated.zip').exists())

    def test_byte_limit(self):
        archive = self.archive('outer.zip', {'large.bin': b'x' * 5000})
        self.options.bytes = 4000
        code, message, out = self.run_extract(archive)
        self.assertEqual(code, 12)
        self.assertIn('byte limit', message)
        self.assertFalse((out / 'large.bin').exists())

    def test_cancel_before_output(self):
        archive = self.archive('outer.zip', {'file': b'ok'})
        code, _, out = self.run_extract(archive, progress=lambda *args: 1)
        self.assertEqual(code, 9)
        self.assertEqual(list(out.iterdir()), [])

    def test_nested_split_group_is_extracted_once(self):
        source = self.root / 'payload.bin'
        source.write_bytes(os.urandom(20000))
        base = self.root / 'parts.7z'
        subprocess.run([SEVEN, 'a', '-t7z', '-mx=0', '-v4k', str(base), str(source)], check=True, capture_output=True)
        members = {p.name: p.read_bytes() for p in sorted(self.root.glob('parts.7z.*'))}
        archive = self.archive('outer.zip', members)
        code, message, out = self.run_extract(archive)
        self.assertEqual(code, 0, message)
        self.assertEqual((out / 'payload.bin').read_bytes(), source.read_bytes())
        self.assertTrue((out / 'parts.7z.001').exists())

    def test_standalone_compression_has_a_safe_output_name(self):
        for ext, compress in [('gz', gzip.compress), ('bz2', bz2.compress), ('xz', lzma.compress)]:
            with self.subTest(ext=ext):
                archive = self.root / ('sample.txt.' + ext)
                archive.write_bytes(compress(b'normal content'))
                code, message, out = self.run_extract(archive)
                self.assertEqual(code, 0, message)
                self.assertEqual((out / 'sample.txt').read_bytes(), b'normal content')
                (out / 'sample.txt').unlink()

    def test_nonleading_volume_routes_to_first(self):
        source = self.root / 'payload.bin'
        source.write_bytes(os.urandom(14000))
        base = self.root / 'parts.7z'
        subprocess.run([SEVEN, 'a', '-t7z', '-mx=0', '-v4k', str(base), str(source)], check=True, capture_output=True)
        code, message, out = self.run_extract(str(base) + '.002')
        self.assertEqual(code, 0, message)
        self.assertEqual((out / 'payload.bin').read_bytes(), source.read_bytes())

    def test_missing_leading_volume_is_named(self):
        archive = self.root / 'parts.7z.002'
        archive.write_bytes(b'not the first volume')
        code, message, out = self.run_extract(archive)
        self.assertEqual(code, 10)
        self.assertIn('parts.7z.001', message)

    def test_zstandard_unknown_size_and_runtime_limit(self):
        payload = b'zstd contents'
        # A valid raw Zstandard frame without a declared content size.
        frame = bytes.fromhex('28b52ffd0000')
        frame += ((len(payload) << 3) | 1).to_bytes(3, 'little') + payload
        archive = self.root / 'sample.txt.zst'
        archive.write_bytes(frame)
        self.options.bytes = 4
        code, message, out = self.run_extract(archive)
        self.assertEqual(code, 12, message)
        self.assertFalse((out / 'sample.txt').exists())
        self.options.bytes = 1024
        code, message, out = self.run_extract(archive)
        self.assertEqual(code, 0, message)
        self.assertEqual((out / 'sample.txt').read_bytes(), payload)

    def test_ar_format(self):
        payload = b'ar contents'
        header = ('%-16s%-12s%-6s%-6s%-8s%-10s' %
                  ('hello.txt/', '0', '0', '0', '100644', str(len(payload)))).encode() + b'`\n'
        archive = self.root / 'sample.ar'
        archive.write_bytes(b'!<arch>\n' + header + payload + (b'\n' if len(payload) % 2 else b''))
        code, message, out = self.run_extract(archive)
        self.assertEqual(code, 0, message)
        self.assertEqual((out / 'hello.txt').read_bytes(), payload)


if __name__ == '__main__':
    unittest.main()
