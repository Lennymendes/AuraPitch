# AuraPitch — NextDSP · AuraSuite
# Correction de hauteur / Pitch & Formant Shifter · VST3 + AU (+ Standalone) · 44,1 kHz … DXD 384 kHz, 32/64 bits.
#
#   cmake -B build -DCMAKE_BUILD_TYPE=Release        # JUCE 8.0.15 téléchargé automatiquement
#   cmake --build build --config Release
#   ctest --test-dir build -C Release --output-on-failure
#
# Options
#   AURAPITCH_JUCE_DIR=/chemin/JUCE   JUCE local au lieu du téléchargement
#   AURAPITCH_BUILD_PLUGIN=OFF        noyau DSP + banc seulement (aucune dépendance, pas de JUCE)
cmake_minimum_required (VERSION 3.22)

project (AuraPitch VERSION 0.1.0 LANGUAGES C CXX)

set (CMAKE_CXX_STANDARD 20)
set (CMAKE_CXX_STANDARD_REQUIRED ON)
set (CMAKE_CXX_EXTENSIONS OFF)
set (CMAKE_POSITION_INDEPENDENT_CODE ON)

if (APPLE)
    set (CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "macOS minimum")
    if (NOT CMAKE_OSX_ARCHITECTURES)
        set (CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "binaire universel" FORCE)
    endif()
endif()

option (AURAPITCH_BUILD_PLUGIN "Construire le plug-in JUCE (VST3 / AU / Standalone)" ON)
option (AURAPITCH_BUILD_TESTS  "Construire le banc d'essai DSP" ON)
set (AURAPITCH_JUCE_DIR "" CACHE PATH "Dossier JUCE local (vide : téléchargement de JUCE 8.0.15)")

#--- Noyau DSP : en-têtes C++20 sans JUCE --------------------------------------------------------------------------
add_library (aurapitch_dsp INTERFACE)
target_include_directories (aurapitch_dsp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/Source/DSP)
target_compile_features (aurapitch_dsp INTERFACE cxx_std_20)

if (MSVC)
    set (AURAPITCH_WARNINGS /W4 /permissive-)
else()
    set (AURAPITCH_WARNINGS -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion)
endif()

#--- Banc d'essai DSP ------------------------------------------------------------------------------------------------
if (AURAPITCH_BUILD_TESTS)
    enable_testing()
    find_package (Threads REQUIRED)
    add_executable (aurapitch_dsp_test Tests/PitchDspSelfTest.cpp)
    target_link_libraries (aurapitch_dsp_test PRIVATE aurapitch_dsp Threads::Threads)
    target_compile_options (aurapitch_dsp_test PRIVATE ${AURAPITCH_WARNINGS})
    add_test (NAME aurapitch_dsp_test COMMAND aurapitch_dsp_test)
endif()

#--- Plug-in ---------------------------------------------------------------------------------------------------------
set (AURAPITCH_PLUGIN_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginProcessor.h
    Source/Parameters.cpp
    Source/Parameters.h
    Source/DSP/PSOLAShifter.h
    Source/DSP/PhaseVocoderShifter.h
    Source/DSP/FormantProcessor.h
    Source/DSP/LatencyCompensatedDry.h
    Source/GUI/PluginEditor.cpp
    Source/GUI/PluginEditor.h
    Source/GUI/NextDspLookAndFeel.cpp
    Source/GUI/NextDspLookAndFeel.h
    Source/GUI/PitchGraph.cpp
    Source/GUI/PitchGraph.h
    Source/GUI/ScaleKeyboard.cpp
    Source/GUI/ScaleKeyboard.h
    Source/GUI/CentsMeter.cpp
    Source/GUI/CentsMeter.h)

# Les sources du plug-in arrivent aux étapes 2 et 3 : tant qu'elles manquent, seul le noyau DSP est construit.
if (AURAPITCH_BUILD_PLUGIN AND NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/Source/PluginProcessor.cpp)
    message (STATUS "AuraPitch : sources du plug-in absentes (étapes 2-3), construction du noyau DSP seul.")
    set (AURAPITCH_BUILD_PLUGIN OFF)
endif()

if (AURAPITCH_BUILD_PLUGIN)
    if (AURAPITCH_JUCE_DIR)
        add_subdirectory (${AURAPITCH_JUCE_DIR} ${CMAKE_BINARY_DIR}/JUCE)
    else()
        include (FetchContent)
        FetchContent_Declare (JUCE
            GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
            GIT_TAG        8.0.15
            GIT_SHALLOW    ON)
        FetchContent_MakeAvailable (JUCE)
    endif()

    set (AURAPITCH_FORMATS VST3 Standalone)
    if (APPLE)
        list (APPEND AURAPITCH_FORMATS AU)
    endif()

    juce_add_plugin (AuraPitch
        PRODUCT_NAME                "AuraPitch"
        COMPANY_NAME                "NextDSP"
        COMPANY_WEBSITE             "https://nextdsp.com"
        BUNDLE_ID                   "com.nextdsp.aurapitch"
        PLUGIN_MANUFACTURER_CODE    Nxds
        PLUGIN_CODE                 Aupt
        FORMATS                     ${AURAPITCH_FORMATS}
        VST3_CATEGORIES             Fx Pitch Shift
        AU_MAIN_TYPE                kAudioUnitType_Effect
        IS_SYNTH                    FALSE
        NEEDS_MIDI_INPUT            FALSE
        NEEDS_MIDI_OUTPUT           FALSE
        IS_MIDI_EFFECT              FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        COPY_PLUGIN_AFTER_BUILD     FALSE)

    target_sources (AuraPitch PRIVATE ${AURAPITCH_PLUGIN_SOURCES})
    target_include_directories (AuraPitch PRIVATE Source Source/DSP Source/GUI)

    target_compile_definitions (AuraPitch PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        JUCE_REPORT_APP_USAGE=0)

    target_link_libraries (AuraPitch
        PRIVATE
            aurapitch_dsp
            juce::juce_audio_utils
            juce::juce_dsp
            juce::juce_opengl
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)
endif()
