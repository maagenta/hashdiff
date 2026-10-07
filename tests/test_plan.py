"""Test 2: the sampling plan (through the testhook) against a Python oracle."""
import pytest
from hypothesis import HealthCheck, assume, given, settings, strategies as st

from conftest import SEEK_BYTES, plan_offsets, plan_oracle_k

KiB, MiB, GiB = 1024, 1024 ** 2, 1024 ** 3


def check_invariants(offsets, n, gap, block):
    assert offsets[0] == 0
    assert offsets[-1] == n - block
    assert all(b > a for a, b in zip(offsets, offsets[1:]))
    # The unread region between consecutive samples never exceeds the gap.
    assert all(b - (a + block) <= gap for a, b in zip(offsets, offsets[1:]))
    k = -(-(n - block) // (gap + block)) + 1
    assert len(offsets) == k


def first_sampled_n(gap, block, profile):
    """Smallest N whose plan is sampled: for each k, N must be in the range of that k and
    satisfy k*B < N and k*(SB+B) < SB+N."""
    sb = SEEK_BYTES[profile]
    for k in range(2, 10 ** 6):
        low = max(block + (k - 2) * (gap + block) + 1, 2 * block + 1,
                  k * (sb + block) - sb + 1, k * block + 1)
        if low <= block + (k - 1) * (gap + block):
            return low
    raise AssertionError("no sampled plan")


def threshold_cases():
    """Values of N around the threshold of rule 3 and around N = 2B."""
    cases = []
    for gap, block, profile in ((64 * MiB, MiB, "hdd"), (256 * KiB, 4 * KiB, "ssd"),
                                (GiB, 64 * KiB, "ssd"), (16 * MiB, 512, "hdd")):
        lo = first_sampled_n(gap, block, profile)
        for n in (lo - 2, lo - 1, lo, lo + 1, lo + 2, 2 * block, 2 * block + 1):
            cases.append((n, gap, block, profile))
    return cases


BATTERY = [
    (10 * GiB, 64 * MiB, MiB, "hdd"),
    (2 * 4096, 256 * KiB, 4096, "ssd"),
    (2 * 4096 + 1, 256 * KiB, 4096, "ssd"),
    (2 * MiB + 123, 64 * KiB, 4 * KiB, "ssd"),
    (2 * MiB + 123, 256 * KiB, 4 * KiB, "ssd"),
    (0, 1, 512, "ssd"),
    (1024, 1, 512, "hdd"),
    (2 ** 62, 2 ** 52, 64 * KiB, "ssd"),
    (2 ** 62 - 1, 2 ** 50, MiB, "hdd"),
    (2 ** 63 - 1, 2 ** 55, MiB, "hdd"),
    (2 ** 62, 1, 512, "ssd"),
    (2 ** 63 - 1, 2 ** 63 - 1, 2 ** 62, "ssd"),
] + threshold_cases()


def expected_offsets(n, gap, block, profile):
    k = plan_oracle_k(n, gap, block, profile)
    return None if k is None else plan_offsets(n, block, k)


@pytest.mark.parametrize("n,gap,block,profile", BATTERY)
def test_battery(testhook_plan, n, gap, block, profile):
    k = plan_oracle_k(n, gap, block, profile)
    assert k is None or k <= 100000
    offsets = testhook_plan(n, gap, block, profile)
    assert offsets == expected_offsets(n, gap, block, profile)
    if offsets is not None:
        check_invariants(offsets, n, gap, block)


def test_thresholds_flip(testhook_plan):
    for gap, block, profile in ((64 * MiB, MiB, "hdd"), (256 * KiB, 4 * KiB, "ssd")):
        lo = first_sampled_n(gap, block, profile)
        assert testhook_plan(lo - 1, gap, block, profile) is None
        assert testhook_plan(lo, gap, block, profile) is not None


def test_control_case(testhook_plan):
    offsets = testhook_plan(10 * GiB, 64 * MiB, MiB, "hdd")
    assert len(offsets) == 159


@settings(max_examples=300, deadline=None,
          suppress_health_check=[HealthCheck.function_scoped_fixture])
@given(n=st.integers(0, 2 ** 63 - 1), gap=st.integers(1, 2 ** 63 - 1),
       block=st.integers(512, 2 ** 40), profile=st.sampled_from(["ssd", "hdd"]))
def test_property(testhook_plan, n, gap, block, profile):
    k = plan_oracle_k(n, gap, block, profile)
    assume(k is None or k <= 20000)
    offsets = testhook_plan(n, gap, block, profile)
    assert offsets == expected_offsets(n, gap, block, profile)
    if offsets is not None:
        check_invariants(offsets, n, gap, block)


@settings(max_examples=200, deadline=None,
          suppress_health_check=[HealthCheck.function_scoped_fixture])
@given(n=st.integers(0, 64 * GiB), gap=st.integers(1, GiB),
       block=st.integers(512, 4 * MiB), profile=st.sampled_from(["ssd", "hdd"]))
def test_property_realistic_sizes(testhook_plan, n, gap, block, profile):
    k = plan_oracle_k(n, gap, block, profile)
    assume(k is None or k <= 20000)
    offsets = testhook_plan(n, gap, block, profile)
    assert offsets == expected_offsets(n, gap, block, profile)
    if offsets is not None:
        check_invariants(offsets, n, gap, block)


@pytest.mark.parametrize("args", [
    ("plan",), ("plan", 1, 1, 512), ("plan", -1, 1, 512, "ssd"), ("plan", 10, 0, 512, "ssd"),
    ("plan", 10, 1, 511, "ssd"), ("plan", 10, 1, 512, "nvme"),
    ("plan", 2 ** 63, 1, 512, "ssd"), ("plan", 10, 2 ** 64, 512, "ssd"),
    ("plan", "1x", 1, 512, "ssd"),
])
def test_invalid_arguments(run_testhook, args):
    code, out, err = run_testhook(*args)
    assert code == 2 and out == b""
