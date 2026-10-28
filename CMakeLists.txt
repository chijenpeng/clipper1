cmake_minimum_required(VERSION 3.22)

project(Clipper1 VERSION 0.1.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# The product name appears here, in the editor title (Source/PluginEditor.cpp: kPluginName) and in README.md.
set(CLIPPER_NAME "Clipper1")
option(COPY_PLUGIN_AFTER_BUILD "Copy AU and VST3 bundles after building the plug-in target" OFF)

if(MSVC)
    set(CLIPPER_DSP_OPTIONS /W4)
    set(CLIPPER_TEST_OPTIONS /W4)
else()
    set(CLIPPER_DSP_OPTIONS -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    set(CLIPPER_TEST_OPTIONS -Wall -Wextra)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Prefer a local JUCE checkout for day-to-day development. A fresh clone remains buildable:
# when third_party/JUCE is absent CMake fetches the exact release used by this project.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/JUCE/CMakeLists.txt")
    add_subdirectory(third_party/JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.9
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# The clipper has no UI dependency; it links only JUCE's DSP module (for the oversampler),
# so it and its tests build and run without a host or a screen.
add_library(clipper1_dsp STATIC Source/dsp/Clipper.cpp)
target_include_directories(clipper1_dsp PUBLIC Source)
target_compile_options(clipper1_dsp PRIVATE ${CLIPPER_DSP_OPTIONS})
target_link_libraries(clipper1_dsp PUBLIC juce::juce_dsp PRIVATE juce::juce_recommended_config_flags)

# Tests: prints PASS/FAIL per check, non-zero exit on any failure.
add_executable(clipper1_test tests/clipper1_test.cpp)
target_link_libraries(clipper1_test PRIVATE clipper1_dsp juce::juce_recommended_config_flags)
target_compile_options(clipper1_test PRIVATE ${CLIPPER_TEST_OPTIONS})
enable_testing()
add_test(NAME clipper1_test COMMAND clipper1_test)

juce_add_plugin(Clipper1
    PRODUCT_NAME "${CLIPPER_NAME}"
    COMPANY_NAME "codycodie"
    BUNDLE_ID "com.c0dypeng.clipper1"
    PLUGIN_MANUFACTURER_CODE Cdpg
    PLUGIN_CODE Clp1
    FORMATS AU VST3
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    VST3_CATEGORIES Fx Distortion Mastering
    AU_MAIN_TYPE kAudioUnitType_Effect
    COPY_PLUGIN_AFTER_BUILD ${COPY_PLUGIN_AFTER_BUILD})

# Inter and JetBrains Mono, both SIL Open Font License (licences are next to the fonts).
juce_add_binary_data(Clipper1Assets SOURCES
    Resources/fonts/Inter-SemiBold.ttf
    Resources/fonts/JetBrainsMono-Medium.ttf)
set_target_properties(Clipper1Assets PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_sources(Clipper1 PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(Clipper1 PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(Clipper1
    PRIVATE
        clipper1_dsp
        Clipper1Assets
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Renders the editor to a PNG with no host or screen (see tests/snapshot.cpp).
juce_add_console_app(clipper1_snapshot PRODUCT_NAME "clipper1_snapshot")
target_sources(clipper1_snapshot PRIVATE
    tests/snapshot.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)
target_include_directories(clipper1_snapshot PRIVATE Source)
target_compile_definitions(clipper1_snapshot PRIVATE
    JucePlugin_Name="${CLIPPER_NAME}"
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)
target_link_libraries(clipper1_snapshot PRIVATE
    clipper1_dsp
    Clipper1Assets
    juce::juce_audio_utils
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
