"""Test 27: path cleaning (section 2.1 of docs/implementation.md).

Any spelling of the same directories is one run: the result files are identical, nothing that
is written or printed holds a ".", a ".." or a "//", and one spelling can resume another.
"""
import os

from conftest import read_bytes, results_line


def header_root(path):
    """The unescaped "# root:" line of a tree or hashes file."""
    with open(path, "rb") as f:
        for line in f:
            if line.startswith(b"# root: "):
                return line[len(b"# root: "):-1]
    raise AssertionError("no '# root:' line in %r" % path)


def spellings(name):
    """(origin, destination) as typed, all naming the same two directories."""
    return [
        (b"origin", b"destination"),
        (b"./origin", b"./destination"),
        (b"origin/", b"destination//"),
        (b"./././origin", b"destination/./"),
        (os.path.join(b"..", name, b"origin"), os.path.join(b"..", name, b"destination")),
    ]


def test_every_spelling_is_the_same_run(run_hashdiff, make_tree, tmp_path, snapshot):
    origin = make_tree("origin", {"a": b"a", "sub/b": b"bb", "sub/c": b"ccc"})
    destination = make_tree("destination", {"a": b"a", "sub/b": b"bb", "sub/c": b"ccc"})
    base = os.fsencode(tmp_path)
    out = os.path.join(base, b"out")
    os.makedirs(out, exist_ok=True)
    results = os.path.join(out, b"results.hashdiff")
    cases = spellings(os.path.basename(base)) + [(origin, destination)]
    first = None

    for typed_origin, typed_destination in cases:
        code, stdout, err = run_hashdiff(typed_origin, typed_destination, "--output", out,
                                         "--force", cwd=base)
        assert code == 0, (typed_origin, err)
        assert results_line(stdout) == results
        for name in (b"tree-origin.txt", b"hashes-origin.txt"):
            assert header_root(os.path.join(results, name)) == origin
        current = snapshot(results)
        if first is None:
            first = current
        else:
            assert current == first, typed_origin


def test_nothing_printed_or_stored_keeps_a_dot(run_hashdiff, make_tree, tmp_path):
    origin = make_tree("origin", {"a": b"aaa"})
    make_tree("destination", {"a": b"bbb"})
    base = os.fsencode(tmp_path)
    code, stdout, err = run_hashdiff(b"./origin/", b"./destination/", cwd=base)
    assert code == 1, err
    results = os.path.join(base, b"results.hashdiff")

    values = [results_line(stdout)]
    values += [line for line in stdout.split(b"\n") if line.startswith(b"rsync ")]
    values.append(read_bytes(os.path.join(results, b"rsync-command.txt")).rstrip(b"\n"))
    for name in (b"tree-origin.txt", b"tree-destination.txt", b"hashes-origin.txt",
                 b"hashes-destination.txt"):
        values.append(header_root(os.path.join(results, name)))

    assert len(values) == 7, values
    for value in values:
        assert value
        for bad in (b"/./", b"/../", b"//"):
            assert bad not in value, value
        assert not value.endswith(b"/"), value


def test_every_output_spelling_prints_the_same_path(run_hashdiff, make_tree, tmp_path):
    origin = make_tree("origin", {"a": b"a"})
    destination = make_tree("destination", {"a": b"a"})
    base = os.fsencode(tmp_path)
    out = os.path.join(base, b"out")
    sub = os.path.join(out, b"sub")
    os.makedirs(sub, exist_ok=True)
    expected = os.path.join(out, b"results.hashdiff")

    for args, cwd in (((), out), (("--output", "."), out), (("--output", "./"), out),
                      (("--output", ".//."), out), (("--output", ".."), sub)):
        code, stdout, err = run_hashdiff(origin, destination, "--force", *args, cwd=cwd)
        assert code == 0, (args, err)
        assert results_line(stdout) == expected, args


def test_resume_accepts_another_spelling(run_hashdiff, make_tree, tmp_path):
    origin = make_tree("origin", {"a": b"a", "b": b"bb"})
    make_tree("destination", {"a": b"a", "b": b"bb"})
    base = os.fsencode(tmp_path)
    out = os.path.join(base, b"out")
    os.makedirs(out, exist_ok=True)
    results = os.path.join(out, b"results.hashdiff")

    code, _, err = run_hashdiff(b"./origin", b"./destination", "--output", out, cwd=base)
    assert code == 0, err
    for side in (b"origin", b"destination"):
        name = os.path.join(results, b"hashes-" + side + b".txt")
        os.rename(name, name + b".tmp")

    code, stdout, err = run_hashdiff(b"origin", b"destination/", "--output", out, "--resume",
                                     cwd=base)
    assert code == 0, err
    assert b"belongs to another root" not in err
    assert b"resume origin:" in stdout


