cmake_minimum_required(VERSION 3.22)

set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")
set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build architectures")

project(ChordRes VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    # ソースは UTF-8 (日本語コメント入り)。指定しないと MSVC が CP932 として読み、C4819 や誤コンパイルになる
    add_compile_options(/utf-8)
endif()

# PluginLab の共通部品 (LabTest.h など)。隣に PluginLab があればそれを、無ければ同梱の lab/ を使う
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../PluginLab/juce/common/LabTest.h")
    set(LAB_COMMON "${CMAKE_CURRENT_SOURCE_DIR}/../PluginLab/juce/common")
else()
    set(LAB_COMMON "${CMAKE_CURRENT_SOURCE_DIR}/lab")
endif()

# ビルド後に VST3 フォルダへコピーするか。Windows のコピー先 (C:\Program Files\Common Files\VST3) は
# 管理者権限が要り、権限が無いとビルドが失敗するので既定 OFF
if(APPLE)
    set(_lab_install_default ON)
else()
    set(_lab_install_default OFF)
endif()
option(LAB_INSTALL "Copy the plugin to the system VST3 folder after build" ${_lab_install_default})

# 共有の JUCE (../_deps/JUCE) があればそれを使う。無ければ取得する
set(_juce_shared "${CMAKE_CURRENT_SOURCE_DIR}/../_deps/JUCE")
if(EXISTS "${_juce_shared}/modules/juce_core/juce_core.h")
    message(STATUS "Using shared JUCE at ${_juce_shared}")
    add_subdirectory("${_juce_shared}" JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.15
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

juce_add_plugin(ChordRes
    PRODUCT_NAME "ChordRes"
    COMPANY_NAME "MIDy"
    BUNDLE_ID "com.midy.chordres"
    PLUGIN_MANUFACTURER_CODE "MIDy"
    PLUGIN_CODE "ChRs"
    FORMATS VST3
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    VST3_CATEGORIES "Fx" "Filter"
    COPY_PLUGIN_AFTER_BUILD ${LAB_INSTALL})

target_sources(ChordRes PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(ChordRes PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(ChordRes
    PRIVATE juce::juce_audio_utils juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags)

# 検証用オフラインドライバ:
#   cmake -B build -DCHORDRES_BUILD_TESTS=ON && cmake --build build --target ChordResTest
option(CHORDRES_BUILD_TESTS "Build the offline test driver" OFF)
if(CHORDRES_BUILD_TESTS)
    juce_add_console_app(ChordResTest PRODUCT_NAME "ChordResTest")
    target_sources(ChordResTest PRIVATE tests/TestMain.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp)
    # PluginLab の検証ハーネス (耐久テスト lab.py robust)
    target_include_directories(ChordResTest PRIVATE "${LAB_COMMON}")
    target_compile_definitions(ChordResTest PUBLIC
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_STANDALONE_APPLICATION=1 JUCE_MODAL_LOOPS_PERMITTED=1
        JucePlugin_Name="ChordRes")
    target_link_libraries(ChordResTest
        PRIVATE juce::juce_audio_utils juce::juce_dsp
        PUBLIC juce::juce_recommended_config_flags)
endif()
