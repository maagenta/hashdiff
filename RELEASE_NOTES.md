# Release notes

## v1.1 (in progress)

Version v1.1 has not been released yet; it is still in progress. Until it is published,
download v1.0 from its tag: <https://github.com/maagenta/hashdiff/releases/tag/v1.0>

### Changes

- When every hash matches, the summary now prints
  `no differences: ORIGIN and DESTINATION match` instead of a breakdown of zeros. The
  exit status is unchanged (0 when there are no differences).

### TODO

- [ ] Add the capability of comparing more than one destination, with
      `--number-of-destinations` and `hashdiff ORIGIN DEST1 DEST2 DEST3 ...`.

## v1.0

First release: <https://github.com/maagenta/hashdiff/releases/tag/v1.0>
