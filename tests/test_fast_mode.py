"""Tests 10, 11 and 13: fast mode."""
import hashlib
import os
import struct

import pytest

from conftest import read_bytes

KiB, MiB, GiB = 1024, 1024 ** 2, 1024 ** 3

# Test 10 uses -g 256K: with -g 64K and the ssd seek cost, rule 3 reads a 2 MiB file in full.
GAP, BLOCK, SIZE = 256 * KiB, 4 * KiB, 2 * MiB + 123
FAST = ("--fast", "-g", "256K", "-b", "4K", "--profile", "ssd")


def sampled_digest(data, offsets, block):
    md5 = hashlib.md5(struct.pack("<Q", len(data)))
    for off in offsets:
        md5.update(data[off:off + block])
    return md5.hexdigest().encode()


def write(path, data):
    with open(path, "wb") as f:
        f.write(data)


def damaged(data, start, length):
    out = bytearray(data)
    for i in range(start, min(start + length, len(out))):
        out[i] ^= 0xFF
    return bytes(out)


def test_s_hash_definition(make_tree, compare, parsers, testhook_plan):
    data = os.urandom(SIZE)
    origin = make_tree("origin", {"f": data})
    destination = make_tree("destination", {"f": data})
    code, out, err, results = compare(origin, destination, *FAST)
    assert code == 0, err
    header, entries = parsers.hashes(os.path.join(results, b"hashes-origin.txt"))
    assert header[b"mode"] == b"fast gap=262144 block=4096 profile=ssd seek_bytes=200000"
    offsets = testhook_plan(SIZE, GAP, BLOCK, "ssd")
    assert offsets is not None
    assert entries == [(b"S", sampled_digest(data, offsets, BLOCK), str(SIZE).encode(), b"f")]
    assert b"sampled files: 2" in out


def test_gap_guarantee(make_tree, compare, parsers, testhook_plan):
    data = os.urandom(SIZE)
    offsets = testhook_plan(SIZE, GAP, BLOCK, "ssd")
    origin = make_tree("origin", {"f": data})
    destination = make_tree("destination", {"f": data})
    target = os.path.join(destination, b"f")

    def detected(new_data):
        write(target, new_data)
        code, out, err, results = compare(origin, destination, "--force", *FAST)
        assert code in (0, 1), err
        return code == 1

    # Any contiguous damage of G + 1 bytes is detected, wherever it starts.
    for start in range(0, SIZE - GAP, 4 * KiB):
        assert detected(damaged(data, start, GAP + 1)), start
    # One byte in the middle of a gap between two samples is not.
    middle = (offsets[0] + BLOCK + offsets[1]) // 2
    assert not detected(damaged(data, middle, 1))
    # One byte within the last B bytes is.
    assert detected(damaged(data, SIZE - BLOCK // 2, 1))


def test_merge_gap_does_not_change_hashes(make_tree, run_hashdiff, tmp_path):
    spec = {"big%d" % i: os.urandom(size) for i, size in
            enumerate((SIZE, 3 * MiB + 1, 5 * MiB + 77, 9000))}
    origin = make_tree("origin", spec)
    destination = make_tree("destination", spec)
    outputs = []
    for merge_gap in ("0", "1T", None):
        out = os.path.join(os.fsencode(tmp_path), b"out-" + str(merge_gap).encode())
        os.mkdir(out)
        env = {"HASHDIFF_MERGE_GAP": merge_gap} if merge_gap is not None else None
        code, stdout, stderr = run_hashdiff(origin, destination, "-o", out, *FAST, env=env)
        assert code == 0, stderr
        results = os.path.join(out, b"results.hashdiff")
        outputs.append([read_bytes(os.path.join(results, b"hashes-%s.txt" % side))
                        for side in (b"origin", b"destination")])
    assert outputs[0] == outputs[1] == outputs[2]
    assert b"\nS " in outputs[0][0]


def test_invalid_merge_gap(make_tree, compare):
    origin = make_tree("origin", {"f": b"x"})
    destination = make_tree("destination", {"f": b"x"})
    code, out, err, results = compare(origin, destination, "--fast",
                                      env={"HASHDIFF_MERGE_GAP": "lots"})
    assert code == 2 and b"HASHDIFF_MERGE_GAP" in err


def make_sparse(path, size):
    with open(path, "wb") as f:
        f.truncate(size)
    st = os.stat(path)
    if st.st_blocks * 512 >= size:
        pytest.skip("the filesystem does not support sparse files")


def poke(path, offset, value):
    with open(path, "r+b") as f:
        f.seek(offset)
        f.write(value)


def sparse_trees(make_tree, testhook_plan):
    size = 5 * GiB + 12345
    origin = make_tree("origin", {})
    destination = make_tree("destination", {})
    for root in (origin, destination):
        try:
            make_sparse(os.path.join(root, b"sparse"), size)
        except OSError as e:
            pytest.skip("cannot create a sparse file of 5 GiB: %s" % e)
    offsets = testhook_plan(size, 64 * MiB, 64 * KiB, "ssd")
    assert offsets is not None
    inside = [off for off in offsets if off > 4 * GiB][0] + 100
    assert inside > 4 * GiB
    return origin, destination, inside


def test_sparse_over_4gib_fast(make_tree, compare, parsers, testhook_plan):
    origin, destination, offset = sparse_trees(make_tree, testhook_plan)
    poke(os.path.join(destination, b"sparse"), offset, b"\x01")
    code, out, err, results = compare(origin, destination, "--fast")
    assert code == 1, err
    _, sections = parsers.report(os.path.join(results, b"diff-files.txt"))
    assert sections[None] == [(b"HASH", b"sparse")]


@pytest.mark.slow
def test_sparse_over_4gib_full(make_tree, compare, parsers, testhook_plan):
    origin, destination, offset = sparse_trees(make_tree, testhook_plan)
    poke(os.path.join(destination, b"sparse"), offset, b"\x01")
    code, out, err, results = compare(origin, destination)
    assert code == 1, err
    _, sections = parsers.report(os.path.join(results, b"diff-files.txt"))
    assert sections[None] == [(b"HASH", b"sparse")]
