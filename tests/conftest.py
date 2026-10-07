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
    return _binary("HASHDIFF_BIN", "build/hashdiff")


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
    """The leading "# key: value" lines, stopping at the footer.

    The footer of section 9 begins at "# finished:" and repeats keys of the header, such as
    "# origin:", with another meaning, so it is not part of the header even when there are no
    records between the two.
    """
    header = {}
    i = 0
    while i < len(lines) and lines[i].startswith(b"# ") \
            and not lines[i].startswith(b"# finished:"):
        key, _, value = lines[i][2:].partition(b": ")
        header[key] = value
        i += 1
    return header, lines[i:]


def parse_hashes(path):
    """Returns (header dict, [(type, hash, size, path)]) of a hashes file.

    The "# finished:" footer of section 8 ends the records.
    """
    header, lines = _split_header(_read_lines(path))
    entries = []
    for line in lines:
        if line.startswith(b"#"):
            break
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
    """Parses a diff-files, tree-diff or tree-changes file.

    Returns (header dict, {section: [(status, path)]}); lines before any "## " section are
    stored under the None key. A diff-files record carries the two hashes of section 9 between
    the status and the path; they are dropped here and parse_diff returns them. Footer lines,
    which begin at "# finished:", are not records (section 9).
    """
    header, lines = _split_header(_read_lines(path))
    is_diff = header.get(b"hashdiff-diff") is not None
    sections = {None: []}
    current = None
    for line in lines:
        if line.startswith(b"#") and not line.startswith(b"## "):
            break                       # the footer
        if line.startswith(b"## "):
            current = line[3:]
            sections.setdefault(current, [])
            continue
        fields = line.split(b" ", 3 if is_diff else 1)
        sections[current].append((fields[0], unescape(fields[-1])))
    return header, sections


def parse_diff(path):
    """Returns (header, [(status, origin hash, destination hash, path)], footer lines)."""
    header, lines = _split_header(_read_lines(path))
    assert header.get(b"hashdiff-diff") == b"1", header
    records, footer = [], []
    for line in lines:
        if footer or line.startswith(b"# finished:"):
            footer.append(line)
            continue
        status, origin, destination, rel = line.split(b" ", 3)
        records.append((status, origin, destination, unescape(rel)))
    return header, records, footer


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
        diff = staticmethod(parse_diff)
        rsync_list = staticmethod(parse_rsync_list)
    return Parsers


@pytest.fixture
def compare(run_hashdiff, tmp_path):
    """compare(origin, destination, *args) runs hashdiff with --output tmp_path/out.

    Returns (exit code, stdout, stderr, path of results.hashdiff).
    """
    outdir = os.path.join(os.fsencode(tmp_path), b"out")
    os.makedirs(outdir, exist_ok=True)

    def run(origin, destination, *args, **kwargs):
        code, out, err = run_hashdiff(origin, destination, "--output", outdir, *args, **kwargs)
        return code, out, err, os.path.join(outdir, b"results.hashdiff")
    return run


def run_on_pty(hashdiff_bin, args, answers):
    """Runs hashdiff with stdin and stderr on a pseudo-terminal, typing answers."""
    master, slave = os.openpty()
    try:
        proc = subprocess.Popen([hashdiff_bin] + args, stdin=slave, stdout=subprocess.PIPE,
                                stderr=slave)
    finally:
        os.close(slave)
    os.write(master, answers)
    chunks = []
    while True:
        try:
            data = os.read(master, 65536)
        except OSError:
            break
        if not data:
            break
        chunks.append(data)
    stdout, _ = proc.communicate()
    os.close(master)
    return proc.returncode, stdout, b"".join(chunks)


def read_bytes(path):
    with open(path, "rb") as f:
        return f.read()


TIME_HEADERS = (b"# started:", b"# resumed:")


def strip_time(data):
    """Removes the lines that record wall-clock time (section 7).

    Those are the "# started:" and "# resumed:" headers and the footer, which begins at the
    "# finished:" line. Two runs over the same trees are byte-for-byte equal once they are
    gone, which is what "identical" means in the test descriptions.
    """
    out = []
    for line in data.split(b"\n"):
        if line.startswith(b"# finished:"):
            out.append(b"")
            break
        if line.startswith(TIME_HEADERS):
            continue
        out.append(line)
    return b"\n".join(out)


