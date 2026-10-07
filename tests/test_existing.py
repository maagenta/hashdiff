"""Test 34: the three states of an existing results.hashdiff (section 3.3).

The state is read from the directory itself: a journal left behind means interrupted, every
side published means finished, and a diff file with a record means it found differences.
"""
import os

from conftest import read_bytes, run_on_pty

SPEC = {"a": b"aa", "b": b"bb", "c": b"cc"}


class Dir:
    """A results.hashdiff left in one of the three states."""

    def __init__(self, run_hashdiff, make_tree, tmp_path, differ=False):
        spec = dict(SPEC)
        self.origin = make_tree("origin", spec)
        if differ:
            spec = dict(spec, b=b"XX")
        self.destination = make_tree("destination", spec)
        self.out = os.path.join(os.fsencode(tmp_path), b"out")
        os.makedirs(self.out, exist_ok=True)
        self.results = os.path.join(self.out, b"results.hashdiff")
        self.run_hashdiff = run_hashdiff
        self.args = [self.origin, self.destination, "-o", self.out]
        code = run_hashdiff(*self.args)[0]
        assert code == (1 if differ else 0)

    def interrupt(self):
        for side in (b"origin", b"destination"):
            final = os.path.join(self.results, b"hashes-%s.txt" % side)
            data = read_bytes(final)
            os.remove(final)
            with open(final + b".tmp", "wb") as f:
                f.write(data[:data.index(b"# finished:")])

    def run(self, *extra, **kwargs):
        return self.run_hashdiff(*self.args, *extra, **kwargs)

    def pty(self, answers, bin):
        return run_on_pty(bin, self.args, answers)

    def names(self):
        return sorted(os.listdir(self.results))


def test_state_1_offers_resume(run_hashdiff, make_tree, tmp_path, hashdiff_bin):
    d = Dir(run_hashdiff, make_tree, tmp_path)
    d.interrupt()
    before = d.names()

    code, out, term = d.pty(b"a\n", hashdiff_bin)
    assert code == 2 and b"holds an interrupted run started on " in term
    assert b"[r]esume, [o]verwrite or [a]bort?" in term
    assert d.names() == before                      # abort touches nothing

    code, out, term = d.pty(b"nope\nr\n", hashdiff_bin)
    assert code == 0, term
    assert term.count(b"[r]esume") == 2             # an unknown answer repeats the question
    assert b"resume origin: " in out


def test_state_2_does_not_offer_resume(run_hashdiff, make_tree, tmp_path, hashdiff_bin):
    d = Dir(run_hashdiff, make_tree, tmp_path)

    code, out, term = d.pty(b"r\na\n", hashdiff_bin)
    assert code == 2, term
    assert b"holds a finished run started on " in term
    assert b"with no content differences." in term
    assert b"[o]verwrite or [a]bort?" in term
    assert b"[r]esume" not in term                  # nothing to resume
    assert term.count(b"[o]verwrite") == 2          # "r" is not an answer here

    code, out, term = d.pty(b"o\n", hashdiff_bin)
    assert code == 0, term
    assert b"resume origin: " not in out


def test_state_3_says_the_differences_were_never_copied(run_hashdiff, make_tree, tmp_path,
                                                        hashdiff_bin):
    d = Dir(run_hashdiff, make_tree, tmp_path, differ=True)

    code, out, term = d.pty(b"a\n", hashdiff_bin)
    assert code == 2, term
    assert b"differences were never copied (see rsync-command.txt)" in term
    assert b"[r]esume" not in term

    code, out, term = d.pty(b"overwrite\n", hashdiff_bin)
    assert code == 1, term


def test_without_a_terminal_each_state_names_its_flags(run_hashdiff, make_tree, tmp_path):
    d = Dir(run_hashdiff, make_tree, tmp_path, differ=True)

    code, out, err = d.run()
    assert code == 2
    assert b"holds a finished run; use --force to start over" in err
    assert b"--resume" not in err

    code, out, err = d.run("--resume")
    assert code == 2 and b"nothing to resume" in err

    d.interrupt()
    code, out, err = d.run()
    assert code == 2
    assert b"holds an interrupted run; use --resume to continue it or --force" in err


def test_a_directory_without_paths_txt_is_refused(run_hashdiff, make_tree, tmp_path):
    d = Dir(run_hashdiff, make_tree, tmp_path)
    os.remove(os.path.join(d.results, b"paths.txt"))

    code, out, err = d.run()
    assert code == 2
    assert b"written by an older hashdiff" in err and b"--force" in err
    assert d.run("--force")[0] == 0


def test_another_run_is_never_a_question(run_hashdiff, make_tree, tmp_path, hashdiff_bin):
    """A different root or count is fatal, even on a terminal (section 3.3)."""
    d = Dir(run_hashdiff, make_tree, tmp_path)
    other = make_tree("other", SPEC)

    code, out, term = run_on_pty(hashdiff_bin, [d.origin, other, "-o", d.out], b"o\n")
    assert code == 2, term
    assert b"used a different destination" in term
    assert b"[o]verwrite" not in term

    code, out, err = run_hashdiff(d.origin, d.destination, other, "-o", d.out)
    assert code == 2
    assert b"a different number of destinations" in err


def test_nothing_hashed_starts_over_without_asking(run_hashdiff, make_tree, tmp_path):
    """After an exit 4 there is nothing to decide, even with --resume (section 3.3)."""
    origin = make_tree("origin", dict(SPEC, only=b"x"))
    destination = make_tree("destination", SPEC)
    out = os.path.join(os.fsencode(tmp_path), b"out")
    os.makedirs(out, exist_ok=True)

    assert run_hashdiff(origin, destination, "-o", out)[0] == 4
    assert run_hashdiff(origin, destination, "-o", out)[0] == 4
    assert run_hashdiff(origin, destination, "-o", out, "--resume")[0] == 4
