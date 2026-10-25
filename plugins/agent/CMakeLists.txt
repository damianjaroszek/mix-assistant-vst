# Agent: one instance at the end of every mixer channel.
juce_add_plugin(MixAssistantAgent
    COMPANY_NAME "${MA_COMPANY_NAME}"
    PLUGIN_MANUFACTURER_CODE ${MA_MANUFACTURER_CODE}
    PLUGIN_CODE MaAg
    PRODUCT_NAME "Mix Assistant Agent"
    FORMATS VST3 Standalone
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx Analyzer)

target_sources(MixAssistantAgent PRIVATE
    src/AgentProcessor.cpp
    src/AgentEditor.cpp)

ma_configure_plugin(MixAssistantAgent)
