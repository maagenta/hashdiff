"""Shared fixtures for the hashdiff test suite.

Paths are handled as bytes everywhere and output files are parsed as bytes, never decoded.
"""
import os
import pathlib
import subprocess

import pytest

ROOT = pathlib.Path(__file__).resolve().parent.parent


def pytest_configure(config):
    config.addinivalue_line(
        "markers", "slow: long-running test, run only when HASHDIFF_SLOW_TESTS=1")


def pytest_collection_modifyitems(config, items):
    if os.environ.get("HASHDIFF_SLOW_TESTS") == "1":
        return
    skip = pytest.mark.skip(reason="slow test; set HASHDIFF_SLOW_TESTS=1 to run it")
    for item in items:
        if "slow" in item.keywords:
            item.add_marker(skip)


def _binary(env_name, default):
    value = os.environ.get(env_name)
    return os.path.abspath(value) if value else str(ROOT / default)


@pytest.fixture(scope="session")
def hashdiff_bin():
    return _binary("HASHDIFF_BIN", "hashdiff")


@pytest.fixture(scope="session")
def testhook_bin():
    return _binary("HASHDIFF_TESTHOOK", "build/hashdiff-testhook")


@pytest.fixture
def run_hashdiff(hashdiff_bin):
    """Runs hashdiff and returns (exit code, stdout bytes, stderr bytes).

    stdin is not a terminal unless the caller passes one, so hashdiff never prompts.
    """
    def run(*args, stdin=subprocess.DEVNULL, env=None, cwd=None, timeout=600):
        full_env = dict(os.environ)
        if env:
            full_env.update(env)
        proc = subprocess.run([os.fsencode(hashdiff_bin)] + [os.fsencode(a) for a in args],
                              stdin=stdin, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              env=full_env, cwd=cwd, timeout=timeout)
        return proc.returncode, proc.stdout, proc.stderr
    return run


@pytest.fixture
def run_testhook(testhook_bin):
    """Runs hashdiff-testhook and returns (exit code, stdout bytes, stderr bytes)."""
    def run(*args, input=b""):
        proc = subprocess.run([os.fsencode(testhook_bin)] + [os.fsencode(str(a)) for a in args],
                              input=input, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        return proc.returncode, proc.stdout, proc.stderr
    return run


class Symlink:
    def __init__(self, target):
        self.target = os.fsencode(target)


class Fifo:
    pass


class Dir:
    """An empty directory."""


@pytest.fixture
def make_tree(tmp_path):
    """make_tree(name, {relpath: content}) creates tmp_path/name and returns it as bytes.

    content is bytes or str (a regular file), Symlink(target), Fifo() or Dir().
    """
    def make(name, spec):
        root = os.path.join(os.fsencode(tmp_path), os.fsencode(name))
        os.makedirs(root, exist_ok=True)
        for rel, value in spec.items():
            path = os.path.join(root, os.fsencode(rel))
            parent = os.path.dirname(path)
            if parent:
                os.makedirs(parent, exist_ok=True)
            if isinstance(value, str):
                value = value.encode()
            if isinstance(value, (bytes, bytearray)):
                with open(path, "wb") as f:
                    f.write(value)
            elif isinstance(value, Symlink):
                os.symlink(value.target, path)
            elif isinstance(value, Fifo):
                os.mkfifo(path)
            elif isinstance(value, Dir):
                os.makedirs(path, exist_ok=True)
            else:
                raise TypeError("unsupported tree entry: %r" % (value,))
        return root
    return make


def unescape(raw):
    """Inverse of the path escaping of the output files: \\\\, \\n and \\r."""
    out = bytearray()
    i = 0
    while i < len(raw):
        c = raw[i:i + 1]
        if c != b"\\":
            out += c
            i += 1
            continue
        nxt = raw[i + 1:i + 2]
        if nxt == b"\\":
            out += b"\\"
        elif nxt == b"n":
            out += b"\n"
        elif nxt == b"r":
            out += b"\r"
        else:
            raise ValueError("invalid escape in %r" % raw)
        i += 2
    return bytes(out)


def _read_lines(path):
    with open(path, "rb") as f:
        data = f.read()
    assert data == b"" or data.endswith(b"\n"), "%s does not end with LF" % path
    return data.split(b"\n")[:-1] if data else []


def _split_header(lines):
    header = {}
    i = 0
    while i < len(lines) and lines[i].startswith(b"# "):
        key, _, value = lines[i][2:].partition(b": ")
        header[key] = value
        i += 1
    return header, lines[i:]


def parse_hashes(path):
    """Returns (header dict, [(type, hash, size, path)]) of a hashes-*.txt file."""
    header, lines = _split_header(_read_lines(path))
    entries = []
    for line in lines:
        kind, digest, size, rel = line.split(b" ", 3)
        entries.append((kind, digest, size, unescape(rel)))
    return header, entries


def parse_tree(path):
    """Returns (header dict, [(type, size, mtime, path)]) of a tree-*.txt file."""
    header, lines = _split_header(_read_lines(path))
    entries = []
    for line in lines:
        kind, size, mtime, rel = line.split(b" ", 3)
        entries.append((kind, size, mtime, unescape(rel)))
    return header, entries


def parse_report(path):
    """Parses diff-files.txt, tree-diff.txt or tree-changes.txt.

    Returns (header dict, {section: [(status, path)]}); lines before any "## " section are
    stored under the None key.
    """
    header, lines = _split_header(_read_lines(path))
    sections = {None: []}
    current = None
    for line in lines:
        if line.startswith(b"## "):
            current = line[3:]
            sections.setdefault(current, [])
            continue
        status, rel = line.split(b" ", 1)
        sections[current].append((status, unescape(rel)))
    return header, sections


def parse_rsync_list(path):
    """Returns the raw paths of rsync-files.lst."""
    with open(path, "rb") as f:
        data = f.read()
    assert data == b"" or data.endswith(b"\0")
    return data.split(b"\0")[:-1] if data else []


@pytest.fixture
def parsers():
    """Parsers for the result files, as attributes of one object."""
    class Parsers:
        hashes = staticmethod(parse_hashes)
        tree = staticmethod(parse_tree)
        report = staticmethod(parse_report)
        rsync_list = staticmethod(parse_rsync_list)
    return Parsers
