cmake_minimum_required(VERSION 3.22)
project(OROGEN VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Флаги оптимизации — только для GCC/Clang. У MSVC (-O2 не понимает) берём
# штатные /O2 /Zi из конфигураций Visual Studio: перезапись через FORCE их ломала.
if(NOT MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "" FORCE)
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g0 -DNDEBUG" CACHE STRING "" FORCE)
endif()
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# JUCE
#   По умолчанию — FetchContent (нужен git и сеть).
#   Если JUCE уже есть локально:  cmake -DOROGEN_JUCE_PATH=/path/to/JUCE ..
# ---------------------------------------------------------------------------
set(OROGEN_JUCE_VERSION 8.0.15 CACHE STRING "JUCE tag to fetch")

if(DEFINED OROGEN_JUCE_PATH AND EXISTS "${OROGEN_JUCE_PATH}/modules")
    message(STATUS "OROGEN: using local JUCE at ${OROGEN_JUCE_PATH}")
    add_subdirectory("${OROGEN_JUCE_PATH}" JUCE EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG ${OROGEN_JUCE_VERSION}
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# ---------------------------------------------------------------------------
# DSP-ядро (orogen-dsp) — header-only, без JUCE
# ---------------------------------------------------------------------------
set(OROGEN_DSP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../orogen-dsp/src" CACHE PATH "Путь к src/ orogen-dsp")

if(NOT EXISTS "${OROGEN_DSP_DIR}/instrument/instrument.h")
    message(FATAL_ERROR "Не найден orogen-dsp: ${OROGEN_DSP_DIR} (задайте -DOROGEN_DSP_DIR=...)")
endif()

# Часть эффектов MonoJuice — не header-only: два .cpp из third_party.
set(OROGEN_MONOJUICE_SOURCES
    "${OROGEN_DSP_DIR}/../third_party/monojuice/dsp/ChorusCore.cpp"
    "${OROGEN_DSP_DIR}/../third_party/monojuice/dsp/AudioAdapter.cpp")
# Сторонний код (GPL-часть MonoJuice) копируется без правок — глушим его
# предупреждения локально: -w у GCC/Clang, /w у MSVC.
if(MSVC)
    set(OROGEN_THIRD_PARTY_WARN_FLAGS "/w")
else()
    set(OROGEN_THIRD_PARTY_WARN_FLAGS "-w")
endif()
set_source_files_properties(${OROGEN_MONOJUICE_SOURCES}
    PROPERTIES COMPILE_OPTIONS "${OROGEN_THIRD_PARTY_WARN_FLAGS}")

# Общие настройки JUCE: минимум зависимостей, без сети и браузера.
set(OROGEN_JUCE_DEFS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_MP3AUDIOFORMAT=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_ALSA=0            # в песочнице нет libasound; для Standalone включите 1
    JUCE_JACK=0
    JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1)

# ---------------------------------------------------------------------------
# Плагин
# ---------------------------------------------------------------------------
# VST3-цель требует juceaide (он собирается на этапе configure, и на слабых
# машинах это самая долгая часть). Для проверок и офлайн-рендера он не нужен:
#   cmake -DOROGEN_BUILD_VST3=OFF ..
option(OROGEN_BUILD_VST3 "Собирать VST3 (нужен juceaide)" ON)

if(OROGEN_BUILD_VST3)

juce_add_plugin(OROGEN
    COMPANY_NAME "OROGEN"
    BUNDLE_ID    "com.orogen.orogen"
    IS_SYNTH     TRUE
    NEEDS_MIDI_INPUT   TRUE
    NEEDS_MIDI_OUTPUT  FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE
    PLUGIN_MANUFACTURER_CODE Orog
    PLUGIN_CODE Orgn
    PRODUCT_NAME "OROGEN"
    VERSION ${PROJECT_VERSION}
    FORMATS VST3)

target_sources(OROGEN PRIVATE
    src/plugin/PluginProcessor.cpp
    src/plugin/PluginEditor.cpp
    src/plugin/OrLookAndFeel.cpp
    ${OROGEN_MONOJUICE_SOURCES})

target_compile_definitions(OROGEN PUBLIC ${OROGEN_JUCE_DEFS})
target_include_directories(OROGEN PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}")
# DSP-ядро — header-only и живёт в другом каталоге: подключаем как СИСТЕМНЫЕ
# заголовки, иначе juce_recommended_warning_flags (-Wsign-conversion,
# -Wfloat-equal, -Wshadow…) выдают ~250 предупреждений из чужого кода.
target_include_directories(OROGEN SYSTEM PRIVATE "${OROGEN_DSP_DIR}")
target_link_libraries(OROGEN PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_gui_basics
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
# LTO осознанно выключен: на 2-ядерной сборке он добавляет минуты и гигабайты
# памяти ради единиц процентов. Включается флагом -DOROGEN_LTO=ON.
option(OROGEN_LTO "Включить link-time optimization" OFF)
if(OROGEN_LTO)
    target_link_libraries(OROGEN PRIVATE juce::juce_recommended_lto_flags)
endif()

else()
    message(STATUS "OROGEN: VST3 отключён — собираются только стенды")
endif()

# ---------------------------------------------------------------------------
# Стенды: офлайн-рендер и тесты.
#
# Здесь НЕ используются juce_add_console_app/juce_add_gui_app: они тянут
# juceaide, который JUCE собирает на этапе configure (дольше любой другой части
# сборки). Для консольной цели juceaide не нужен — достаточно слинковать модули
# и объявить определения самому.
# ---------------------------------------------------------------------------
function(orogen_console_app target)
    add_executable(${target} ${ARGN})
    target_compile_definitions(${target} PRIVATE
        ${OROGEN_JUCE_DEFS}
        JUCE_STANDALONE_APPLICATION=1
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_MODULE_AVAILABLE_juce_core=1
        JUCE_MODULE_AVAILABLE_juce_events=1
        JUCE_MODULE_AVAILABLE_juce_data_structures=1
        JUCE_MODULE_AVAILABLE_juce_graphics=1
        JUCE_MODULE_AVAILABLE_juce_gui_basics=1
        JUCE_MODULE_AVAILABLE_juce_audio_basics=1
        JUCE_MODULE_AVAILABLE_juce_audio_devices=1
        JUCE_MODULE_AVAILABLE_juce_audio_formats=1
        JUCE_MODULE_AVAILABLE_juce_audio_processors=1
        JUCE_MODULE_AVAILABLE_juce_audio_utils=1)
    target_include_directories(${target} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}")
    target_include_directories(${target} SYSTEM PRIVATE "${OROGEN_DSP_DIR}")
    target_link_libraries(${target} PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_audio_formats
        juce::juce_gui_basics
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
endfunction()

option(OROGEN_BUILD_STANDS "Собирать консольные стенды (рендер/тесты/скриншоты)" ON)

if(OROGEN_BUILD_STANDS)

orogen_console_app(orogen_render tools/render_plugin.cpp
                                 src/plugin/PluginProcessor.cpp
                                 src/plugin/PluginEditor.cpp
                                 src/plugin/OrLookAndFeel.cpp
                                 ${OROGEN_MONOJUICE_SOURCES})

orogen_console_app(render_ui tools/render_ui.cpp
                             src/plugin/PluginProcessor.cpp
                             src/plugin/PluginEditor.cpp
                             src/plugin/OrLookAndFeel.cpp
                             ${OROGEN_MONOJUICE_SOURCES})

orogen_console_app(test_plugin tests/test_plugin.cpp
                              src/plugin/PluginProcessor.cpp
                              src/plugin/PluginEditor.cpp
                              src/plugin/OrLookAndFeel.cpp
                              ${OROGEN_MONOJUICE_SOURCES})

# Проверка собранного .vst3 «снаружи»: грузим бандл так, как это делает хост.
# Цель не зависит от OROGEN_VST3 (нужен только файл), поэтому её можно собрать
# и запустить на машине, где плагин уже собран: build/verify_vst3 <OROGEN.vst3>
orogen_console_app(verify_vst3 tools/verify_vst3.cpp)
# хостинг VST3 внутри стенда: без этого заголовка VST3PluginFormat не существует
target_compile_definitions(verify_vst3 PRIVATE JUCE_PLUGINHOST_VST3=1)

endif()   # OROGEN_BUILD_STANDS
