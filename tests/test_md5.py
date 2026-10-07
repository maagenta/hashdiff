"""Test 1: MD5 against the RFC 1321 A.5 vectors and hashlib.md5 (through the testhook)."""
import hashlib
import string

import pytest
from hypothesis import HealthCheck, given, settings, strategies as st

RFC_VECTORS = [
    (b"", "d41d8cd98f00b204e9800998ecf8427e"),
    (b"a", "0cc175b9c0f1b6a831c399e269772661"),
    (b"abc", "900150983cd24fb0d6963f7d28e17f72"),
    (b"message digest", "f96b697d7cb7938d525a2f31aaf161d0"),
    (b"abcdefghijklmnopqrstuvwxyz", "c3fcd3d76192e4007dfb496cca67e13b"),
    ((string.ascii_uppercase + string.ascii_lowercase + string.digits).encode(),
     "d174ab98d277d9f5a5611c2c9f419d9f"),
    (b"1234567890" * 8, "57edf4a22be3c955ac49da2e2107b67a"),
]


def md5_of(run_testhook, data, chunk=None):
    args = ["md5"] if chunk is None else ["md5", chunk]
    code, out, err = run_testhook(*args, input=data)
    assert code == 0, err
    assert out.endswith(b"\n") and len(out) == 33
    return out[:-1].decode("ascii")


@pytest.mark.parametrize("data,digest", RFC_VECTORS)
@pytest.mark.parametrize("chunk", [None, 1, 63, 64, 65])
def test_rfc1321_vectors(run_testhook, data, digest, chunk):
    assert md5_of(run_testhook, data, chunk) == digest


@pytest.mark.parametrize("size", [55, 56, 57, 63, 64, 65, 119, 120, 121, 128, 1 << 20])
def test_padding_boundaries(run_testhook, size):
    data = bytes(i % 251 for i in range(size))
    assert md5_of(run_testhook, data, 7) == hashlib.md5(data).hexdigest()


# run_testhook keeps no state between examples, so a function-scoped fixture is fine.
@settings(max_examples=200, deadline=None,
          suppress_health_check=[HealthCheck.function_scoped_fixture])
@given(data=st.binary(min_size=0, max_size=10000), chunk=st.integers(min_value=1, max_value=20000))
def test_matches_hashlib(run_testhook, data, chunk):
    assert md5_of(run_testhook, data, chunk) == hashlib.md5(data).hexdigest()


@pytest.mark.parametrize("args", [["md5", "0"], ["md5", "-1"], ["md5", "x"], ["md5", "1", "2"],
                                  ["nope"], []])
def test_invalid_arguments(run_testhook, args):
    code, out, err = run_testhook(*args)
    assert code == 2 and out == b""
