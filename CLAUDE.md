# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Barber Pole is a Traktor-style "Barber Pole" Colour FX plugin for VirtualDJ 8, written in C++11 and built with CMake.

## Build

The real plugin needs `vdjPlugin8.h` and `vdjDsp8.h` from the VirtualDJ SDK in `SDK/`. They're gitignored, so download them from https://virtualdj.com/developers/VirtualDJ8_SDK_20211003.zip. CMake stops with an error if they're missing.

```
cmake -S . -B build -A x64            # Windows 64-bit; also builds knob_sequencer.exe and compare_recordings.exe
cmake --build build --config Release  # -> build\Release\BarberPole.dll
cmake -S . -B build32 -A Win32        # 32-bit plugin, kept in a separate folder
cmake -S . -B build -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_BUILD_TYPE=Release   # macOS universal bundle
```

To check the code compiles without the real SDK (Linux/macOS only), point `VDJ_SDK_DIR` at the stubs: `-DVDJ_SDK_DIR=test/sdk_stub`. Never ship a build made against the stubs.

## Tests

These run on Linux/macOS (WSL on Windows). `test/analyze.py` drives the DSP core through ctypes:

```
cd test
g++ -O2 -std=c++11 -shared -fPIC -o libbp.so capi.cpp
python3 analyze.py        # prints PASS/FAIL per check; needs numpy
```

`test/benchmark.cpp` measures the DSP's speed and prints a hash of its output per case: `g++ -O2 -std=c++11 -I../src -o benchmark benchmark.cpp && ./benchmark`. Run it before and after a performance change; matching hashes mean the sound didn't change (compare builds from the same compiler only).

`test/host_sim.cpp` loads a built plugin through `DllGetClassObject` the way VirtualDJ does: `host_sim <path to plugin>`. Add `-DREAL_SDK` when compiling it against the real headers.

## CI and releases

`.github/workflows/build.yml` builds the Windows x64 and x86 DLLs and the macOS universal bundle on every push and PR. It also checks that both DLLs export an undecorated `DllGetClassObject`. Pushing a `v*` tag creates a GitHub release with the zipped builds. Running the workflow by hand (workflow_dispatch) with `release_tag` set, e.g. `v2.5`, does the same and creates the tag on the commit it built; use this from cloud sessions, which can't push tags.

To release, bump the version in all three places:

1. `info->Version` in `src/BarberPole.cpp`
2. The README title
3. The tag, `vX.Y.Z` (pushed, or the workflow's `release_tag` input)

## Architecture

- `src/barberpole_dsp.h` is the whole DSP: header-only, namespace `barberpole`, with no SDK dependency. `BarberPole::prepare(sampleRate)`, `setParams(color, speed, resonance, beatsPerSecond)` and `process(interleavedStereo, frames)` are the interface. Inside it:
  - A 4th-order state-variable DJ filter.
  - A `BandPassBank` of 8 octave-spaced notches in two stages, with resonant peaks between them, faded in and out at the range ends (the Shepard/Risset illusion).
  - A `DenormalGuard` for x86.
- `src/BarberPole.h/.cpp` is a thin `IVdjPluginDsp8` wrapper (`CBarberPole`). It declares the parameters, including the Colour knob through `DeclareParameterColorFX`, maps the track's BPM to beats per second, and forwards audio to the DSP.
- `src/BarberPole.def` exports `DllGetClassObject` undecorated. 32-bit `__stdcall` builds need this, or VirtualDJ can't load the DLL.
- `tools/` holds Windows-only helpers for comparing against Traktor: `knob_sequencer` (MIDI knob playback through loopMIDI) and `compare_recordings`. The README describes the workflow.

## Invariants

- The parameter IDs (`ID_COLOR`, `ID_SPEED`, `ID_RESONANCE`) must never change: VirtualDJ stores settings and controller mappings by them.
- Knob centre ±3 % must be a bit-exact bypass, and returning to centre must end fully clean. `analyze.py` checks both.
- Constants marked "matched to Traktor" were fitted to recordings. Don't change them without re-running the comparison test.
- `test/capi.cpp` names and signatures are called by name from `analyze.py`. Change both together.
- Performance changes should keep output bit-identical. v2.4.3 sped the DSP up by about a third; across the benchmark and 30 s knob sweeps at 44.1/48/96 kHz the only difference from v2.4.2 is one sample in a 96 kHz silent decay tail, off by 2e-25.
- Windows builds use the static MSVC runtime, so users don't need the VC++ redistributable.

## Conventions

- Use British English in prose, comments and the README ("colour", "centre"). Keep SDK and VDJScript names in their real American spelling (`DeclareParameterColorFX`, `filter_selectcolorfx`, the `Color` parameter).
- Audio test files (`*.wav`, `*.flac`), recordings and analysis output are gitignored and kept locally only.

## Working style

- Be concise: no recaps, and don't echo code back. Read only the line ranges you need, and search before reading.
- Keep command output short: filter it, or use `| tail`.
- Use the `searcher` agent for broad searches and `/commit` for commits.
