# AuraKey — NextDSP · AuraSuite
# Analyseur de tonalité VST3 : flux live (clé, cercle des quintes) + import de fichiers (clé, tempo, sections).
# Inclus par le CMakeLists.txt racine avec include() (JUCE, options et bancs d'essai y sont configurés) : les produits
# sortent à côté de ceux d'AuraPitch, dans build/AuraKey_artefacts/.
#
# Partagé avec AuraPitch : design system NextDSP (Source/GUI/NextDSPLookAndFeel, Controls), FFT réelle et Auto-Key
# (Source/DSP/FFT.h, AutoKeyDetector.h).

option (AURAKEY_MP3_DECODER "Décodeur MP3 logiciel de JUCE (JUCE_USE_MP3AUDIOFORMAT : lire l'avertissement de licence dans juce_audio_formats.h)" ON)

set (AURAKEY_DIR ${CMAKE_CURRENT_LIST_DIR})
set (AURASUITE_DIR ${CMAKE_CURRENT_LIST_DIR}/..)

# Noyau DSP : en-têtes C++20 sans JUCE
add_library (aurakey_dsp INTERFACE)
target_include_directories (aurakey_dsp INTERFACE ${AURAKEY_DIR}/Source/DSP ${AURASUITE_DIR}/Source/DSP)
target_compile_features (aurakey_dsp INTERFACE cxx_std_20)

if (AURAPITCH_BUILD_PLUGIN)
    juce_add_plugin (AuraKey
        PRODUCT_NAME                "AuraKey"
        VERSION                     ${PROJECT_VERSION}
        COMPANY_NAME                "NextDSP"
        COMPANY_WEBSITE             "https://nextdsp.com"
        COMPANY_EMAIL               "support@nextdsp.com"
        BUNDLE_ID                   "com.nextdsp.aurakey"
        DESCRIPTION                 "Key, tempo and section analyser, live stream and audio files, up to DXD 384 kHz"
        PLUGIN_MANUFACTURER_CODE    Nxds
        PLUGIN_CODE                 Akey
        FORMATS                     VST3                            # VST3 seulement
        VST3_CATEGORIES             Fx Analyzer
        IS_SYNTH                    FALSE
        NEEDS_MIDI_INPUT            FALSE
        NEEDS_MIDI_OUTPUT           FALSE
        IS_MIDI_EFFECT              FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        HARDENED_RUNTIME_ENABLED    TRUE
        COPY_PLUGIN_AFTER_BUILD     ${AURAPITCH_COPY_PLUGIN})

    target_sources (AuraKey PRIVATE
        ${AURAKEY_DIR}/Source/PluginProcessor.cpp
        ${AURAKEY_DIR}/Source/GUI/AuraKeyEditor.cpp
        ${AURAKEY_DIR}/Source/GUI/CircleOfFifths.cpp
        ${AURAKEY_DIR}/Source/GUI/SectionMap.cpp
        ${AURASUITE_DIR}/Source/GUI/NextDSPLookAndFeel.cpp
        ${AURASUITE_DIR}/Source/GUI/Controls.cpp)

    target_include_directories (AuraKey PRIVATE ${AURAKEY_DIR}/Source)

    target_compile_definitions (AuraKey PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        JUCE_REPORT_APP_USAGE=0
        JUCE_MODAL_LOOPS_PERMITTED=0
        JUCE_USE_FLAC=1
        JUCE_USE_OGGVORBIS=1
        JUCE_USE_MP3AUDIOFORMAT=$<BOOL:${AURAKEY_MP3_DECODER}>)

    target_link_libraries (AuraKey
        PRIVATE
            aurakey_dsp
            juce::juce_audio_utils
            juce::juce_audio_formats
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)
endif()

if (AURAPITCH_BUILD_TESTS)
    # Avec le plug-in, le banc teste aussi le processeur (fichiers, état, interface) ; sans, le noyau DSP seul (version
    # compilée sous ASan / UBSan / TSan).
    add_executable (aurakey_test ${AURAKEY_DIR}/Tests/AuraKeyTests.cpp)
    if (AURAPITCH_BUILD_PLUGIN)
        target_link_libraries (aurakey_test PRIVATE AuraKey aurakey_dsp Threads::Threads)
        target_include_directories (aurakey_test PRIVATE ${AURAKEY_DIR}/Source $<TARGET_PROPERTY:AuraKey,INCLUDE_DIRECTORIES>)
        target_compile_definitions (aurakey_test PRIVATE AURAKEY_PLUGIN=1 $<TARGET_PROPERTY:AuraKey,COMPILE_DEFINITIONS>)
        target_compile_features (aurakey_test PRIVATE cxx_std_20)
    else()
        target_link_libraries (aurakey_test PRIVATE aurakey_dsp Threads::Threads)
        target_compile_options (aurakey_test PRIVATE ${AURAPITCH_WARNINGS} $<$<CXX_COMPILER_ID:GNU>:-Wno-mismatched-new-delete>)
    endif()
    add_test (NAME aurakey_test COMMAND aurakey_test)
    add_dependencies (AuraPitchTests aurakey_test)
endif()
