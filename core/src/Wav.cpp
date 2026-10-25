#include "ma/Wav.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace ma
{
namespace
{
constexpr std::uint16_t kFormatPcm = 1;
constexpr std::uint16_t kFormatFloat = 3;
constexpr std::uint16_t kFormatExtensible = 0xFFFE;

std::uint32_t readU32(const unsigned char* p)
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8)
           | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint16_t readU16(const unsigned char* p)
{
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

void writeU32(std::ofstream& out, std::uint32_t v)
{
    const unsigned char b[4] = { static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8),
                                 static_cast<unsigned char>(v >> 16), static_cast<unsigned char>(v >> 24) };
    out.write(reinterpret_cast<const char*>(b), 4);
}

void writeU16(std::ofstream& out, std::uint16_t v)
{
    const unsigned char b[2] = { static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8) };
    out.write(reinterpret_cast<const char*>(b), 2);
}

float decodeSample(const unsigned char* p, std::uint16_t format, int bits)
{
    if (format == kFormatFloat)
    {
        if (bits == 32)
        {
            float f;
            std::uint32_t u = readU32(p);
            std::memcpy(&f, &u, 4);
            return f;
        }
        double d;
        std::uint64_t u = static_cast<std::uint64_t>(readU32(p)) | (static_cast<std::uint64_t>(readU32(p + 4)) << 32);
        std::memcpy(&d, &u, 8);
        return static_cast<float>(d);
    }

    switch (bits)
    {
        case 8: return (static_cast<int>(p[0]) - 128) / 128.0f;
        case 16: return static_cast<std::int16_t>(readU16(p)) / 32768.0f;
        case 24:
        {
            std::int32_t v = static_cast<std::int32_t>(p[0] | (p[1] << 8) | (p[2] << 16));
            if (v & 0x800000)
                v -= 0x1000000;
            return static_cast<float>(v / 8388608.0);
        }
        case 32: return static_cast<float>(static_cast<std::int32_t>(readU32(p)) / 2147483648.0);
        default: return 0.0f;
    }
}
} // namespace

AudioData readWav(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot open " + path);

    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
        throw std::runtime_error(path + " is not a RIFF/WAVE file");

    std::uint16_t format = 0, numChannels = 0, bits = 0;
    std::uint32_t sampleRate = 0;
    const unsigned char* data = nullptr;
    std::uint32_t dataSize = 0;

    size_t pos = 12;
    while (pos + 8 <= bytes.size())
    {
        const unsigned char* chunk = bytes.data() + pos;
        std::uint32_t size = readU32(chunk + 4);
        const size_t bodyStart = pos + 8;
        const size_t available = bytes.size() - bodyStart;
        if (size > available)
            size = static_cast<std::uint32_t>(available); // tolerate truncated files

        if (std::memcmp(chunk, "fmt ", 4) == 0 && size >= 16)
        {
            const unsigned char* f = chunk + 8;
            format = readU16(f);
            numChannels = readU16(f + 2);
            sampleRate = readU32(f + 4);
            bits = readU16(f + 14);
            if (format == kFormatExtensible && size >= 26)
                format = readU16(f + 24); // first two bytes of the sub-format GUID
        }
        else if (std::memcmp(chunk, "data", 4) == 0)
        {
            data = chunk + 8;
            dataSize = size;
        }
        pos = bodyStart + size + (size & 1u);
    }

    if (data == nullptr || numChannels == 0 || sampleRate == 0)
        throw std::runtime_error(path + ": missing fmt or data chunk");
    if (format != kFormatPcm && format != kFormatFloat)
        throw std::runtime_error(path + ": unsupported WAV format " + std::to_string(format));
    if (bits % 8 != 0 || bits == 0)
        throw std::runtime_error(path + ": unsupported bit depth " + std::to_string(bits));

    const int bytesPerSample = bits / 8;
    const size_t frameSize = static_cast<size_t>(bytesPerSample) * numChannels;
    const size_t frames = dataSize / frameSize;

    AudioData audio;
    audio.sampleRate = sampleRate;
    audio.channels.assign(numChannels, std::vector<float>(frames));
    for (size_t i = 0; i < frames; ++i)
        for (int c = 0; c < numChannels; ++c)
            audio.channels[static_cast<size_t>(c)][i] = decodeSample(data + i * frameSize + static_cast<size_t>(c) * bytesPerSample, format, bits);

    return audio;
}

void writeWav(const std::string& path, const AudioData& audio)
{
    std::ofstream out(path, std::ios::binary);
    if (!out)
        throw std::runtime_error("cannot write " + path);

    const auto numChannels = static_cast<std::uint16_t>(audio.numChannels());
    const auto frames = static_cast<std::uint32_t>(audio.numSamples());
    const std::uint32_t dataSize = frames * numChannels * 4u;

    out.write("RIFF", 4);
    writeU32(out, 36 + dataSize);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    writeU32(out, 16);
    writeU16(out, kFormatFloat);
    writeU16(out, numChannels);
    writeU32(out, static_cast<std::uint32_t>(audio.sampleRate));
    writeU32(out, static_cast<std::uint32_t>(audio.sampleRate) * numChannels * 4u);
    writeU16(out, static_cast<std::uint16_t>(numChannels * 4));
    writeU16(out, 32);
    out.write("data", 4);
    writeU32(out, dataSize);

    for (std::uint32_t i = 0; i < frames; ++i)
    {
        for (std::uint16_t c = 0; c < numChannels; ++c)
        {
            std::uint32_t u;
            std::memcpy(&u, &audio.channels[c][i], 4);
            writeU32(out, u);
        }
    }
}

} // namespace ma
