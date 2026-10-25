cmake_minimum_required(VERSION 3.22)

project(MixAssistant VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(MA_BUILD_PLUGINS "Build the JUCE plugins (Agent, Brain)" ON)
option(MA_BUILD_LAB "Build the offline lab CLI" ON)
option(MA_BUILD_TESTS "Build the core unit tests" ON)

if(MSVC)
    # Source files are UTF-8 (Polish UI strings).
    add_compile_options(/utf-8)
endif()

add_subdirectory(core)

if(MA_BUILD_LAB)
    add_subdirectory(lab)
endif()

if(MA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(MA_BUILD_PLUGINS)
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
    add_subdirectory(plugins)
endif()
