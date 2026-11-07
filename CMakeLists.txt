# AuraPitch — NextDSP · AuraSuite
# Correction de hauteur / Pitch & Formant Shifter · VST3 + AU + Standalone · 44,1 kHz … DXD 384 kHz, 32/64 bits.
#
#   cmake -B build -DCMAKE_BUILD_TYPE=Release        # JUCE 8.0.15 téléchargé automatiquement (GitHub)
#   cmake --build build --config Release
#   ctest --test-dir build -C Release --output-on-failure
#
# Options
#   JUCE_DIR=/chemin/JUCE              JUCE local au lieu du téléchargement (alias : AURAPITCH_JUCE_DIR)
#   AURAPITCH_BUILD_PLUGIN=OFF         noyau DSP + bancs seulement (aucune dépendance, pas de JUCE)
#   AURAPITCH_COPY_PLUGIN=ON           copie le .vst3 / .component dans les dossiers système après la compilation
#
# Produits (macOS) : build/AuraPitch_artefacts/Release/{VST3/AuraPitch.vst3, AU/AuraPitch.component, Standalone/AuraPitch.app}
cmake_minimum_required (VERSION 3.22)

project (AuraPitch VERSION 1.0.0 LANGUAGES C CXX)

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

if (MSVC)
    add_compile_options (/utf-8 /MP)                     # sources UTF-8 (commentaires en français), compilation parallèle
    set (CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # plug-in sans dépendance au runtime VC
endif()

option (AURAPITCH_BUILD_PLUGIN "Construire le plug-in JUCE (VST3 / AU / Standalone)" ON)
option (AURAPITCH_BUILD_TESTS  "Construire les bancs d'essai" ON)
option (AURAPITCH_COPY_PLUGIN  "Copier le plug-in dans les dossiers système après la compilation" OFF)
set (JUCE_DIR "" CACHE PATH "Dossier JUCE local (vide : téléchargement de JUCE 8.0.15)")
set (AURAPITCH_JUCE_DIR "" CACHE PATH "Alias de JUCE_DIR")
if (NOT JUCE_DIR AND AURAPITCH_JUCE_DIR)
    set (JUCE_DIR "${AURAPITCH_JUCE_DIR}")
endif()

#--- Noyau DSP : en-têtes C++20 sans JUCE --------------------------------------------------------------------------
add_library (aurapitch_dsp INTERFACE)
target_include_directories (aurapitch_dsp INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/Source/DSP)
target_compile_features (aurapitch_dsp INTERFACE cxx_std_20)

if (MSVC)
    set (AURAPITCH_WARNINGS /W4 /permissive-)
else()
    set (AURAPITCH_WARNINGS -Wall -Wextra -Wshadow)
endif()

#--- Plug-in ---------------------------------------------------------------------------------------------------------
if (AURAPITCH_BUILD_PLUGIN)
    if (JUCE_DIR)
        add_subdirectory (${JUCE_DIR} ${CMAKE_BINARY_DIR}/JUCE)
    else()
        include (FetchContent)
        set (FETCHCONTENT_QUIET OFF)
        FetchContent_Declare (JUCE
            GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
            GIT_TAG        8.0.15
            GIT_SHALLOW    ON
            GIT_PROGRESS   ON)
        FetchContent_MakeAvailable (JUCE)
    endif()

    set (AURAPITCH_FORMATS VST3 Standalone)
    if (APPLE)
        list (APPEND AURAPITCH_FORMATS AU)
    endif()

    juce_add_plugin (AuraPitch
        PRODUCT_NAME                "AuraPitch"
        VERSION                     ${PROJECT_VERSION}
        COMPANY_NAME                "NextDSP"
        COMPANY_WEBSITE             "https://nextdsp.com"
        COMPANY_EMAIL               "support@nextdsp.com"
        BUNDLE_ID                   "com.nextdsp.aurapitch"
        DESCRIPTION                 "Pitch correction & formant shifter, 44.1 kHz to DXD 384 kHz"
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
        HARDENED_RUNTIME_ENABLED    TRUE
        COPY_PLUGIN_AFTER_BUILD     ${AURAPITCH_COPY_PLUGIN})

    target_sources (AuraPitch PRIVATE
        Source/Parameters.cpp
        Source/PluginProcessor.cpp
        Source/GUI/NextDSPLookAndFeel.cpp
        Source/GUI/Controls.cpp
        Source/GUI/PitchDisplay.cpp
        Source/GUI/ScaleKeyboard.cpp
        Source/GUI/PluginEditor.cpp)

    target_include_directories (AuraPitch PRIVATE Source)

    target_compile_definitions (AuraPitch PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        JUCE_REPORT_APP_USAGE=0
        JUCE_MODAL_LOOPS_PERMITTED=0)

    target_link_libraries (AuraPitch
        PRIVATE
            aurapitch_dsp
            juce::juce_audio_utils
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)
endif()

#--- Bancs d'essai ---------------------------------------------------------------------------------------------------
if (AURAPITCH_BUILD_TESTS)
    enable_testing()
    find_package (Threads REQUIRED)
    add_custom_target (AuraPitchTests)

    # Étape 1 : détecteur, gamme, FIFO · Étape 2 : PSOLA, vocodeur Studio/DXD, formants, moteur complet
    foreach (bench aurapitch_dsp_test:PitchDspSelfTest aurapitch_engine_test:Step2SelfTest)
        string (REPLACE ":" ";" parts ${bench})
        list (GET parts 0 target)
        list (GET parts 1 source)
        add_executable (${target} Tests/${source}.cpp)
        target_link_libraries (${target} PRIVATE aurapitch_dsp Threads::Threads)
        target_compile_options (${target} PRIVATE ${AURAPITCH_WARNINGS}
            $<$<CXX_COMPILER_ID:GNU>:-Wno-mismatched-new-delete>)   # operator new remplacé par le compteur d'allocations
        add_test (NAME ${target} COMMAND ${target})
        add_dependencies (AuraPitchTests ${target})
    endforeach()

    # Étape 3 : le processeur JUCE (paramètres, presets, latence, 32/64 bits, double mono, bypass, état, éditeur)
    if (AURAPITCH_BUILD_PLUGIN)
        add_executable (aurapitch_plugin_test Tests/PluginSelfTest.cpp)
        # Le banc se lie au code partagé du plug-in (JUCE déjà compilé dedans) et reprend ses en-têtes et définitions.
        target_link_libraries (aurapitch_plugin_test PRIVATE AuraPitch aurapitch_dsp)
        target_include_directories (aurapitch_plugin_test PRIVATE Source $<TARGET_PROPERTY:AuraPitch,INCLUDE_DIRECTORIES>)
        target_compile_definitions (aurapitch_plugin_test PRIVATE $<TARGET_PROPERTY:AuraPitch,COMPILE_DEFINITIONS>)
        target_compile_features (aurapitch_plugin_test PRIVATE cxx_std_20)
        add_test (NAME aurapitch_plugin_test COMMAND aurapitch_plugin_test)
        add_dependencies (AuraPitchTests aurapitch_plugin_test)
    endif()
endif()
