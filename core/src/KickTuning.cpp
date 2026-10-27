#include "ma/KickTuning.h"

#include <array>
#include <cmath>

namespace ma
{
namespace
{
constexpr double kMaxCents = 25.0;
constexpr double kLowestTailHz = 35.0, kHighestTailHz = 80.0;
constexpr double kFifthPenaltyCents = 50.0; // the tonic is the first choice, the fifth close behind

double exactMidi(double hz)
{
    return 69.0 + 12.0 * std::log2(hz / 440.0);
}

bool inScale(int degree, bool minor)
{
    static constexpr std::array<int, 7> natMinor { 0, 2, 3, 5, 7, 8, 10 };
    static constexpr std::array<int, 7> major { 0, 2, 4, 5, 7, 9, 11 };
    for (int d : minor ? natMinor : major)
        if (d == degree)
            return true;
    return false;
}
} // namespace

std::string noteNameOf(int midiNote)
{
    static const char* const names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const int pc = ((midiNote % 12) + 12) % 12;
    const int octave = midiNote / 12 - 1 - (midiNote < 0 && midiNote % 12 != 0 ? 1 : 0);
    return std::string(names[pc]) + std::to_string(octave);
}

KickTuning analyseKickTuning(double kickHz, MusicalKey key, double bassHz)
{
    KickTuning t;
    if (!(kickHz > 20.0) || !key.valid())
        return t;
    t.valid = true;
    t.kickHz = kickHz;
    t.key = key;
    const double exact = exactMidi(kickHz);
    t.midiNote = static_cast<int>(std::lround(exact));
    t.cents = (exact - t.midiNote) * 100.0;
    const int pc = ((t.midiNote % 12) + 12) % 12;
    t.degree = (pc - key.tonic + 12) % 12;
    t.fit = t.degree == 0 ? KickTuning::Fit::tonic
            : t.degree == 7 ? KickTuning::Fit::fifth
            : inScale(t.degree, key.minor) ? KickTuning::Fit::inScale
                                           : KickTuning::Fit::outOfScale;

    if (bassHz > 20.0)
    {
        t.bassKnown = true;
        t.bassHz = bassHz;
        const int bassPc = ((static_cast<int>(std::lround(exactMidi(bassHz))) % 12) + 12) % 12;
        t.bassInterval = (pc - bassPc + 12) % 12;
        t.clashesWithBass = t.bassInterval == 1 || t.bassInterval == 2 || t.bassInterval == 6 || t.bassInterval == 10
                            || t.bassInterval == 11;
    }

    t.needsTuning = t.fit == KickTuning::Fit::outOfScale || std::abs(t.cents) > kMaxCents || t.clashesWithBass;
    t.couldBeBetter = !t.needsTuning && t.fit == KickTuning::Fit::inScale;
    if (!t.needsTuning && !t.couldBeBetter)
        return t;

    // The nearest tonic or fifth (the tonic a little preferred) in a kick's usual range.
    double best = 1e9;
    for (int shift = -6; shift <= 6; ++shift)
    {
        const int note = t.midiNote + shift;
        const int degree = (((note % 12) + 12) % 12 - key.tonic + 12) % 12;
        if (degree != 0 && degree != 7)
            continue;
        const double hz = 440.0 * std::pow(2.0, (note - 69) / 12.0);
        if (hz < kLowestTailHz || hz > kHighestTailHz)
            continue;
        const double move = (note - exact) * 100.0;
        const double score = std::abs(move) + (degree == 7 ? kFifthPenaltyCents : 0.0);
        if (score < best)
        {
            best = score;
            t.targetMidiNote = note;
            t.targetHz = hz;
            t.shiftCents = move;
            t.shiftSemitones = shift;
        }
    }
    if (best > 1e8) // nothing in range: keep the note, fix the cents only
    {
        t.targetMidiNote = t.midiNote;
        t.targetHz = 440.0 * std::pow(2.0, (t.midiNote - 69) / 12.0);
        t.shiftCents = -t.cents;
        t.shiftSemitones = 0;
    }
    return t;
}

} // namespace ma
