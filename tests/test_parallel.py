"""Test 14: identical results with any -j, --serial and --profile hdd; signals; progress."""
import os
import random
import subprocess

import pytest

from conftest import Symlink, read_bytes

RESULT_FILES = [b"tree-origin.txt", b"tree-destination.txt", b"tree-diff.txt",
                b"hashes-origin.txt", b"hashes-destination.txt", b"diff-files.txt",
                b"rsync-files.lst", b"rsync-command.txt"]


def varied_trees(make_tree):
    rnd = random.Random(1234)
    spec = {}
    for i in range(300):
        size = rnd.choice([0, 1, 100, 4096, 65536, 70000, 300000, 1 << 20, 3 << 20])
        spec["d%d/f%03d" % (i % 7, i)] = rnd.randbytes(size)
    for i in range(10):
        spec["links/l%d" % i] = Symlink("../d0/f%03d" % i)
    origin = make_tree("origin", spec)
    for i in range(0, 300, 37):
        data = bytearray(spec["d%d/f%03d" % (i % 7, i)])
        if data:
            data[len(data) // 2] ^= 1
            spec["d%d/f%03d" % (i % 7, i)] = bytes(data)
    destination = make_tree("destination", spec)
    return origin, destination


def results_of(run_hashdiff, tmp_path, origin, destination, args):
    out = os.path.join(os.fsencode(tmp_path), b"out-" + "_".join(args).encode())
    os.mkdir(out)
    code, stdout, stderr = run_hashdiff(origin, destination, "-o", out, *args)
    assert code == 1, stderr
    results = os.path.join(out, b"results.hashdiff")
    files = {name: read_bytes(os.path.join(results, name)) for name in RESULT_FILES}
    # The command names its own results directory, which differs between runs.
    files[b"rsync-command.txt"] = files[b"rsync-command.txt"].replace(out, b"OUT")
    return files


@pytest.mark.parametrize("variants", [
    [(), ("-j", "4"), ("--serial",), ("-j", "3", "--serial"), ("--profile", "hdd"),
     ("-j", "4", "--profile", "hdd")],
    [("--fast", "-g", "1M", "-b", "4K"), ("--fast", "-g", "1M", "-b", "4K", "-j", "4"),
     ("--fast", "-g", "1M", "-b", "4K", "--serial", "-j", "2")],
    [("--fast", "--profile", "hdd", "-g", "1M"), ("--fast", "--profile", "hdd", "-g", "1M",
                                                  "-j", "4"),
     ("--fast", "--profile", "hdd", "-g", "1M", "--serial")],
], ids=["full", "fast-ssd", "fast-hdd"])
def test_identical_results(make_tree, run_hashdiff, tmp_path, variants):
    origin, destination = varied_trees(make_tree)
    outputs = [results_of(run_hashdiff, tmp_path, origin, destination, list(args))
               for args in variants]
    for other in outputs[1:]:
        assert other == outputs[0]
    hashes = outputs[0][b"diff-files.txt"].count(b"\nHASH ")
    # Full mode finds every changed byte; fast mode only those inside a sample.
    assert hashes == 8 if "--fast" not in variants[0] else 1 <= hashes <= 8


def test_hdd_jobs_warning(make_tree, compare):
    origin = make_tree("origin", {"a": b"a"})
    destination = make_tree("destination", {"a": b"a"})
    code, out, err, results = compare(origin, destination, "--fast", "--profile", "hdd",
                                      "-j", "2")
    assert code == 0
    assert b"several readers on the same disk" in err


def run_with_pty_stderr(hashdiff_bin, args):
    master, slave = os.openpty()
    try:
        proc = subprocess.Popen([hashdiff_bin] + args, stdin=subprocess.DEVNULL,
                                stdout=subprocess.PIPE, stderr=slave)
    finally:
        os.close(slave)
    chunks = []
    while True:
        try:
            data = os.read(master, 65536)
        except OSError:
            break
        if not data:
            break
        chunks.append(data)
    proc.communicate()
    os.close(master)
    return proc.returncode, b"".join(chunks)


def test_progress_on_terminal(make_tree, hashdiff_bin, tmp_path):
    origin = make_tree("origin", {"f%d" % i: b"data" for i in range(5)})
    destination = make_tree("destination", {"f%d" % i: b"data" for i in range(5)})
    out = os.fsencode(tmp_path)
    code, err = run_with_pty_stderr(hashdiff_bin, [origin, destination, "-o", out])
    assert code == 0, err
    assert b"[origin] 5/5 files, 20 B" in err
    assert b"[destination] 5/5 files, 20 B" in err
    code, err = run_with_pty_stderr(hashdiff_bin, [origin, destination, "-o", out, "--force",
                                                   "-q"])
    assert code == 0 and b"[origin]" not in err
