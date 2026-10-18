# Contributing to arduino-core-edison

## Build from source

You need [arduino-cli](https://arduino.github.io/arduino-cli/) (or the Arduino IDE),
[Zig 0.17.0](https://ziglang.org/download/) and Docker (to build the libgpiod sysroot and
the tools).

1. Build the sysroot and the tools (`edison-upload`, and `edison-cc`, a small wrapper the
   build uses): `tools/make-sysroot.sh` and `tools/build-tools.sh` (they write `dist/`).
2. Put this repository (or a link to it) at `<sketchbook>/hardware/open-edison/edison`.
3. Create `platform.local.txt` next to `platform.txt`, pointing at the tools (forward
   slashes, and a trailing slash on `compiler.path`; pick your computer's tools folder):

   ```
   compiler.path=/path/to/zig-x86_64-linux-0.17.0/
   compiler.sysroot=/path/to/arduino-core-edison/dist/edison-sysroot
   edison.tools.path=/path/to/arduino-core-edison/dist/edison-tools/linux-amd64
   ```

4. `arduino-cli compile -b open-edison:edison:edison tests/sketches/Blink`, then
   `arduino-cli upload -b open-edison:edison:edison -p PORT tests/sketches/Blink`.

**Release archives:** after step 1, `tools/package.sh VERSION [PREVIOUS_INDEX]` writes the
Boards Manager and PlatformIO archives and the index to `dist/release/`, as CI does. It
downloads Zig from ziglang.org and checks its checksums; put the archives in `downloads/`
to build without that site. Every archive has one top-level folder (the Boards Manager
strips it) holding a `package.json` (how PlatformIO finds the package and its host), so
[platform-edison](https://github.com/open-edison/platform-edison) installs the same
files.

## Layout

| Path | Purpose |
|---|---|
| `boards.txt`, `platform.txt` | The Arduino platform: compile recipes (Zig) and upload (edison-upload) |
| `cores/edison/` | The core; `api/` is ArduinoCore-API, vendored unchanged |
| `variants/edison/` | The Arduino kit's pin table: level shifters, pulls and muxes per pin |
| `tools/edison-upload/` | The uploader (Go): SSH upload over USB or the network |
| `tools/edison-cc/` | Wraps preprocessing so Zig never writes to `nul` on Windows |
| `tools/make-sysroot.sh` | libgpiod headers and library from Debian 13's packages |
| `tools/build-tools.sh` | Tests the Go tools and builds them for every host |
| `tools/package.sh` | Release archives and the Boards Manager index |
| `tests/sketches/` | Sketches CI compiles |

## CI and tests

- `.github/workflows/build.yml`, on every push: tests and builds the Go tools, packages a
  release (`tools/package.sh`), then on Linux, Windows and macOS (arm64 and x86_64)
  installs the core from that index (served locally) and compiles `tests/sketches/` and
  the elapsedMillis examples.
- `.github/workflows/release.yml`, on `v*` tags: builds through `build.yml` and creates the
  GitHub release with every archive, `SHA256SUMS` and the index. The index adds the new
  version to the previous release's, so earlier versions stay installable.

`tools/build-tools.sh` runs `gofmt`, `go vet` and `go test` before building; run it before
pushing a change to the tools.

## Releasing

1. Set `version=` in `platform.txt` to the new version; the release fails if it doesn't
   match the tag.
2. Make sure `main` is green and the change has been tested on a real board.
3. Tag and push: `git tag -a v0.2.0 -m "arduino-core-edison v0.2.0"` then
   `git push origin v0.2.0`.
4. Update `CORE_VERSION` in platform-edison so PlatformIO picks up the release.

The Boards Manager URL always serves the latest release's index:
`https://github.com/open-edison/arduino-core-edison/releases/latest/download/package_open-edison_index.json`.

## Conventions

- **Commits:** [Conventional Commits](https://www.conventionalcommits.org)
  (`feat: ...`, `fix(upload): ...`, `docs: ...`).
- **Licence:** the core is LGPL-2.1-or-later; new files carry the LGPL header, as in
  `cores/edison/main.cpp`.
- **Go:** `gofmt`-formatted, clean under `go vet`, standard library plus `x/crypto` only.
- **Shell:** POSIX `sh`, needing only Docker on the host.
- **Pin every external input** (Zig, the Go and Debian images, libgpiod) by version and
  checksum.
- **Workflow actions:** pin to the commit SHA of the latest release, with the version in a
  comment (`actions/checkout@<sha> # v7.0.1`). Actions published as immutable releases use
  their exact tag instead. Dependabot opens one grouped pull request a month with updates.
- **Hardware changes need a hardware test.** Note what you tested on a real board in the
  pull request, with anything you found that users should know (it belongs in the README or
  the docs).