def parse_paths(path):
    """(header dict, [(label, root)]) of paths.txt or one block of history.txt."""
    header, roles = {}, []
    for line in read_bytes(path).split(b"\n")[:-1]:
        if line.startswith(b"# "):
            key, _, value = line[2:].partition(b": ")
            header[key] = value
        elif line:
            label, _, root = line.partition(b" ")
            roles.append((label, root))
    return header, roles


def test_paths_txt_records_the_roots(run_hashdiff, make_tree, tmp_path):
    """Test 28: one line per side, in command-line order (section 3.2)."""
    spec = {"a": b"a"}
    origin = make_tree("origin", spec)
    first = make_tree("d1", spec)
    second = make_tree("d2", spec)
    out = os.path.join(os.fsencode(tmp_path), b"out")
    os.makedirs(out, exist_ok=True)
    results = os.path.join(out, b"results.hashdiff")

    code, _, err = run_hashdiff(origin, first, second, "--output", out)
    assert code == 0, err
    header, roles = parse_paths(os.path.join(results, b"paths.txt"))
    assert header[b"hashdiff-paths"] == b"1"
    assert roles == [(b"origin", origin), (b"destination-1", first),
                     (b"destination-2", second)]
    assert b"file" not in header                      # only with --file
    import datetime
    datetime.datetime.strptime(header[b"started"].decode(), "%d/%m/%Y %H:%M")

    code, _, err = run_hashdiff(origin, first, "--output", out, "--force")
    assert code == 0, err
    _, roles = parse_paths(os.path.join(results, b"paths.txt"))
    assert roles == [(b"origin", origin), (b"destination", first)]


def test_paths_txt_escapes_a_root_with_a_newline(run_hashdiff, make_tree, tmp_path):
    """Test 28: the escaping of section 8, which a label/value layout could not hold."""
    spec = {"a": b"a"}
    origin = make_tree("odd\nname", spec)
    destination = make_tree("destination", spec)
    out = os.path.join(os.fsencode(tmp_path), b"out")
    os.makedirs(out, exist_ok=True)

    code, _, err = run_hashdiff(origin, destination, "--output", out)
    assert code == 0, err
    line = read_bytes(os.path.join(out, b"results.hashdiff", b"paths.txt"))
    assert b"origin " + origin.replace(b"\n", b"\\n") + b"\n" in line
    assert origin not in line                         # the raw newline is never written


def test_paths_txt_survives_a_run_that_stops_in_the_tree_stage(compare, make_tree):
    """Test 28: it is written before anything is hashed."""
    origin = make_tree("origin", {"a": b"a", "only-here": b"x"})
    destination = make_tree("destination", {"a": b"a"})
    code, _, err, results = compare(origin, destination)
    assert code == 4, err
    assert b"paths.txt" in os.listdir(results)
    assert b"hashes-origin.txt" not in os.listdir(results)


def test_history_keeps_one_block_per_run(run_hashdiff, make_tree, tmp_path):
    """Test 28: appended, oldest first, and never removed (section 3.2)."""
    spec = {"a": b"a"}
    origin = make_tree("origin", spec)
    first = make_tree("d1", spec)
    second = make_tree("d2", spec)
    out = os.path.join(os.fsencode(tmp_path), b"out")
    os.makedirs(out, exist_ok=True)
    history = os.path.join(out, b"results.hashdiff", b"history.txt")

    assert run_hashdiff(origin, first, "--output", out)[0] == 0
    assert run_hashdiff(origin, second, "--output", out, "--force")[0] == 0
    data = read_bytes(history)
    assert data.count(b"# hashdiff-history: 1\n") == 1
    assert data.count(b"# started: ") == 2
    assert data.index(first) < data.index(second)     # oldest first
    assert b"history.txt" in os.listdir(os.path.dirname(history))

    # A run that fails before reading anything appends nothing.
    before = read_bytes(history)
    assert run_hashdiff(origin, first, "--output", out)[0] == 2      # results exist, no flag
    assert read_bytes(history) == before
