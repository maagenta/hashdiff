# hashdiff @VERSION@

<!-- Write the summary of this release here, then publish the draft. -->

## Binaries

Every archive holds the `hashdiff` binary, `README.md` and `LICENSE`. Unpack it and copy
the binary anywhere on your `PATH`:

    tar xzf hashdiff-@VERSION@-<system>.tar.gz
    sudo cp hashdiff-@VERSION@-<system>/hashdiff /usr/local/bin/

| Archive | System | Minimum requirement |
| --- | --- | --- |
| `hashdiff-@VERSION@-linux-x86_64.tar.gz` | Linux, x86-64 | Kernel 2.6.39 (the minimum musl supports). Statically linked against musl: it needs no libc and no other library, and runs the same on glibc and musl distributions. |
| `hashdiff-@VERSION@-linux-arm64.tar.gz` | Linux, aarch64 | Any arm64 kernel: Linux gained arm64 in 3.7, well after 2.6.39. Statically linked against musl, as above. |
| `hashdiff-@VERSION@-macos-x86_64.tar.gz` | macOS, Intel | macOS 10.13 High Sierra. |
| `hashdiff-@VERSION@-macos-arm64.tar.gz` | macOS, Apple Silicon | macOS 11.0 Big Sur, the first release for Apple Silicon. |
| `hashdiff-@VERSION@-freebsd-amd64.tar.gz` | FreeBSD, amd64 | FreeBSD 14.0, against the base system libc. A 14.x binary keeps working on later majors. |

On Apple Silicon take the arm64 archive. The Intel one also runs there, translated by
Rosetta, and the only symptom is that hashing is slower.

Nothing else is needed to run hashdiff: it never executes `rsync`, it only prints the
command for you to review and run. GNU make and a C89 compiler are needed to build it,
not to use it.

## Verifying a download

    sha256sum -c SHA256SUMS         # Linux
    shasum -a 256 -c SHA256SUMS     # macOS and FreeBSD

`SHA256SUMS` covers every archive of this release; `sha256sum -c` reports the ones you
did not download as missing, which is expected.

## Building from source

The archives are a convenience. On any other system, or to build with your own compiler
and flags, see the "Build and install" section of the README: hashdiff is strict C89 with
no dependencies beyond libc and POSIX, and `make install` is the whole procedure.
