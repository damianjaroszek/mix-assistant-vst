# mixcheck: the plugins' own Agents and Brain run offline on real multitrack stems, the way they run
# in FL (learn on the drop, analyse, apply, refine), then the processed mix is rendered and compared
# with a reference mix. Off by default (it compiles both plugins once more).
juce_add_console_app(MixCheck PRODUCT_NAME "mixcheck")

target_sources(MixCheck PRIVATE
    main.cpp
    ${CMAKE_SOURCE_DIR}/plugins/agent/src/AgentProcessor.cpp
    ${CMAKE_SOURCE_DIR}/plugins/agent/src/AgentEditor.cpp
    ${CMAKE_SOURCE_DIR}/plugins/brain/src/BrainProcessor.cpp
    ${CMAKE_SOURCE_DIR}/plugins/brain/src/BrainEditor.cpp
    ${CMAKE_SOURCE_DIR}/plugins/master/src/MasterProcessor.cpp
    ${CMAKE_SOURCE_DIR}/plugins/master/src/MasterEditor.cpp)

target_include_directories(MixCheck PRIVATE
    ${CMAKE_SOURCE_DIR}/plugins/common
    ${CMAKE_SOURCE_DIR}/plugins/agent/src
    ${CMAKE_SOURCE_DIR}/plugins/brain/src
    ${CMAKE_SOURCE_DIR}/plugins/master/src)

target_compile_definitions(MixCheck PRIVATE
    JucePlugin_Name="mixcheck"
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1)

target_link_libraries(MixCheck PRIVATE
    ma_core
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_dsp
    juce::juce_recommended_config_flags)

# Both plugins define the plugin factory; here neither is a plugin, so the Agent's gets another name.
set_source_files_properties(${CMAKE_SOURCE_DIR}/plugins/agent/src/AgentProcessor.cpp
    PROPERTIES COMPILE_DEFINITIONS createPluginFilter=createAgentFilter)
set_source_files_properties(${CMAKE_SOURCE_DIR}/plugins/master/src/MasterProcessor.cpp
    PROPERTIES COMPILE_DEFINITIONS createPluginFilter=createMasterFilter)
