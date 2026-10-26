# Brain: one instance at the end of the master chain.
juce_add_plugin(MixAssistantBrain
    COMPANY_NAME "${MA_COMPANY_NAME}"
    PLUGIN_MANUFACTURER_CODE ${MA_MANUFACTURER_CODE}
    PLUGIN_CODE MaBr
    PRODUCT_NAME "Mix Assistant Brain"
    FORMATS VST3 Standalone
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx Analyzer)

target_sources(MixAssistantBrain PRIVATE
    src/BrainProcessor.cpp
    src/BrainEditor.cpp)

ma_configure_plugin(MixAssistantBrain)

# Stem references from online separators often come as MP3.
target_compile_definitions(MixAssistantBrain PUBLIC JUCE_USE_MP3AUDIOFORMAT=1)
