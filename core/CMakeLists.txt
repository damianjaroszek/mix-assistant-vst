# DSP core shared by the plugins and the offline lab. No JUCE dependency,
# so it can be built and tested anywhere.
add_library(ma_core STATIC
    src/LoudnessMeter.cpp
    src/TruePeakMeter.cpp
    src/SpectrumAnalyzer.cpp
    src/LevelStats.cpp
    src/EqualLoudness.cpp
    src/GainStaging.cpp
    src/ChannelAnalyzer.cpp)
target_include_directories(ma_core PUBLIC include)

# WAV reading/writing, used by the lab and tests only.
add_library(ma_io STATIC src/Wav.cpp)
target_include_directories(ma_io PUBLIC include)
