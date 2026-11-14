# Master check: one instance at the very end of the master channel, after the mastering chain (Ozone).
juce_add_plugin(MixAssistantMaster
    COMPANY_NAME "${MA_COMPANY_NAME}"
    PLUGIN_MANUFACTURER_CODE ${MA_MANUFACTURER_CODE}
    PLUGIN_CODE MaMc
    PRODUCT_NAME "Mix Assistant Master Check"
    FORMATS VST3 Standalone
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx Analyzer)

target_sources(MixAssistantMaster PRIVATE
    src/MasterProcessor.cpp
    src/MasterEditor.cpp)

ma_configure_plugin(MixAssistantMaster)
