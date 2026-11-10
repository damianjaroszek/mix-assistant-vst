# Mix Assistant

VST3 mix assistant for FL Studio (JUCE 8, C++17). Owner writes Polish; UI strings are Polish
(wrap non-ASCII literals with `ma::ui::pl()` from `plugins/common/MaUi.h`).

## Layout

- `core/` DSP without JUCE (`ma_core`), plus WAV I/O (`ma_io`). Must stay allocation-free in
  `process()` because the plugins call it on the audio thread.
- `lab/` `ma-lab` CLI running the core on WAV stems.
- `tests/` plain-assert unit tests, run through CTest.
- `plugins/agent` per-channel plugin, `plugins/brain` master plugin. Audio thread publishes
  measurements through atomics; editors poll them on a timer. Both editors have a fixed size (Agent 940x600,
  Brain 1120x680) with the controls on tabs (`ma::ui::Tabs` / `ma::ui::Page` in `MaUi.h`); a new feature goes on a
  tab (or a new one), never into a taller window.
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
  dry and wet stay in phase) sits after the Agent's EQ; since phase 13 it is part of `Command` and the A/B backup. The Agent's
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
  Phase 12 break: `StemProfiler::measureDrums` (after `measureBlock`, same block) lets `chooseDrop` also find the break (drums smoothed
  over ~1 s at least 15 dB under their drop level, the sum within 20 dB of the drop, outside it, >= 8 s); `StemProfile::breakdown`
  keeps its groups and `belowDropDb`, `StemTarget::breakContrast` the corridor, `stemBreakTarget` turns a set into its breaks so the
  usual compare works (`StemComparison::Group::extra` = ours plays where the references are silent). The Brain accumulates our
  break only once the drop is known (`breakSums`, drums level from the drums-group channels); the break is report/view/HTML only,
  never planned or solved.
  Phase 13 saturation: `planChains` step 4 picks saturation for tonal roles thin in overtones (bass: power above 150 Hz vs below
  120 Hz; others: 2nd-8th harmonic region vs the fundamental region) using `predictSaturation` (runs the real `Saturator` on a tone
  at the part's loud level, Goertzel per harmonic): the gentlest of a drive/mix ladder that reaches the role's target, stepped down
  while its predicted overtones raise masking of a more important channel (`analyseMasking`, +0.05 to >= 0.2). Never on kick,
  percussion, effects, bus, send or `intentionalDrive` channels (the Brain then keeps the Agent's own setting). `proposeWholeMix` also
  turns off a compressor / clipper / evener the plan does not want, when the plan knows the dynamics.
  Phase 14 kick tuning: the Learn pass also measures the pitch a hit's tail settles on (`Summary::tailPitchHz`, 150-300 ms after
  onsets, 30-130 Hz, 10-cent histogram median, >= 4 onsets), published as `AgentData::tailPitchHz` and persisted with the stats.
  `ma/KickTuning.h` `analyseKickTuning` places it in the key (tonic / fifth / in scale / out of scale, natural minor or major),
  against the bass's usual note (clash = 1, 2, 6, 10 or 11 semitones) and picks the nearest tonic or fifth in 35-80 Hz. Advice only:
  the Brain's report item "Stopa a tonacja" and the kick's verdict line; the retuning is done in FL's sampler.
  Phase 15 bus glue: channels with role `bus` are left out of every mix sum (masking, translation, solver, `planChains` steps
  1-4, stems), since their sources have Agents of their own. `planChains` step 5 gives a bus with Learn stats a glue compressor
  by what it carries (`ChainChannel::contentRole` = the Agent's `detectedRole`, else crest >= 14 dB means drums): drums 4:1,
  30 ms, 1/16 release, 3 dB on P95; vocals 2:1, 10 ms, 1/8, 2 dB; music 2:1, 30 ms, 1/4, 1.5 dB; none when the crest says the
  sources arrive squashed. On apply a bus keeps its trim; its thresholds move with its sources' gain and `Command::inputShiftDb`
  shifts its published learned levels (persisted, reset by Learn, restored by undo), so a second plan is idempotent.
  Phase 16 intro and build: `StemSection` {intro, build, breakdown}; `StemProfile::section()`, `StemTarget::contrast()`,
  `stemSectionTarget`. The profiler finds drop entries (first drop block after >= 4 s without one, moved back 1.5 s for the
  short-term loudness's lag): build = the 8 s before each entry outside the drop (>= 4 s total), intro = before the first entry
  minus its build (>= 8 s); the break now only counts after the first entry and may overlap a build. File lines `intro`/`build`
  plus `i`/`u`-prefixed `group`/`mid`/`side` (like `break`/`b...`). The Brain bins every update by the host's song position
  (`songBins`, 1 s, position minus 0.5 s) and in `updateSongSections` finds the first drop as the first 4 s run of bins at the
  drop's level (minus 1.5 s), so a later louder drop does not move it; `MixSnapshot::sections` holds all three. Report/view/HTML only.
  Phase 17 sketch mode (`ma/Sketch.h`): `arrangementFromStems` (each group's drop spectrum relative to the whole, averaged over
  profiles, at the level corridor's centre; expected = most profiles have it) or `typicalArrangement` (house shapes, no vocal);
  `analyseSketch` anchors it to the groups that play and makes ghosts: a whole missing group, or a region of a present group none
  of its channels carries (within 10 dB of its own loudest region) that is >= 6 dB (9 typical) short and not `cutOnPurpose`.
  The Brain (`updateSketch`, drop only, `sketchOn` saved as `sketchMode`) adds them to the tonal comparison (`addGhosts`,
  `arrangementRegions` = plain regions the ghosts explain), to stem levels (`addGhostGroups`; `planStemLevels` `fixed`), to the
  solver (`SolverChannel::ghost`: never cut, cuts making room for one capped at 3 dB, tonal excess left alone when ghosts carry
  >= 30%; `StemShapeInput::arrangementHole` skipped) and to collisions; report "Aranż", tab "Aranż", advisory proposal items.
  Phase 18 one correction per problem: with `SolverOptions::tonalTarget` + `tonalMasterDb` the solver's tonal stage predicts the
  master per band from the change of each channel from `nowDb` (Brain: `Channel::measuredDb`, untouched by a planned cleanup) or
  rawDb + `currentEq` to rawDb + `gainDb` + proposed EQ, re-compares after each fix (largest region first, each range once) and steps
  an overshooting bell back (< 1 dB dropped); a tonal cut where a deep enough cut already sits is not recorded. `proposeWholeMix`
  plans stem group levels first (`planLevels`), solves with them as `gainDb`, re-plans with the proposed EQ and re-solves once if a
  group moved more than 0.5 dB.
  Phase 19 section automation: `planSectionAutomation` (StemReference) per section: group levels into the section corridor (shared
  `balanceGroups` with `planStemLevels`, up to 12 dB, never above the group's drop level), groups the references lack there muted,
  then all audible groups shifted together until the K-weighted (`kWeightingDb`) loudness change puts `belowDropDb` in the contrast
  corridor; < 1 dB dropped. The Brain plans it in the timer (`MixSnapshot::Section::automation`, `ranges` from `SectionSums::seconds`,
  intro/build measured with a 1 s guard each side of a boundary, break ranges before the first drop hidden); report, HTML and the
  "Referencje" tab's "automatyka" column. Advice only: it belongs in FL's channel volume (before the Agent) so a re-measure is idempotent.
  Phase 20 energy map (`ma/EnergyMap.h`): `StemProfile::energy` (`EnergyTimeline`: whole and per-group short-term loudness a second
  minus the drop's, from `StemProfiler::measureGroup` in pass 1; `energy`/`egroup` lines in `.mastems`). `energyEntries` (first second
  >= -3 dB starting a 4 s run after a 4 s gap, minus 1.5 s) for both sides, `energyCorridor` lines references up at their first drop,
  `compareEnergy` gives first drop / drop count / deepest 4 s between drops against them plus >= 8 s spans >= 3 dB outside. The Brain
  builds ours from `songBins` (`songEnergy`: master short-term per second, group shares K-weighted from the channels' band powers);
  report "Przebieg utworu", HTML, tab "Energia" (`EnergyView`). Advice and view only.
  Phase 21 audible ghosts (`ma/GhostNoise.h`): random-phase FFT overlap-add noise (8192, square-root Hann, half overlap) shaped to an
  ERB spectrum as `SpectrumAnalyzer` reads it (the mid; a band narrower than a bin goes to its nearest bin), an independent side stream
  for the side share, optionally pulsed per beat (mean square kept). The Brain (`updateGhostNoise` in the timer) sums the sketch's
  ghosts into drums (pulsed, host ppq) and the rest, publishes them through atomics, and `ghostAudible` = `listenGhosts` (not saved)
  && sketch active && transport playing && short-term within 4 dB of the drop average. Added after every measurement, before the
  playback simulator; report warning while on.
  Phase 22 readability: one "Analizuj" (`BrainProcessor::analysisScope`: 1 = `proposeWholeMix`, 0 = `propose`) changes nothing; "Zastosuj (N)"
  counts selected non-advisory items; `brain::StepPath` shows the next step and that button is highlighted; after apply/undo the editor
  waits for `dropSeconds` to restart and reach `kMinimumDropSeconds`. `ReportItem::group` (mix / arrangement / setup): the view shows
  non-good items by group, worst first, good ones folded. Tonal balance is red/amber only for regions EQ can fix (`carriedByAChannel`:
  some channel within 24 dB of its own peak there); `chainAddsDb` (output minus heard, positive part) adds what an Agent's chain makes
  (saturation, harmonics) to the sketch's channel spectra and that check. `overtoneSource` (bass ghosts only) advises saturation first.
  Phase 23 auto transparency: `bus::kVersion` 13, `AgentData::brainTools` / `changedTools` (`bus::ToolBit`): the Agent keeps what the
  Brain last set (`BrainSet`, read back from the parameters after apply/adapt, restored by undo, saved as `brainSet`) and compares per tool;
  its editor shows "od Mózgu" / "zmienione ręcznie" / "ręcznie". The Agent saves the learn pass's level and true peak (`learned`); the planner's
  `ChainPlan::peakKnown` guards crest/clipper. `Proposal::Item::inPlace` (EQ band already in the Agent, `eqHasBand`): kept on apply, counted
  only when unticked; `proposeHarmonics` carries the Agent's own cuts in 300 Hz-6 kHz that the new EQ lacks (one room cut per channel).
  Collisions: `IssueHandling` pending / levelStep / settled from the last analysis (`collisionSteps`, kept after apply); `planCollisionSteps`
  adds `Action::channelLevel` (masker quieter by <= 3 dB). Verdicts are kept (`lastVerdicts`), lines tagged with a tool, `verdictsNow()` gives
  their state. `proposeWholeMix` compares cleanup with `saved` (the mix carries the planned cleanup for the solver).
  Phase 24 corridor centre: `ma::zoneOffsets` (TonalBalance) gives TBC's four ranges (`tonalZoneEdgeHz` 40/250/2000/9000/16000) as the
  mean of mix minus the corridor centre. `SolverOptions::centreZones` (Brain: always with a target) runs after the corridor stage in the
  predicting branch: largest offset beyond `centreDeadZoneDb` (0.75) first, ranges still outside the corridor skipped; grows the range's
  existing same-direction tonal bell or puts one broad bell on a free band (cut: least important carrier >= 25%, boost: most important
  >= 15% with the masking check), 0.5 dB steps, range moved <= 1.5 dB, bell <= 3 dB, no crossing the centre, never more outside the
  corridor (`outsideCorridorDb`); `SolverChange::centre/zone/zoneOffsetDb/zoneAfterDb`. `MasterPreview::limiterTiltDb` (ERB analysers on
  the limiter's input after gain and its output); the Brain publishes it (`previewTilt`), `limiterTilt()` removes its 60 Hz-8 kHz mean and
  caps it at +-2 dB, and `tonalMasterSpectrum()` (master + tilt while `tonalAfterLimiter`, saved, default on) feeds `mix.tonal`, the
  sketch and the solver; `tonalBeforeLimiter()` is the plain comparison drawn thin. Captured references stay untilted (they are masters).
  Phase 25 reference width: `StereoSettings::widen` (`StereoShaper`: the mid, delayed `delayMs` and high-passed twice at `lowHz`,
  added to the side at `amountDb`; mono sum untouched; `kWidenOffDb` -40 = off, Agent params `widenAmount`/`widenFrom`/`widenDelay`,
  packed before saturation in `brainSet`, old states get defaults). `widthTargetFrom(keyedStems)` = per stem group per region >= 300 Hz
  the reference profiles' median coherence; `planStereo(..., &width)` lifts existing side or, on a near-mono part, sets the widener
  (never kick, bass, bus, send; vocals lift only), each channel and the sum kept >= `kSafeCoherence` 0.2 (`keepSumInPhase` steps
  every widening in the worst band back: widener 1 dB, lift 0.5 dB). Without stem references the phase 8 behaviour stays. `bus::kVersion` 14.
  Phase 26 width slider (`ma/Width.h`): Agent params `width` (0-200 %, 100 = off) and `widthFrom` (mono below, default 150 Hz), the
  user's (not in `Command`, `brainSet` or the A/B backup). `WidthSettings` (in Stereo.h) goes to `StereoShaper::setWidth`: side LR4
  high-passed at fromHz (mono sum untouched), scaled, own widener from max(300, fromHz) replacing the Brain's, two guard side bells.
  `planWidth` (each Agent runs it in its timer every 0.3 s on all slots, heard = mid/side + trim + EQ, and takes its own row): above
  100 % the widest `reach` (side up to +6 dB, widener up to -2 dB) where two guards keep every audible band >= 0.2, the slider spread
  over it; too many anti-phase bands narrow the side as a whole; the sum (power sums) steps widenings back where a slider makes a band
  < 0.25 with >= 2 side sources (< 0.2 always). The stereo analyser runs while any slot uses a slider (`widthWanted`). AgentData carries
  the request and result (`bus::kVersion` 15); the Brain's `StereoChannel::userWidth` channels get no stereo proposal and count in
  the sum as they play (`processedSidePower`).
  Phase 27 stability: `propose` runs `proposeOnce` and, when the translation steps (`translationDelta`, from `proposeHarmonics`'
  trial minus base) move any band of the sum by >= 0.5 dB, once more with them as `SolverChannel::extraDb` (counted only in the
  tonal prediction); every translation step needs `tonalOk` (sum of distances outside the corridor grows by <= 1 dB), harmonics
  grow only while it holds. `planStereo` keeps an existing side cut (`StereoChannel::current`, as deep) while the band's own
  coherence < 0.15. `planWidth` precomputes per-channel tables (`SideModel`); Agents plan every 0.5 s or when their slider moves.
  Phase 28 closing the loop: `planCollisionSteps` judges collisions on `predictedHeard` (heard + proposal gain, cleanup and
  EQ deltas, `proposedEqFor` shared with apply), so one analysis adds `channelLevel` items for collisions its own cuts will not
  clear; `CollisionStep::clearedByProposal` / `applied` (set on apply). After apply an applied step reads settled; issues without a
  usable step get `liveLevelStep` (cached 2 s): amber `cutAtLimit` only when a step <= 3 dB would clear it, else settled.
  Loud master: `ChainContext::limiterReductionDb` (preview P95, >= `kMinimumPreviewSeconds`), `ChainChannel::carriesMasterPeaks`
  (kick/percussion within 6 dB of the highest unclipped peak: max(held output, learned true peak + trim)) and `currentClipDb`;
  planner step 3b adds `loudMasterClipDb` (applied extra + measured - 2.5, dead zone 1.5-3 dB, total <= 6 dB). `peakHold` only
  fills once the preview reset is consumed and 1 s measured. Editor viewports re-lay without shrinking (`fitToViewport`).
  Phase 29 balance without references: `ma::roleLevelOffsetDb` (Instrument.h: kick 0, bass -2, vocal -4, lead/percussion -6,
  pad -8, effects/send -10) is added to the Learn target when the Agent's `roleLevel` param is on (default; the role is recognised
  before the trim is planned; `LearnResult::roleOffsetDb/targetDb`). Without stem references (`typicalLevelsReady`) `proposeWholeMix`
  plans group levels against `typicalLevelTarget(present)` (Sketch.h: typicalArrangement levels +-2 dB (drums -8.5, bass -7.9, vocals -5, music -2 vs the whole: measured on two real productions with ma-lab), shifted so the present
  groups sum to the whole; level corridors only, no profiles: `planStemLevels` now needs only valid level corridors) and
  `anchorStemLevels` keeps the drums at 0 dB.
  Phase 30 no cable (`ma/Unmask.h`): `KeyFollower` (output, mono, 8 octave bands 125 Hz-16 kHz, 3 ms mean square then 5/25 ms
  attack/release) writes into the slot's `KeyFeed` (atomics outside the seqlock, `keyId`/`keySinceMs` there too) while another Agent
  `wantKey`s it (3 s, renewed each tick); `readKey` treats > 250 ms old as silence. `AgentData::keyId` is kept in the project (a clone
  that loads the same id while the original runs takes a new one). `Unmasker`: up to `kMaxUnmask` 2 dips (bell, depth, key bands), the
  key against its own recent loud level (full 6 dB under it, none 20 dB under), 10/150 ms; after the duck. `Ducker::processKeyed` is the
  duck without a cable (when no sidechain signal in 3 s; `duckKey`). Dips and keys are part of `Command` (`unmask`, `unmaskKey`,
  `duckKey`), the A/B backup and `brainSet` (packed dynamics 72 floats, keys beside them), `ToolBit::toolUnmask`; `bus::kVersion` 16.
  Brain: `planUnmask` (in `planCollisionSteps`, on the predicted mix) for collisions EQ and a <= 2 dB step do not clear, and for all of
  them while `groupLevelsPlanned` (no channel level steps then); never kick/percussion as masker; the bell over the masked channel's loudest
  part of the range (within 12 dB, 150 Hz-10 kHz), smallest of 2-6 dB that clears, else 6 dB if the score drops >= 0.15.
  `IssueHandling::unmasked`; kick ducks carry `duckKey` and count as ducked with `duckKeyFound`. `CommandKind::refine` = apply that keeps
  an existing backup; `applyByUser` starts `Refine` (param `autoRefine`, default on): after the drop (and preview, <= 15 s more) is
  measured again, analyse with the same scope, keep the user's unticked items unticked, apply, at most `kRefineRounds` 2, stop when the
  changes repeat. The last tonal target is remembered (`last-target.txt`) and selected in a project without one.
  Phase 31 decisions: `ma::bus::ToolNotes` (slot, own seqlock, `kVersion` 17): per tool (ToolBit order) a `ToolDecision`
  (proposed / kept / notNeeded / notChecked / yours) and a UTF-8 reason. `BrainProcessor::analyse(scope)` (editor and refine)
  runs the analysis, then `buildToolNotes` (decision sites fill `toolWhy` during the pass: translation steps, duck; the rest
  from the proposal, `lastPlans`/`lastStereoPlans` and the Agent's state), `addToolLinesToVerdicts`, `publishToolNotes`. The Agent
  keeps them (`brainNotes`, saved), shows them on the "Decyzje Mózgu" tab, in the origin labels and under the "Barwa" tools.
  Brain tab "Jak zacząć" (`GuideView`, `updateGuide`). `planChains` step 4: a near-sine bass (overtones > 12 dB under target)
  gets the harmonic enhancer (`ChainPlan::harmonics`, `sineSub`) instead of saturation; each is the other's fallback when it
  would cover a more important channel; `proposeWholeMix` adds its predicted overtones to the mix copy the proposal sees.
  mixcheck fixes (`tools/mixcheck`, `synth_song.py` makes its synthetic test song): while refining, `proposeOnce` gives back up to
  `Refine::globalGainDb` (the ceiling cut since the click, sized before the planned clippers) as a positive `globalGain`, leaving
  `kRestoreMarginDb` 1.5 dB (a refine round measures 8 s of the drop). The solver keeps each collision on its own EQ slot, records the
  depth it set (`SolverChange::gainDb`) and gives a dropped cut's depth back on a shared band; a ghost item shows its own <= 3 dB and
  names the real cut sharing the band. `roleLevelOffsetDb(role, raw)`: percussion with >= 60% of its power above 5 kHz (hats) -12 dB.
  A restore waits until the drop is measured as long as at the click (`Refine::peakSeconds`, <= 32 s). Learn leaves the trim (and
  drops the stats) of a part that hardly played (`LearnResult::barelyPlayed`: < -60 dB or active < 15% of the passage).
  mixcheck keeps its drop loop's phase across calls (short calls used to replay the drop's first 4 s), prints each Agent's learn
  result, and its channel table shows each part's change over the drop per TBC range.
  Second real-multitrack round: level percentiles count the 100 ms level only within 20 dB of the recent peak (`kLevelWindowDb`,
  release tails and pause edges are not quiet passages); vocal saturation drive <= 6 dB (`SaturationProfile::maxDriveDb`); Learn
  lifts at most `kLearnMaxBoostDb` 12 dB (`LearnResult::boostCapped`); `settledAdvice` gives drums "hits cover only for a moment"
  and a kick covering >= 1 kHz "another hit in the same track, split it" instead of "an octave up". mixcheck's channel table
  measures each part's output over the drop while rendering (the Agents' published spectrum is their latest second only).
  Producer's balance inside a group (`planGroupBalance`, whole-mix analysis only): music and vocal parts get back their raw-track
  level against the rest of their group (K-weighted over the drop, after trim and chain loss; group loudness kept; <= 15 dB, steps
  >= 0.5 dB) as `Action::balance` items (channel gain, like `channelLevel`), counted in the group levels and the solver
  (`balanceGain`, set only while `proposeWholeMix` runs). Drums and bass stay at the role levels: on both real songs the stems'
  own hats and sub balance (+5 dB each) made the mix worse. Idempotent: after apply the group's trims differ only by chain loss.
  Group levels count what each Agent's chain takes away over the drop (`chainLossDb`: K-weighted output vs heard, summed in
  `DropSum::chainIn/chainOut`, only losses, <= 15 dB), so a refining round gives back what compressors, ducks and dips took.
- Phase 32 problems that come and go (`ma/DynamicEq.h`): three dynamic band slots (`DynamicProbe` boom 250 Hz, harsh 3.2 kHz,
  sibilance 6.8 kHz; width, attack, release per slot from `dynamicProbeBand`), each cutting a bell 3:1 over its threshold up to its
  range while the band's 5 ms level minus the whole part's (mono) exceeds `thresholdDb` (relative, so a louder phrase does not
  trigger it; coefficients every 16 samples), plus `RiderSettings` (100 ms level towards `targetDb` within +-range, holds in pauses
  24 dB under target). After the Agent's EQ, before the clipper. Learn: `InstrumentFeatures` probes (band-pass on the mid, 20 ms,
  only within 20 dB of the recent peak) give `probeP50/probeP95` (band minus mid, dB), saved with the stats (13 numbers) and on the bus.
  `planChains` step 6: vocal/lead/pad/other only; harshness any of them, sibilance and boom vocals only; P95-P50 >= 6 dB (sibilance 8)
  and P95 >= -20 dB, skipped where the mix covers the range at its spikes (`probeBuried`), threshold max(P50+3, P95-5), range
  spike/2 (+1.5 over a more important channel; sibilance <= 6, others <= 4); rider on vocals with P50-P10 >= 4 dB (target P50,
  range <= 4). Part of `Command` (rider target moves with the channel's gain on apply), the A/B backup and `brainSet` (packed
  dynamics 97 floats; 72 = before phase 32), `ToolBit::toolDynamicEq`, `kToolCount` 12, `bus::kVersion` 18. Agent tab "Dynamika";
  the Brain's chain item `ChainTool::dynamicEq` (a Brain-made setting the plan drops goes off, the user's stays), notes and verdict line.
- Phase 33 panning (`ma/Pan.h`): `Panner` is the Agent's last stage (after the dips; the output spectrum is measured before it, so
  chain loss and level plans ignore a pan; the peak after it): constant power, unity in the centre (sqrt2 cos/sin), below
  `monoBelowHz` (default 150, LR4 split that sums flat) the signal stays put. `BalanceMeter` publishes the input's lean
  (`AgentData::inputBalanceDb`, L minus R above 150 Hz). `planPan` (whole-mix analysis, after `planStereo`, from the same pre-pan
  mid/side): kick, bass, bus, send and the loudest vocal and lead stay centred; user pans (on but not in `brainTools`) and inputs
  leaning >= 3 dB are counted where they are; wide (coherence < 0.3) and quiet (< -40 dB of the mix) parts stay; the rest by role
  (perc/fx 0.45, other 0.35, pad/lead 0.3, vocal 0.25, less when partly wide): pairs sharing >= 35% of their 150 Hz-16 kHz spectrum go
  opposite, then singles where they balance the mix; then the loudest leaning part steps back 0.05 until the mix is within 0.5 dB and
  each region (150-2k, 2-9k, 9-16k) within 1.5 dB. Part of `Command`, the A/B backup and `brainSet` (packed dynamics 99 floats, 97 =
  before phase 33), `ToolBit::toolPan`, `kToolCount` 13, `bus::kVersion` 19; Agent "Stereo" tab, Brain `ChainTool::pan`, notes, verdict.
- Phase 34 depth (`ma/Depth.h`): `DepthSettings::highCutHz` (12 dB/oct `HighCut`, right after the cleanup; `depthResponseDb` is the
  digital response at 48 kHz and goes into `applyCleanup`, so `bandsDb` carries it). `planChains` step 7: a `send` whose `topTiltDb`
  (8-16 kHz minus 1-4 kHz) is above the rest of the mix's minus `kDarkerThanMixDb` 3 gets `kSendHighCutHz` 9 kHz. Learn never lifts a send's trim (`LearnResult::sendKept`: a return is set by the source channels' send knobs; it only lowers one louder than its role level). The Brain
  (`proposeWholeMix`) also has every send without a cable duck the whole signal by `kSendDuckDb` 3 dB under `mainVoice()` (loudest
  vocal, else lead; keyed over the bus, 10 ms, release 1/4 synced) and adds an advisory item with `depthTimes` (pre-delay 1/64, decay
  half a bar, delay 1/8, dotted 1/8, 1/4; two items `reverbAdviceText` / `delayAdviceText` named by the usual knobs); report "Głębia (pogłos, delay)" (sends vs dry 200 Hz-8 kHz, warning over -6 dB). Part of
  `Command`, the A/B backup and `brainSet` (packed dynamics 100 floats, 99 = before phase 34), `ToolBit::toolDepth`, `kToolCount` 14,
  `bus::kVersion` 20; Agent param `highCut` (20 kHz = off) on the "Inżynier miksu" tab. mixcheck names reverb/delay/return/send files as sends.
- Phase 35 kick-bass time and the Ozone gate (`ma/Alignment.h`): `LowAlignMeter` (both LP2 150 Hz, decimated ~2 kHz, cross-correlation
  +-10 ms, ~2 s decay, gated on the key's energy) runs on the main and sidechain mono before the chain; `AlignDelay` (<= 10 ms, before the
  cleanup, so the lag is measured undelayed and planning again is idempotent). AgentData `align`, `alignValid/LagMs/Best/Zero` (> 0 = main
  later). Brain `alignAdvice` (bass with a cable only): |lag| >= 0.5 and <= 5 ms, best >= 0.3, best - zero >= 0.1, zero >= -0.2 (else
  polarity first); an early bass gets the delay (`ChainTool::align`, whole-mix analysis), a late one an advisory item (move it in FL),
  a Brain-made delay no longer needed goes off; report "Stopa i bas w czasie", notes, verdict "Czas ze stopą: ". Part of `Command`, the
  A/B backup and `brainSet` (packed dynamics 101 floats, 100 = before phase 35), `ToolBit::toolAlign`, `kToolCount` 15, `bus::kVersion` 21;
  Agent param `alignDelay` on the "EQ i ustępowanie" tab. `ReportItem::gate` "Gotowe dla Ozone?" is inserted first: red items outside the
  arrangement group (plus ghosts / playback simulator on) block, amber ones are listed to check; the view always shows it on top and the
  guide's and HTML's problem counts skip it. mixcheck `--kick-cable` feeds the kick stem into the bass Agents' sidechain, `--shift-bass MS`.
- Phase 36 ducked reverb: `ma::planSendDuck` (Depth.h) sizes a send's duck under `mainVoice()` from both drop spectra (`dropSpectrumDb`,
  raw + trim, pre-duck so it is idempotent): a bell (not the whole signal) at the voice's loudest ERB band in 800 Hz-5 kHz, Q 0.8, depth =
  send minus voice over 1-5 kHz + `kSendUnderVoiceDb` 12, clamped 2-8 dB in 0.5 dB steps (3 dB until the drop is measured). Kept while the
  Agent's duck is a bell keyed to the voice within 1 dB and 0.3 octave.
- Phase 37 what a send gives way to: `ma::findSendSources` (Depth.h) fits a send's per-second power in 8 regions (150 Hz-12 kHz,
  normalised per region) as a non-negative sum of the candidates' (0.7 this second + 0.3 the last, for the tail; coordinate-descent NNLS;
  >= `kMinSourceSeconds` 12 s within 40 dB of its loudest; valid with uncentred fit >= 0.7). The Brain (`sendSources`, from `songBins`,
  candidates = channels in the mix sums except sends) picks per send: Agent param `sendDuckTo` (`bus::SendDuckTo` auto / voice / source /
  none, `AgentData::sendDuckTo`, the user's, `bus::kVersion` 22; "Inżynier miksu" tab, enabled on sends): auto = drums source -> whole-signal
  duck under it (2 ms, 1/16 synced, depth over 200 Hz-8 kHz, <= `kSendDuckSourceMaxDb` 6), else the main voice (phase 36), else its source
  (bell 300 Hz-6 kHz, 1/8); "none" turns a Brain-made duck off. `lastSendNotes` feeds the "Głębia" report line (sources, key).
- Tonal balance (`ma/TonalBalance.h`) is on ERB bands like everything else; the Brain's master
  analyser is ERB too. Targets are `.matarget` text files in the user's app data folder.

## Commands

- Core only (fast, no JUCE): `cmake -S . -B build-core -DMA_BUILD_PLUGINS=OFF && cmake --build build-core && ctest --test-dir build-core`
- Everything: `cmake -S . -B build && cmake --build build --config Release`
- Offline check on real stems: `-DMA_BUILD_MIXCHECK=ON`, target `MixCheck` (`tools/mixcheck`): the real Agents and Brain
  on a stems folder (learn on the loudest 16 s, Brain window flagged open so Agents send spectra, `analyse(1)`, `applyByUser`,
  refine), renders `ours.wav` and compares with `--reference` at matched loudness (TBC zones, LUFS, PLR, width). Runs at
  about real time (Brain timers use the wall clock); run it with HOME pointed at a scratch folder.
- CI (`.github/workflows/build.yml`) builds the Windows VST3s and uploads them as an artifact.

## Design reference

Architecture, phases and decisions: https://claude.ai/artifact/5erMybFtPRsZ84QrMGyBFY
