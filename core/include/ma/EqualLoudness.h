#pragma once

namespace ma
{

// Equal-loudness contours after ISO 226:2003 (defined 20 Hz to 12.5 kHz; outside
// that range the nearest edge value is used).
//
// Sound pressure level in dB SPL a pure tone at `hz` needs to sound as loud
// as a 1 kHz tone at `phon` dB SPL.
double equalLoudnessSpl(double hz, double phon);

// How many dB quieter the ear hears `hz` than 1 kHz at the given listening level.
// Subtract it from a measured band level to get a level as the ear weighs it.
// Default 80 phon is a typical mixing level.
double equalLoudnessWeightDb(double hz, double phon = 80.0);

} // namespace ma
