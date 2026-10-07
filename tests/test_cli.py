"""Command line parsing, --help and --version."""
import pytest


def usage_error(stderr):
    return b"Try 'hashdiff --help'" in stderr


def test_version(run_hashdiff):
    code, out, err = run_hashdiff("--version")
    assert (code, out, err) == (0, b"hashdiff 2.0\n", b"")
    assert run_hashdiff("-V")[:2] == (0, b"hashdiff 2.0\n")


def test_help(run_hashdiff):
    code, out, err = run_hashdiff("--help")
    assert code == 0 and err == b""
    assert out.startswith(b"Usage: hashdiff ORIGIN DESTINATION [DESTINATION ...] [OPTIONS]\n")
    for opt in (b"--output", b"--resume", b"--force", b"--fast", b"--gap", b"--block",
                b"--profile", b"--jobs", b"--serial", b"--one-file-system",
                b"--number-of-destinations", b"--quiet"):
        assert opt in out
    assert b"HASHDIFF_MERGE_GAP" not in out
    assert run_hashdiff("-h")[:2] == (0, out)


@pytest.mark.parametrize("args", [
    (),
    ("only-one",),
    ("a", "b", "--number-of-destinations", "2"),
    ("a", "b", "c", "--number-of-destinations", "4"),
    ("a", "b", "--number-of-destinations", "0"),
    ("a", "b", "--number-of-destinations", "65"),
    ("a", "b", "--number-of-destinations", "x"),
    ("a", "b", "--bogus"),
    ("a", "b", "-Z"),
    ("a", "b", "--output"),
    ("a", "b", "--fast=yes"),
    ("a", "b", "-j", "0"),
    ("a", "b", "-j", "257"),
    ("a", "b", "-j", "4x"),
    ("a", "b", "--gap", "0"),
    ("a", "b", "--gap", "-1"),
    ("a", "b", "--gap", "8X"),
    ("a", "b", "--gap", "8MB"),
    ("a", "b", "--gap", ""),
    ("a", "b", "--gap", "9223372036854775808"),
    ("a", "b", "--gap", "8388608T"),
    ("a", "b", "--block", "511"),
    ("a", "b", "--profile", "nvme"),
    ("a", "b", "--resume", "--force"),
    ("", "b"),
])
def test_usage_errors(run_hashdiff, args):
    code, out, err = run_hashdiff(*args)
    assert code == 2 and out == b""
    assert usage_error(err)


@pytest.mark.parametrize("args", [
    ("a", "b"),
    ("--fast", "a", "b"),
    ("a", "--fast", "b"),
    ("a", "b", "--fast", "--output", "dir"),
    ("a", "b", "-f", "--output=dir"),
    ("a", "b", "-f", "-g", "8M"),
    ("a", "b", "-f", "-g8m"),
    ("a", "b", "-fg8M"),
    ("a", "b", "-f", "--gap=8388607T"),
    ("a", "b", "-f", "--block", "512"),
    ("a", "b", "-f", "-b1k", "--profile", "hdd"),
    ("a", "b", "-j", "256", "--serial", "-x", "-q"),
    ("-fqx", "a", "b"),
    ("--", "-a", "-b"),
    ("a", "--", "--output"),
    ("a", "b", "--resume"),
    ("a", "b", "--force"),
])
def test_accepted_forms(run_hashdiff, args):
    code, out, err = run_hashdiff(*args)
    assert not usage_error(err), err
    assert b"has no effect" not in err


@pytest.mark.parametrize("opt,name", [
    (("--gap", "8M"), b"--gap"),
    (("-b", "4K"), b"--block"),
    (("--profile", "hdd"), b"--profile"),
])
def test_fast_only_options_warn(run_hashdiff, opt, name):
    code, out, err = run_hashdiff("a", "b", *opt)
    assert not usage_error(err)
    assert b"warning: " + name + b" has no effect without --fast" in err
