#!/usr/bin/env python3
"""Volumes, disguise, nested extract, and ordered password checks.

usage: test_product.py <archiver-cli> <engine> <7zz> [rar]
"""

import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

CLI = sys.argv[1]
ENGINE = sys.argv[2]
SEVEN = sys.argv[3]
RAR = sys.argv[4] if len(sys.argv) > 4 else ""


def run(args, check=True):
    result = subprocess.run(
        [CLI, *args, "--engine", ENGINE],
        input=b"",
        capture_output=True,
        timeout=60,
    )
    if check and result.returncode != 0:
        raise SystemExit(
            "fail %s\ncode %s\n%s\n%s" % (args, result.returncode, result.stdout, result.stderr)
        )
    return result


def main():
    tmp = tempfile.mkdtemp(prefix="product-")
    try:
        hello = os.path.join(tmp, "hello.txt")
        with open(hello, "w", encoding="utf-8") as fh:
            fh.write("nested-ok\n")

        # ZIP renamed to .mp4, and a ZIP concatenated after a tiny MP4.
        renamed = os.path.join(tmp, "renamed.mp4")
        with zipfile.ZipFile(renamed, "w") as zf:
            zf.write(hello, "dir/hello.txt")
        listed = run(["list", renamed])
        if b"dir/hello.txt" not in listed.stdout:
            raise SystemExit("renamed mp4 did not list")
        dest = os.path.join(tmp, "renamed-out")
        os.makedirs(dest)
        run(["extract", renamed, dest])
        with open(os.path.join(dest, "dir", "hello.txt"), encoding="utf-8") as fh:
            if fh.read() != "nested-ok\n":
                raise SystemExit("renamed mp4 extract mismatch")
        print("ok zip renamed to mp4")

        carrier = os.path.join(tmp, "carrier.mp4")
        payload = os.path.join(tmp, "payload.zip")
        with zipfile.ZipFile(payload, "w") as zf:
            zf.write(hello, "dir/hello.txt")
        with open(carrier, "wb") as out:
            out.write(b"\x00\x00\x00\x18ftypmp42\x00\x00\x00\x00mp42isom")
            out.write(b"\x00" * 64)
            with open(payload, "rb") as src:
                out.write(src.read())
        listed = run(["list", carrier])
        if b"dir/hello.txt" not in listed.stdout:
            raise SystemExit("appended mp4 did not list: %s" % listed.stdout)
        dest = os.path.join(tmp, "carrier-out")
        os.makedirs(dest)
        run(["extract", carrier, dest])
        print("ok zip appended to mp4")

        plain = os.path.join(tmp, "plain.mp4")
        with open(plain, "wb") as out:
            out.write(b"\x00\x00\x00\x18ftypmp42\x00\x00\x00\x00mp42isom")
            out.write(b"\x00" * 32)
        plain_result = run(["list", plain], check=False)
        if plain_result.returncode != 8:
            raise SystemExit("plain mp4 should be unsupported, got %s" % plain_result.returncode)
        print("ok plain mp4 rejected")

        # Two-level nested archive.
        inner = os.path.join(tmp, "inner.zip")
        with zipfile.ZipFile(inner, "w") as zf:
            zf.write(hello, "hello.txt")
        outer = os.path.join(tmp, "outer.zip")
        with zipfile.ZipFile(outer, "w") as zf:
            zf.write(inner, "inner.zip")
        nest_dest = os.path.join(tmp, "nest-out")
        os.makedirs(nest_dest)
        nested = run(["nested", outer, nest_dest])
        hello_out = os.path.join(nest_dest, "hello.txt")
        if not os.path.exists(hello_out):
            raise SystemExit("nested extract missing hello: %s" % nested.stdout)
        with open(hello_out, encoding="utf-8") as fh:
            if fh.read() != "nested-ok\n":
                raise SystemExit("nested contents mismatch")
        if os.path.exists(os.path.join(nest_dest, "inner.zip")):
            raise SystemExit("intermediate archive was left behind")
        print("ok two-level nested extract")

        # Two saved passwords. The second one matches. stdin stays empty.
        book = os.path.join(tmp, "book.txt")
        first = "not-the-password"
        second = "the-real-password"
        with open(book, "w", encoding="utf-8") as fh:
            fh.write(first + "\n" + second + "\n")
        enc_src = os.path.join(tmp, "secret.txt")
        with open(enc_src, "w", encoding="utf-8") as fh:
            fh.write("secret\n")
        enc = os.path.join(tmp, "enc.7z")
        made = subprocess.run(
            [SEVEN, "a", "-t7z", "-mhe=on", "-p" + second, enc, enc_src],
            capture_output=True,
            timeout=30,
        )
        if made.returncode != 0:
            raise SystemExit("could not create encrypted 7z: %s" % made.stderr)
        tried = run(["try-passwords", enc, book])
        if tried.stdout.strip() != b"matched 1":
            raise SystemExit("password order failed: %s" % tried.stdout)
        if first.encode() in tried.stdout + tried.stderr or second.encode() in tried.stdout + tried.stderr:
            raise SystemExit("password leaked")
        print("ok password book order matched the second password")

        wrong = os.path.join(tmp, "wrong.txt")
        with open(wrong, "w", encoding="utf-8") as fh:
            fh.write("one-wrong\nanother-wrong\n")
        missed = run(["try-passwords", enc, wrong], check=False)
        if missed.returncode != 5:
            raise SystemExit("two wrong passwords should ask, code %s %s" % (missed.returncode, missed.stdout))
        if b"one-wrong" in missed.stdout + missed.stderr:
            raise SystemExit("wrong password leaked")
        print("ok two wrong passwords stop with password-required")

        # 7z volumes and a missing volume.
        vol_src = os.path.join(tmp, "vol.txt")
        vol_payload = os.urandom(20000)
        with open(vol_src, "wb") as fh:
            fh.write(vol_payload)
        vol_base = os.path.join(tmp, "parts.7z")
        made = subprocess.run(
            [SEVEN, "a", "-t7z", "-mx=0", "-v4k", vol_base, vol_src],
            capture_output=True,
            timeout=30,
        )
        if made.returncode != 0:
            raise SystemExit("could not create 7z volumes: %s" % made.stderr)
        first_vol = vol_base + ".001"
        if not os.path.exists(first_vol):
            raise SystemExit("missing %s" % first_vol)
        listed = run(["list", first_vol])
        if b"vol.txt" not in listed.stdout:
            raise SystemExit("7z volume list failed: %s" % listed.stdout)
        vol_dest = os.path.join(tmp, "vol-out")
        os.makedirs(vol_dest)
        run(["extract", first_vol, vol_dest])
        with open(os.path.join(vol_dest, "vol.txt"), "rb") as fh:
            if fh.read() != vol_payload:
                raise SystemExit("7z volume extract mismatch")
        print("ok 7z volumes")

        names = sorted(name for name in os.listdir(tmp) if name.startswith("parts.7z."))
        if len(names) < 2:
            raise SystemExit("expected multiple volumes, got %s" % names)
        removed = names[1]
        os.remove(os.path.join(tmp, removed))
        missing = run(["list", first_vol], check=False)
        text = missing.stdout + missing.stderr
        if missing.returncode != 10 or removed.encode() not in text:
            raise SystemExit("missing volume not named (%s): %s %s" % (missing.returncode, text, removed))
        print("ok missing 7z volume names", removed)

        # ZIP split volumes via 7zz.
        zip_base = os.path.join(tmp, "split.zip")
        made = subprocess.run(
            [SEVEN, "a", "-tzip", "-mx=0", "-v4k", zip_base, vol_src],
            capture_output=True,
            timeout=30,
        )
        if made.returncode != 0:
            raise SystemExit("could not create zip volumes: %s" % made.stderr)
        zip_first = zip_base + ".001"
        listed = run(["list", zip_first])
        if b"vol.txt" not in listed.stdout:
            raise SystemExit("zip volume list failed: %s" % listed.stdout)
        print("ok zip volumes")
        zip_names = sorted(name for name in os.listdir(tmp) if name.startswith("split.zip."))
        if len(zip_names) < 2:
            raise SystemExit("expected multiple zip volumes, got %s" % zip_names)
        os.remove(os.path.join(tmp, zip_names[1]))
        missing = run(["list", zip_first], check=False)
        blob = missing.stdout + missing.stderr
        if missing.returncode != 10 or zip_names[1].encode() not in blob:
            raise SystemExit("missing zip volume not named (%s): %s" % (missing.returncode, blob))
        print("ok missing zip volume names", zip_names[1])

        # Nine nested layers hit the depth limit on the last one.
        current = hello
        deepest = os.path.join(tmp, "level0.zip")
        with zipfile.ZipFile(deepest, "w") as zf:
            zf.write(hello, "hello.txt")
        current = deepest
        for level in range(1, 9):
            nxt = os.path.join(tmp, "level%d.zip" % level)
            with zipfile.ZipFile(nxt, "w") as zf:
                zf.write(current, os.path.basename(current))
            current = nxt
        deep_dest = os.path.join(tmp, "deep-out")
        os.makedirs(deep_dest)
        limited = run(["nested", current, deep_dest], check=False)
        if limited.returncode != 12 or b"nested depth limit" not in limited.stdout:
            raise SystemExit("depth limit failed %s %s" % (limited.returncode, limited.stdout))
        print("ok nested depth limit")

        # Two archives in one layer stop and name both.
        choice = os.path.join(tmp, "choice.zip")
        with zipfile.ZipFile(choice, "w") as zf:
            zf.write(inner, "a.zip")
            zf.write(inner, "b.zip")
        choice_dest = os.path.join(tmp, "choice-out")
        os.makedirs(choice_dest)
        choose = run(["nested", choice, choice_dest], check=False)
        if choose.returncode != 11 or b"a.zip" not in choose.stdout or b"b.zip" not in choose.stdout:
            raise SystemExit("need-choice failed %s %s" % (choose.returncode, choose.stdout))
        print("ok nested choice stop")

        if RAR and os.path.exists(RAR):
            rar_dir = os.path.join(tmp, "rarvol")
            os.makedirs(rar_dir)
            rar_src = os.path.join(rar_dir, "note.txt")
            with open(rar_src, "wb") as fh:
                fh.write(os.urandom(20000))
            rar_arc = os.path.join(rar_dir, "bundle.part1.rar")
            # Official rar uses part1.rar when -v is set.
            made = subprocess.run(
                [RAR, "a", "-m0", "-v4k", "-ep", rar_arc, rar_src],
                capture_output=True,
                timeout=30,
            )
            if made.returncode != 0:
                print("skip rar volumes:", made.stderr.decode("utf-8", "replace")[:400])
            else:
                parts = sorted(name for name in os.listdir(rar_dir) if ".part" in name and name.endswith(".rar"))
                if not parts:
                    raise SystemExit("rar created no parts")
                listed = run(["list", os.path.join(rar_dir, parts[0])])
                if b"note.txt" not in listed.stdout:
                    raise SystemExit("rar volume list failed: %s %s" % (parts, listed.stdout))
                print("ok rar volumes", parts)
                if len(parts) >= 2:
                    gone = parts[-1]
                    os.remove(os.path.join(rar_dir, gone))
                    missing = run(["list", os.path.join(rar_dir, parts[0])], check=False)
                    blob = missing.stdout + missing.stderr
                    if missing.returncode != 10 or gone.encode() not in blob:
                        raise SystemExit(
                            "missing rar volume not named (%s): %s %s" % (missing.returncode, blob, gone)
                        )
                    print("ok missing rar volume names", gone)
        else:
            print("skip rar volumes: rar tool not passed")

        # Stored ZIP created by the core, then listed.
        made_zip = os.path.join(tmp, "made.zip")
        run(["create-zip", made_zip, hello])
        listed = run(["list", made_zip])
        if b"hello.txt" not in listed.stdout:
            raise SystemExit("create-zip did not list: %s" % listed.stdout)
        print("ok create-zip")
        print("product tests passed")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
