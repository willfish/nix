#!/usr/bin/env python3
"""CLI, accounting, terminal and live mapping checks (stdlib only)."""

import errno
import fcntl
import mmap
import os
from pathlib import Path
import pty
import re
import struct
import subprocess
import sys
import tempfile
import termios
import time
import unittest

BIN = str(Path(sys.argv.pop(1)).resolve())


def mapping(
    inode=1,
    path="/models/weights.gguf",
    pss=1048576,
    flags="rd mr mw me",
    anon=0,
):
    return (
        f"10000000-90000000 r--p 00000000 08:01 {inode} {path}\n"
        f"Size: 2097152 kB\nRss: {pss} kB\nPss: {pss} kB\n"
        f"Anonymous: {anon} kB\nShared_Hugetlb: 0 kB\nPrivate_Hugetlb: 0 kB\n"
        f"VmFlags: {flags}\n"
    )


class CliTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root / "meminfo").write_text(
            "MemTotal: 33554432 kB\nMemAvailable: 12582912 kB\n"
            "AnonPages: 16777216 kB\n"
        )
        self.proc(10, "llama-server", mapping())
        self.proc(11, "browser", mapping(2, "/cache/media-cache.db", 524288))
        self.proc(12, "audio-server", mapping(3, "/lib/libcuda.so.1", 262144))
        self.proc(13, "other", mapping(4, "/lib/other.so", 262144))

    def tearDown(self):
        self.tmp.cleanup()

    def proc(self, pid, name, smaps):
        p = self.root / str(pid)
        p.mkdir(exist_ok=True)
        (p / "comm").write_text(name + "\n")
        (p / "smaps").write_text(smaps)
        return p

    def run_cli(self, *args, env=None):
        e = dict(os.environ, NO_COLOR="", TERM="xterm-256color")
        e.update(env or {})
        return subprocess.run(
            [BIN, "--proc-root", str(self.root), *args],
            text=True,
            capture_output=True,
            env=e,
            check=False,
        )

    def output(self, *args, **kw):
        result = self.run_cli(*args, **kw)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def test_headline_and_denominators(self):
        out = self.output("--limit", "3")
        self.assertIn("20.0 GiB / 32.0 GiB used   62.5%", out)
        self.assertIn("12.0 GiB available", out)
        self.assertIn("2.0 GiB PSS | 6.25% of total RAM", out)
        self.assertRegex(out, r"weights.gguf.*50.0%")
        self.assertRegex(out, r"Other observed mappings.*12.5%")
        one = self.output("--limit", "1")
        self.assertRegex(one, r"weights.gguf.*50.0%")
        self.assertRegex(one, r"Other observed mappings.*50.0%")
        self.assertNotIn("\x1b", out)

    def test_identity_aliases_and_deleted(self):
        self.proc(14, "second", mapping(1, "/alias/model (deleted)", 1048576))
        out = self.output()
        self.assertIn("3.0 GiB PSS", out)
        self.assertRegex(out, r"66.7%")
        self.assertIn("[deleted]", out)
        self.assertIn("second", out)
        self.proc(15, "different", mapping(99, "/another/weights.gguf", 200000))
        self.assertIn("identity", self.output())

    def test_private_copy_not_subtracted(self):
        self.proc(10, "llama-server", mapping(anon=524288))
        self.assertIn("2.0 GiB PSS", self.output())

    def test_special_mappings_excluded(self):
        self.proc(
            20, "device", mapping(200, "/dev/example", 1048576, "rd io pf")
        )
        out = self.output()
        self.assertIn("2.0 GiB PSS", out)
        self.assertIn("1 special mappings excluded", out)

    def test_shmem_identified(self):
        self.proc(10, "shared", mapping(1, "/memfd:example (deleted)"))
        self.assertIn("[shmem]", self.output())

    def test_future_fields_and_empty_or_vanished_process(self):
        # A-F are hexadecimal characters but these names are not VMA headers.
        fields = (
            "AnonHugePages: 0 kB\nFilePmdMapped: 0 kB\nFuture_Field: 123 kB\n"
        )
        self.proc(
            10,
            "llama-server",
            mapping().replace("VmFlags:", fields + "VmFlags:"),
        )
        self.proc(90, "kernel-thread", "")
        vanished = self.proc(91, "exited", "")
        (vanished / "smaps").unlink()
        out = self.output()
        self.assertIn("2.0 GiB PSS", out)
        self.assertIn("4 read", out)
        self.assertIn("1 vanished", out)
        self.assertNotIn("invalid process", out)

    def test_pss_overflow_discards_whole_process(self):
        self.proc(
            10,
            "bad",
            mapping(pss=18446744073709551615) + mapping(inode=20, pss=1),
        )
        out = self.output()
        self.assertIn("1 invalid", out)
        self.assertIn("1.0 GiB PSS", out)
        self.assertNotIn("weights.gguf", out)

    def test_clipped_names_have_identity(self):
        suffix = "same-suffix-012345678901234567890"
        self.proc(10, "first", mapping(1, "/models/first-" + suffix))
        self.proc(11, "second", mapping(2, "/models/second-" + suffix, 524288))
        out = self.output("--width", "80")
        self.assertIn("identity 8:1:1", out)
        self.assertIn("identity 8:1:2", out)

    def test_embedded_newline_in_comm(self):
        self.proc(10, "worker\nA", mapping())
        self.assertIn("worker\\x0aA", self.output())

    def test_full_paths(self):
        name = "/a/very/long/path/" + "component/" * 10 + "weights.gguf"
        self.proc(10, "llama-server", mapping(path=name))
        out = self.output("--full-paths")
        self.assertIn(name, "".join(out.splitlines()))

    def test_missing_available_no_free_fallback(self):
        (self.root / "meminfo").write_text(
            "MemTotal: 33554432 kB\nMemFree: 123 kB\n"
        )
        self.assertIn("used/available unavailable", self.output())

    def test_bad_total_and_overflow(self):
        for content in [
            "MemTotal: 0 kB\n",
            "MemTotal: 18446744073709551616 kB\n",
            "MemTotal: -1 kB\n",
            "MemTotal: 32 MB\n",
        ]:
            with self.subTest(content=content):
                (self.root / "meminfo").write_text(content)
                self.assertNotEqual(self.run_cli().returncode, 0)

    def test_transactional_malformed_process(self):
        self.proc(
            10, "bad", mapping() + mapping(19, pss=9999).split("VmFlags:")[0]
        )
        out = self.output()
        self.assertIn("1 invalid", out)
        self.assertIn("1.0 GiB PSS", out)
        self.assertNotIn("weights.gguf", out)

    def test_empty_proc_tree(self):
        for p in self.root.glob("[0-9]*"):
            for child in p.iterdir():
                child.unlink()
            p.rmdir()
        self.assertIn("Mapping PSS unavailable", self.output())

    def test_attachment_ellipsis_and_expansion(self):
        for pid in range(20, 28):
            self.proc(pid, f"process-{pid}-with-a-long-name", mapping(pss=1024))
        compact = self.output("--width", "40")
        self.assertIn("...", compact)
        self.assertTrue(all(len(line) <= 40 for line in compact.splitlines()))
        expanded = self.output("--width", "40", "--full-paths")
        self.assertIn("process-20-with-a-long-name", expanded)

    def test_zero_and_unavailable(self):
        for p in self.root.glob("[0-9]*/smaps"):
            p.write_text(mapping(pss=0))
        out = self.output()
        self.assertIn("No resident PSS", out)
        self.assertNotIn("nan", out.lower())
        for p in self.root.glob("[0-9]*/smaps"):
            p.write_text("bad\n")
        self.assertIn("Mapping PSS unavailable", self.output())

    @unittest.skipIf(os.getuid() == 0, "root bypasses fixture permissions")
    def test_permissions(self):
        target = self.root / "10/smaps"
        target.chmod(0)
        try:
            out = self.output()
            self.assertIn("1 denied", out)
            self.assertIn("PARTIAL", out)
            self.assertNotIn("weights.gguf", out)
        finally:
            target.chmod(0o600)

    def test_terminal_injection_and_width(self):
        self.proc(
            10, "bad\x1b[2Jname", mapping(path="/evil/\x1b[31mweights.gguf")
        )
        for width in (8, 20, 40, 60, 80, 120):
            with self.subTest(width=width):
                out = self.output("--width", str(width))
                self.assertNotIn("\x1b", out)
                self.assertTrue(
                    all(len(line) <= width for line in out.splitlines()), out
                )
        self.assertIn("\\x1b", self.output())

    def test_color_contract_and_options(self):
        styled = self.output("--color", "always")
        for sgr in ("34", "0;35", "36", "32", "1", "1;39"):
            self.assertIn(f"\x1b[{sgr}m", styled)
        unstyled = re.sub(r"\x1b\[[0-9;]*m", "", styled)
        plain = self.output("--color", "never")
        # Elapsed time varies between otherwise identical samples.
        self.assertEqual(
            re.sub(r"\d+ ms sample", "TIME", unstyled),
            re.sub(r"\d+ ms sample", "TIME", plain),
        )
        self.assertNotIn(
            "\x1b", self.output("--color", "always", env={"NO_COLOR": "1"})
        )
        self.assertNotIn("\x1b", self.output(env={"TERM": "dumb"}))
        for args in [
            ("--limit", "0"),
            ("--limit", "-1"),
            ("--width", "999"),
            ("--color", "invalid"),
            ("unexpected",),
        ]:
            self.assertEqual(self.run_cli(*args).returncode, 2)
        self.assertIn("memscope 0.1.0", self.output("--version"))
        self.assertIn("MemTotal - MemAvailable", self.output("--help"))

    def test_pty_width_and_unicode(self):
        for width in (40, 60, 80, 120):
            master, slave = pty.openpty()
            try:
                fcntl.ioctl(
                    slave,
                    termios.TIOCSWINSZ,
                    struct.pack("HHHH", 40, width, 0, 0),
                )
                env = dict(
                    os.environ,
                    TERM="xterm-256color",
                    LC_ALL="C.UTF-8",
                    NO_COLOR="",
                )
                p = subprocess.Popen(
                    [BIN, "--proc-root", str(self.root)],
                    stdout=slave,
                    stderr=subprocess.PIPE,
                    env=env,
                )
                os.close(slave)
                slave = -1
                chunks = []
                while True:
                    try:
                        chunk = os.read(master, 65536)
                        if not chunk:
                            break
                        chunks.append(chunk)
                    except OSError:
                        break
                self.assertEqual(p.wait(timeout=10), 0)
                p.stderr.close()
                out = b"".join(chunks).decode()
                self.assertIn("\x1b[34m", out)
                self.assertIn("█", out)
                plain = re.sub(r"\x1b\[[0-9;]*m", "", out)
                self.assertTrue(
                    all(len(line) <= width for line in plain.splitlines()),
                    plain,
                )
            finally:
                os.close(master)
                if slave >= 0:
                    os.close(slave)


