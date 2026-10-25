# Mix Assistant

VST3 mix assistant for FL Studio (JUCE 8, C++17). Owner writes Polish; UI strings are Polish
(wrap non-ASCII literals with `ma::ui::pl()` from `plugins/common/MaUi.h`).

## Layout

- `core/` DSP without JUCE (`ma_core`), plus WAV I/O (`ma_io`). Must stay allocation-free in
  `process()` because the plugins call it on the audio thread.
- `lab/` `ma-lab` CLI running the core on WAV stems.
- `tests/` plain-assert unit tests, run through CTest.
- `plugins/agent` per-channel plugin, `plugins/brain` master plugin. Audio thread publishes
  measurements through atomics; editors poll them on a timer.
- Agents talk to the Brain through `ma::bus` (`core/include/ma/MixBus.h`), a seqlock slot table
  in a memory-mapped temp file (`plugins/common/SharedSession.h`), so it works across processes.
  Bump `bus::kVersion` whenever `AgentData` changes. Spectra on the bus are raw ERB dBFS, pre-trim.

## Commands

- Core only (fast, no JUCE): `cmake -S . -B build-core -DMA_BUILD_PLUGINS=OFF && cmake --build build-core && ctest --test-dir build-core`
- Everything: `cmake -S . -B build && cmake --build build --config Release`
- CI (`.github/workflows/build.yml`) builds the Windows VST3s and uploads them as an artifact.

## Design reference

Architecture, phases and decisions: https://claude.ai/artifact/5erMybFtPRsZ84QrMGyBFY
