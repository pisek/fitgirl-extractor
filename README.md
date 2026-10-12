# fitgirl-extractor

`fitgirl-extractor.exe` unpacks FitGirl repacks (`setup.exe` + `fg-*.bin`) without running their installer.
It is a single Windows program, so it runs on Windows directly and on macOS or Linux under Wine.

FitGirl's `setup.exe` (Inno Setup + `ISDone.dll`) crashes under Wine before it unpacks anything:
it fails in every Wine build tried (Heroic Wine 11.18, Wine 11.18-DXMT, CrossOver 23.7.1, Game Porting Toolkit 3.0).
This tool skips the installer and drives FitGirl's own unpacker (`unarc.dll`, a modified FreeArc) directly.
That DLL handles their archive format and calls their decompression tools (srep, magic2, xtool/rtool,
Oodle, Razor, …). The installer itself is unpacked by an embedded copy of
[innoextract](https://constexpr.org/innoextract/), so nothing else needs to be installed.

## Requirements

- **macOS / Linux:** a Wine build with WoW64 support. Heroic's **Wine-11.18** (Heroic → Wine Manager) is tested;
  on Apple Silicon, Rosetta 2 is needed as well.
- **Windows:** 64-bit Windows 10 or later (FitGirl's helper tools are 64-bit). Not tested yet.

## Download or build

Releases ship `fitgirl-extractor.exe` (plus a zip with this README and the licenses of the bundled libraries).

To build it yourself you only need [zig](https://ziglang.org/) 0.17 (`brew install zig`):

```sh
zig build          # -> zig-out/bin/fitgirl-extractor.exe (32-bit Windows, all libraries linked in)
zig build test     # unit tests, run on the build machine
```

The build downloads its pinned dependencies (innoextract, Boost, liblzma, zlib, bzip2; see `build.zig.zon`)
and cross-compiles everything; no Windows SDK or other compiler is needed. Tested on macOS; CI builds on Linux.

## Usage

```sh
wine fitgirl-extractor.exe ~/Downloads/FitGirl-repack/ -d ~/Games/FitGirl-repack-extracted/
```

The first path is the folder with `setup.exe` and the `fg-*.bin` files, `-d` is where the game goes.
Unix paths work as they are (they're mapped to Wine's `Z:` drive). Any Wine prefix will do; nothing is installed into it.

`wine` stands for any Wine with WoW64 support, e.g. Heroic's
`"$HOME/Library/Application Support/heroic/tools/wine/Wine-11.18/Contents/Resources/wine/bin/wine"`.
Set `WINEDEBUG=-all` to silence Wine's diagnostics. On Windows, run the exe directly.

| Option | Meaning |
|---|---|
| `-d, --dest DIR` | where to unpack the game (required unless `--list`) |
| `--list` | only list archive contents |
| `--only GLOB` / `--exclude GLOB` | choose `.bin` files, e.g. `--exclude 'fg-selective-*'` (repeatable; `*`, `?` and `[...]`, case-insensitive) |
| `--skip-bin-check` | don't check the `.bin` files against `MD5\*.md5` first |
| `--skip-verify` | don't verify unpacked files against `_Redist\*.md5` |
| `--timeout HOURS` | per-archive timeout (default 6) |
| `-v, --verbose` | print every file and the worker's raw output |
| `--version` | show the version |

Exit code: 0 if everything unpacked and matched FitGirl's MD5 list, 1 if an archive failed or verification
found problems, 2 on other errors.

## What it does

1. Checks the `.bin` files against `MD5\fitgirl-bins.md5`.
2. Unpacks the installer's files from `setup.exe` (embedded innoextract) into `<dest>\.fgtmp`.
   Its `tmp\` folder contains `unarc.dll`, `arc.ini`, the CLS plugins and the external tools.
3. Fills in Inno Setup placeholders in their `.ini` files (`{src}`, `{tmp}`, and `{app}`, which `CLS.ini` only uses
   for temp paths, so it's mapped to `<dest>\.fgtmp\tmp` to keep helper temp files out of the game folder)
   and applies the CLS namespace fix (see below).
4. Unpacks each archive in a worker process (the same exe, started with `--worker`), which loads `unarc.dll` and calls
   `FreeArcExtract(callback, "x", "-o+", "-dp<dest>", "-w<dest>\.fgtmp\tmp", "-cfg<tmp>\arc.ini", "--", <archive>, NULL)`.
5. Copies the installer's regular files (e.g. `_Redist\`) into the destination, removes `.fgtmp` and verifies
   everything against `_Redist\fitgirl.md5`.

## Incompatibilities this works around

- **Installer crash.** `setup.tmp` makes a null call right after loading `ISDone.dll`/`InnoCallback.dll`,
  in every Wine version tried. Bypassed by not running the installer.
- **CLS namespace mismatch.** Some plugin pairs disagree on names: `cls-srep_x64.exe` creates
  `Global\Read_MapFile_CLS-srep00`, while the 32-bit `CLS-srep.dll` opens `Read_MapFile_CLS-srep00`.
  Wine keeps those as two different objects, so the DLL fails (`Can't open read file mapping!`,
  surfacing as `archive data corrupted (decompression fails)`) and the helper hangs. Whenever a
  `cls-<name>.dll` has no `Global\` string, the unpacked copies of its `_x64.exe`/`_x86.exe` helpers get
  their standalone `Global\` string blanked (one byte). `setup.exe` itself is never modified.
- **arc.ini location.** `unarc.dll` looks for `arc.ini` (the external compressor definitions) next to a hard-coded
  `c:\unarc.dll`; without it every external method fails with `unsupported compression method`. FitGirl's build
  accepts `-cfg<file>` (no `=`), so the worker points it at the installer's copy and nothing is written to `C:\`.
- **unarc.dll memory probe.** It binary-searches the largest free memory block with `(lo+hi)/2` in 32 bits.
  In a large-address-aware (4 GB) process this overflows and loops forever on `VirtualAlloc(size 0)`.
  The exe is therefore built **without** `LARGE_ADDRESS_AWARE`; the worker refuses to run otherwise and CI checks it.
- **Standard handle reuse.** While cleaning up after external tools, `unarc.dll` closes the process's
  standard output handle. Wine hands the same handle value to the next file it creates, so anything
  printed to stdout afterwards lands *inside an extracted game file* (in the tested pack, the last file of
  `fg-01.bin` got a progress line prepended in about half of the runs). The worker therefore writes all its output
  through a private duplicate of the stdout handle.
- **Exit deadlocks and orphaned helpers.** CLS helpers and DLL detach can deadlock at exit, so the worker calls
  `TerminateProcess` right after `FreeArcExtract` returns. Each worker runs in a job object that is terminated
  afterwards (or on timeout, or if the main process dies), which also stops any helper processes left behind.

## Limitations

- Only unpacks. The installer's post-install steps are **not** run: redistributables (`_Redist\`),
  registry entries, shortcuts, renames done by the install script (e.g. a leftover `*.rne` file).
  Install the redistributables in your game's prefix if the game needs them.
- Only packs whose `setup.exe` ships `unarc.dll` (FreeArc-based) are supported, and only Inno Setup versions
  innoextract supports (up to 6.4).
- `unarc.dll` takes ANSI paths, so the pack and destination paths must be representable in the Windows ANSI
  code page (under Wine usually Windows-1252); otherwise the tool stops with an error.
- Each CLS method needs its 64-bit helper to work under Wine. Only the ones used by the tested pack were exercised.

## Tested

| Pack | Methods | Result |
|---|---|---|
| Rivage (2 × .bin, 3.0 GB) | `xt3d9a`, `doodle9ka/9ma` (Oodle), `srep`, `magic2`, `dispack070`, `delta`, `rzwb` (Razor) | 38/38 files MD5-verified, ~2 min, Wine-11.18 on macOS 26 (Apple Silicon) |

## Development

| Path | Contents |
|---|---|
| `src/main.cpp` | command line and orchestration |
| `src/worker.cpp` | `--worker` mode: loads `unarc.dll` and calls `FreeArcExtract` |
| `src/core.cpp` | platform-independent helpers (unit-tested in `tests/test_core.cpp`) |
| `src/winutil.cpp` | Windows helpers: Wine path mapping, MD5, job objects |
| `build.zig`, `build.zig.zon` | build of the exe and its dependencies |

`zig build -Doptimize=debug` builds without optimizations. `.github/workflows/build.yml` runs the tests and the build
on every push; pushing a `v*` tag publishes a release with the exe.
