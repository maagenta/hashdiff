"""Test 15: the printed rsync command repairs corruption that keeps size and mtime."""
import os
import shutil

import pytest

from conftest import printed_command, run_command_line


@pytest.mark.skipif(shutil.which("rsync") is None, reason="rsync is not in PATH")
def test_rsync_repairs_same_size_same_mtime(make_tree, compare):
    data = os.urandom(200000)
    origin = make_tree("origin", {"big.bin": data, "other": b"same"})
    destination = make_tree("destination", {"big.bin": data, "other": b"same"})
    target = os.path.join(destination, b"big.bin")
    st = os.stat(target)
    corrupted = bytearray(data)
    corrupted[123456] ^= 0xFF
    with open(target, "r+b") as f:
        f.write(corrupted)
    os.utime(target, ns=(st.st_atime_ns, st.st_mtime_ns))
    assert os.stat(target).st_mtime_ns == st.st_mtime_ns

    code, out, err, results = compare(origin, destination)
    assert code == 1, err
    proc = run_command_line(printed_command(out))
    assert proc.returncode == 0, proc.stderr
    code, out, err, results = compare(origin, destination, "--force")
    assert code == 0, (out, err)
