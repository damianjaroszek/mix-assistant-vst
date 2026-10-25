#pragma once

#include <string>
#include <vector>

namespace ma
{

struct AudioData
{
    double sampleRate = 0.0;
    std::vector<std::vector<float>> channels; // planar

    int numChannels() const { return static_cast<int>(channels.size()); }
    int numSamples() const { return channels.empty() ? 0 : static_cast<int>(channels.front().size()); }
};

// Reads PCM 16/24/32-bit and IEEE float 32/64-bit WAV files (incl. WAVE_FORMAT_EXTENSIBLE).
// Throws std::runtime_error with a readable message on failure.
AudioData readWav(const std::string& path);

// Writes 32-bit float WAV.
void writeWav(const std::string& path, const AudioData& audio);

} // namespace ma