def snapshot_results(results):
    """{file name: contents without the time-dependent lines} for one results.hashdiff.

    history.txt and lock are left out: see NOT_COMPARABLE.
    """
    out = {}
    for name in sorted(os.listdir(results)):
        path = os.path.join(results, name)
        if name not in NOT_COMPARABLE and os.path.isfile(path):
            out[name] = strip_time(read_bytes(path))
    return out


@pytest.fixture
def snapshot():
    """snapshot(results) compares two runs byte for byte, ignoring the timestamps."""
    return snapshot_results


# Everything a single-destination run leaves in results.hashdiff. history.txt grows by one
# block per run and lock is this run's own (sections 3.2 and 3.6), so neither is comparable
# between two runs over the same trees.
NOT_COMPARABLE = (b"history.txt", b"lock")
RESULT_FILES = [b"paths.txt", b"history.txt", b"lock", b"tree-origin.txt",
                b"tree-destination.txt", b"tree-diff.txt", b"hashes-origin.txt",
                b"hashes-destination.txt", b"diff-files.txt", b"rsync-files.lst",
                b"rsync-command.txt"]
TREE_STAGE_FILES = [b"paths.txt", b"history.txt", b"lock", b"tree-origin.txt",
                    b"tree-destination.txt", b"tree-diff.txt"]


def results_line(stdout):
    """The path printed on the "results:" line of the summary, or None."""
    for line in stdout.split(b"\n"):
        if line.startswith(b"results: "):
            return line[len(b"results: "):]
    return None


def printed_command(stdout):
    """The rsync command line printed on stdout, or None."""
    for line in stdout.split(b"\n"):
        if line.startswith(b"rsync "):
            return line
    return None


def run_command_line(line):
    """Runs a printed command without a shell: shlex.split on the bytes as a str."""
    import shlex
    args = [os.fsencode(a) for a in shlex.split(os.fsdecode(line))]
    return subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def skip_if_root():
    if hasattr(os, "geteuid") and os.geteuid() == 0:
        pytest.skip("running as root: permissions are not enforced")


OFF_MAX = 2 ** 63 - 1
SEEK_BYTES = {"ssd": 200000, "hdd": 1440000}
DEFAULT_BLOCK = {"ssd": 64 * 1024, "hdd": 1024 * 1024}


def plan_oracle_k(n, gap, block, profile):
    """Python reimplementation of the decision of the sampling plan (section 6.2).

    Returns None for a full MD5, otherwise k. Unbounded integers: any value that would not fit
    in a 64-bit off_t makes rule 3 choose a full read, like the C code.
    """
    sb = SEEK_BYTES[profile]
    if n <= 2 * block:
        return None
    k = -(-(n - block) // (gap + block)) + 1
    if k * block > OFF_MAX or k * block >= n:
        return None
    lhs, rhs = k * (sb + block), sb + n
    if sb + block > OFF_MAX or lhs > OFF_MAX or rhs > OFF_MAX or lhs >= rhs:
        return None
    return k


def plan_offsets(n, block, k):
    """Offsets of k samples with the Bresenham accumulator of section 6.2."""
    q, r = divmod(n - block, k - 1)
    offsets, off, acc = [0], 0, 0
    for _ in range(k - 1):
        off += q
        acc += r
        if acc >= k - 1:
            off += 1
            acc -= k - 1
        offsets.append(off)
    return offsets


def plan_oracle(n, gap, block, profile):
    """None for a full MD5, otherwise the list of offsets."""
    k = plan_oracle_k(n, gap, block, profile)
    return None if k is None else plan_offsets(n, block, k)


@pytest.fixture
def testhook_plan(run_testhook):
    """Runs "hashdiff-testhook plan" and returns None (full) or the list of offsets."""
    def plan(n, gap, block, profile):
        code, out, err = run_testhook("plan", n, gap, block, profile)
        assert code == 0, err
        lines = out.split(b"\n")[:-1]
        if lines == [b"full"]:
            return None
        kind, k = lines[0].split(b" ")
        assert kind == b"sampled"
        offsets = [int(x) for x in lines[1:]]
        assert len(offsets) == int(k)
        return offsets
    return plan
