#pragma once

#include "Key.h"

namespace ma
{

// Phase 14: a kick's low tail is a note. In house and trance it is tuned to the track's
// tonic or fifth; out of the key, or a semitone away from the bass, the low end beats
// against itself on every hit. The kick's pitch comes from the Agent's Learn pass (the
// tail 80-200 ms after each hit), the key from the Brain, the bass from its usual note.
struct KickTuning
{
    bool valid = false;
    double kickHz = 0.0;
    int midiNote = 0;    // nearest note
    double cents = 0.0;  // kick against that note, -50..50
    MusicalKey key;
    int degree = -1;     // semitones above the tonic, 0..11
    enum class Fit
    {
        tonic,
        fifth,
        inScale,
        outOfScale
    };
    Fit fit = Fit::outOfScale;

    bool bassKnown = false;
    double bassHz = 0.0;
    int bassInterval = 0;       // kick's pitch class above the bass's, 0..11
    bool clashesWithBass = false; // a second or a tritone apart: they beat

    // The suggestion: retune the sample by this much to sit on the tonic or the fifth,
    // within the usual range of a kick's tail (35-80 Hz). 0 / 0 when nothing is needed.
    int shiftSemitones = 0;
    double shiftCents = 0.0;    // the whole move in cents (semitones and the fine part)
    double targetHz = 0.0;
    int targetMidiNote = 0;
    bool needsTuning = false;   // out of the key, more than 25 cents off, or against the bass
    bool couldBeBetter = false; // in the key, but not on the tonic or the fifth
};

KickTuning analyseKickTuning(double kickHz, MusicalKey key, double bassHz = 0.0);

// "F#1" style (sharps), for display; MIDI 69 = A4.
std::string noteNameOf(int midiNote);

} // namespace ma