class LiveMappingTests(unittest.TestCase):
    def test_process_exits_after_pid_enumeration(self):
        # A FIFO pauses collection after the PID list has been read. Terminate
        # a real later process before releasing the first smaps read.
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "meminfo").write_text("MemTotal: 33554432 kB\n")
            first = root / "1"
            first.mkdir()
            (first / "comm").write_text("barrier\n")
            os.mkfifo(first / "smaps")
            child = subprocess.Popen(
                [sys.executable, "-c", "import time; time.sleep(30)"]
            )
            os.symlink(f"/proc/{child.pid}", root / str(child.pid))
            collector = subprocess.Popen(
                [BIN, "--proc-root", str(root)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            writer = None
            try:
                deadline = time.monotonic() + 5
                while writer is None:
                    try:
                        writer = os.open(
                            first / "smaps", os.O_WRONLY | os.O_NONBLOCK
                        )
                    except OSError as error:
                        if (
                            error.errno != errno.ENXIO
                            or time.monotonic() >= deadline
                        ):
                            raise
                        time.sleep(0.01)
                child.terminate()
                child.wait(timeout=5)
                os.write(writer, mapping().encode())
                os.close(writer)
                writer = None
                out, err = collector.communicate(timeout=5)
                self.assertEqual(collector.returncode, 0, err)
                self.assertIn("1 read", out)
                self.assertIn("1 vanished", out)
                self.assertIn("PARTIAL", out)
            finally:
                if writer is not None:
                    os.close(writer)
                if child.poll() is None:
                    child.kill()
                child.wait()
                if collector.poll() is None:
                    collector.kill()
                collector.communicate()

    def test_shared_and_private_copy_and_unlink(self):
        # Two mappings in this process share resident physical pages; PSS must
        # not double count them. A private write then creates an extra copy.
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "meminfo").write_text(Path("/proc/meminfo").read_text())
            os.symlink(f"/proc/{os.getpid()}", root / str(os.getpid()))
            path = root / "memscope-live-fixture"
            n = 8 * 1024 * 1024
            with path.open("w+b") as f:
                f.truncate(n)
                with mmap.mmap(
                    f.fileno(), n, access=mmap.ACCESS_WRITE
                ) as shared:
                    shared[:] = b"x" * n
                    with mmap.mmap(
                        f.fileno(), n, access=mmap.ACCESS_COPY
                    ) as private:
                        for i in range(0, n, 4096):
                            self.assertEqual(private[i], ord("x"))
                        # Disable THP for deterministic accounting if supported.
                        if hasattr(private, "madvise"):
                            private.madvise(mmap.MADV_NOHUGEPAGE)
                        before = self.file_pss(path)
                        self.assertTrue(8000 <= before <= 8300, before)
                        private[:] = b"y" * n
                        after = self.file_pss(path)
                        self.assertTrue(16000 <= after <= 16600, after)
                        path.unlink()
                        out = subprocess.check_output(
                            [BIN, "--proc-root", str(root), "--limit", "1000"],
                            text=True,
                        )
                        self.assertIn("memscope-live-fixture", out)
                        self.assertIn("[deleted]", out)
                        row = next(
                            line
                            for line in out.splitlines()
                            if "memscope-live-fixture" in line
                        )
                        self.assertIn("16 MiB", row)

    def test_shared_file_across_processes(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "meminfo").write_text(Path("/proc/meminfo").read_text())
            os.symlink(f"/proc/{os.getpid()}", root / str(os.getpid()))
            path = root / "shared-live-fixture"
            with path.open("w+b") as f:
                f.truncate(8 * 1024 * 1024)
                with mmap.mmap(f.fileno(), 0) as shared:
                    shared[:] = b"x" * len(shared)
                    script = (
                        "import mmap,sys; "
                        "f=open(sys.argv[1],'r+b'); m=mmap.mmap(f.fileno(),0); "
                        "data=m[::4096]; print('ready',flush=True); "
                        "sys.stdin.read(1)"
                    )
                    child = subprocess.Popen(
                        [sys.executable, "-c", script, str(path)],
                        stdin=subprocess.PIPE,
                        stdout=subprocess.PIPE,
                        text=True,
                    )
                    try:
                        self.assertEqual(
                            child.stdout.readline().strip(), "ready"
                        )
                        os.symlink(f"/proc/{child.pid}", root / str(child.pid))
                        out = subprocess.check_output(
                            [BIN, "--proc-root", str(root), "--limit", "1000"],
                            text=True,
                        )
                        row = next(
                            x
                            for x in out.splitlines()
                            if "shared-live-fixture" in x
                        )
                        self.assertIn(
                            "8 MiB", row
                        )  # not 16 MiB RSS double counting
                        self.assertIn("2 read", out)
                    finally:
                        child.communicate("x", timeout=10)

    @unittest.skipUnless(hasattr(os, "memfd_create"), "Linux memfd required")
    def test_live_memfd(self):
        fd = os.memfd_create("memscope-memfd")
        try:
            os.ftruncate(fd, 4 * 1024 * 1024)
            with (
                mmap.mmap(fd, 0) as shared,
                tempfile.TemporaryDirectory() as temp,
            ):
                shared[:] = b"x" * len(shared)
                root = Path(temp)
                (root / "meminfo").write_text(Path("/proc/meminfo").read_text())
                os.symlink(f"/proc/{os.getpid()}", root / str(os.getpid()))
                out = subprocess.check_output(
                    [BIN, "--proc-root", str(root), "--limit", "1000"],
                    text=True,
                )
                self.assertIn("memfd:memscope-memfd", out)
                self.assertIn("[shmem]", out)
        finally:
            os.close(fd)

    @staticmethod
    def file_pss(path):
        total = 0
        selected = False
        for line in Path(f"/proc/{os.getpid()}/smaps").read_text().splitlines():
            if re.match(r"^[0-9a-f]+-[0-9a-f]+ ", line):
                selected = str(path) in line
            elif selected and line.startswith("Pss:"):
                total += int(line.split()[1])
        return total


if __name__ == "__main__":
    unittest.main(verbosity=2)
