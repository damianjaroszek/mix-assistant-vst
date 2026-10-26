#pragma once

#include "SpectrumAnalyzer.h"

#include <array>

namespace ma
{

// Resonances: narrow peaks that stick out of a channel's spectrum and stay put whatever
// is played (a ringing drum shell, a boxy room, a harsh synth formant). Found on the Learn
// pass from the fine FFT bins; the planner decides in the context of the mix whether and
// how deep to cut them. A melody's harmonics move with the notes, so they rarely qualify.
struct Resonance
{
    float hz = 0.0f;           // 0 = none
    float prominenceDb = 0.0f; // over the surrounding third of an octave
    float q = 0.0f;
    float persistence = 0.0f;  // share of playing frames in which it stood out
};

constexpr int kMaxResonances = 3;
using Resonances = std::array<Resonance, kMaxResonances>;

// Strongest first. Message thread only: it allocates.
Resonances findResonances(const SpectrumAnalyzer& analyser, double minHz = 150.0, double maxHz = 12000.0);

} // namespace ma
