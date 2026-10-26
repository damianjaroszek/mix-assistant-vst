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
  Bump `bus::kVersion` whenever `AgentData`, `Command` or the layout changes. Spectra on the bus are
  raw ERB dBFS, pre-trim and pre-EQ; the Brain adds trim and the Agent's EQ to get what the mix hears.
- The Brain only writes `Command`s (apply / undo) into Agent slots; Agents change their own parameters.
  `ma::solve` works from pre-EQ spectra, so solving again is idempotent rather than stacking cuts.
- Dynamic unmasking (`ma/Ducker.h`): the Agent has an optional sidechain input bus; the duck dips a bell
  (or the whole channel) by how far the sidechain's band envelope is over the threshold. Commands carry
  the full Agent state (trim, EQ, duck, polarity), so the Brain fills unchanged fields from `AgentData`.
- Translation (`ma/Translation.h`): rough playback-system curves (phone, laptop, earbuds, car, club) used both
  for analysis (ear-weighted share of each channel vs the mix) and for the Brain's listening preview.
  Agents publish `outputDb` (after their whole chain) for it; `ma/Harmonics.h` adds overtones and
  `predictHarmonics` / `predictTransients` (`ma/Transient.h`, attack emphasis) let the Brain size a proposal
  before applying it; the translation steps are chosen most important channel first on a running trial mix.
- Phase 6: `ma/Saturator.h` (4x oversampled shaper, polyphase IIR half-bands, mix inside the oversampled domain so
  dry and wet stay in phase) sits after the Agent's EQ; it is not part of `Command` or the A/B backup. The Agent's
  `intentionalDrive` lifts the peak limit on Learn and the Brain's clipping warning. `ma/Instrument.h` guesses a role
  from the Learn pass (rule-based on temporal features plus the ERB spectrum, tested on synthetic parts only) and sets
  it with `defaultPriority` only while the role is still "Inny". Adaptive mode is `CommandKind::adapt` (EQ only, the
  backup stays): one step a second of at most 0.25 dB towards the solver's answer, with enter/release hysteresis.
- Phase 7 "Inżynier miksu": `ma/ChannelChain.h` (cleanup = 24 dB/oct HPF + phase-coherent mono-below + mud bell,
  RMS compressor, 4x oversampled clipper, bass-note evener driven by a zero-crossing pitch tracker) sits after the Brain EQ
  (order: cleanup, EQ, clipper, compressor, evener, saturator...). It IS part of `Command` and the A/B backup. The Learn pass
  stores level percentiles and the usual low note (persisted in the Agent state). `ma/ChainPlanner.h` plans every channel at once
  from raw post-trim spectra plus those stats (role profiles, compression capped per role so arrangement dynamics stay); the Brain's
  `proposeWholeMix` runs it, re-runs the normal proposal on the mix with the planned cleanup, and builds a per-channel verdict.
  Resonances (`ma/Resonance.h`): the Learn analyser tracks fine FFT bins (`SpectrumAnalyzer::setTrackBins`), a peak counts
  when it stands out in >= 70% of playing frames; the planner cuts up to three (cleanup notches) only where audible in the mix,
  never below 300 Hz on drums and bass, and on tonal roles only broad (Q <= 8) bumps.
  Phase 8 stereo (`ma/Stereo.h`, `ma/StereoPlanner.h`): per-ERB-band mid/side power (two mono `SpectrumAnalyzer`s),
  coherence = (M-S)/(M+S). Agents publish live input mid/side (pre-trim, pre stereo EQ); the Brain measures the master the same
  way for the "Stereo" view and report only. `planStereo` works from the channels' power sums (not the master, which already
  carries applied cuts) so a second pass is idempotent. Side cuts on anti-phase bands (channel's own < 0, or the sum < 0 and the
  channel brings >= 30% of the side), +1 dB side lifts only on wide roles where the sum is >= 0.85. `StereoShaper` (side-only bells)
  sits right after cleanup and is part of `Command`.
  Phase 9 stem references (`ma/StemReference.h`): groups drums / bass / vocals / music (roles map to them, bus and send
  left out); per group the ear-weighted level vs the sum of the groups, its own shape (level per broad region minus its
  mean over the regions it plays in, so a dull vocal shows on the vocal) and side share. `StemProfiler` measures a
  reference's stems over the drop only (short-term loudness within 3 dB of the loudest); the Brain accumulates each
  channel's raw spectrum over its own drop the same way (`accumulateDrop`, reset with `requestReset`). `planStemLevels`
  moves group levels (trims, thresholds with them) keeping the overall loudness; the solver's `stems` stage adds broad
  bells per group from pre-EQ drop spectra. Reference stems load on a worker thread (JUCE readers, MP3 enabled in the Brain).
  `MixSnapshot::Channel::rawDb` is pre-cleanup, `bandsDb` post-cleanup pre-EQ, `heardDb` post-EQ.
  Phase 10 key (`ma/Key.h`): `KeyDetector` (downsampled to ~11 kHz, 8192-point FFT, spectral peaks to pitch classes, plus a
  bass-only profile below 160 Hz) and `estimateKey` (Sha'ath profiles; the relative key is a tie decided by the bass, minor
  by default). Profiles keep each group's ERB mid/side spectrum and their key (`key`/`mid`/`side` lines in `.mastems`; old sets
  parse without them and are never shifted). `keyedStemTarget` shifts bass/vocals/music spectra (`shiftErbSpectrum`, power
  spread evenly over each band) by `keyShiftSemitones` (via relative majors, -5..+6) and re-measures; the Brain keeps the saved
  set (`currentStems`) and the keyed one (`keyedStems`, used everywhere for comparing and planning). The master's key detector
  ignores `requestReset`; its chroma is saved in the Brain state (`keyChroma`, `keySeconds`) with the manual `keyOverride`.
  Phase 11 pre-master: `ma/MasterPreview.h` (analysis only, on the Brain's audio thread): gain to the `masterTarget` parameter
  (set from the timer as target minus integrated; a move over 1 dB restarts its statistics), a lookahead limiter (sliding minimum
  of the needed gain plus a moving average of the same length, so no overshoot; release 80 ms) and per-100 ms reduction stats.
  The Brain holds each channel's output peak (`peakHold`, Agents only publish their latest block). `masteringReportHtml` builds the
  "Raport dla Ozone" file from the snapshot.
- Tonal balance (`ma/TonalBalance.h`) is on ERB bands like everything else; the Brain's master
  analyser is ERB too. Targets are `.matarget` text files in the user's app data folder.

## Commands

- Core only (fast, no JUCE): `cmake -S . -B build-core -DMA_BUILD_PLUGINS=OFF && cmake --build build-core && ctest --test-dir build-core`
- Everything: `cmake -S . -B build && cmake --build build --config Release`
- CI (`.github/workflows/build.yml`) builds the Windows VST3s and uploads them as an artifact.

## Design reference

Architecture, phases and decisions: https://claude.ai/artifact/5erMybFtPRsZ84QrMGyBFY
