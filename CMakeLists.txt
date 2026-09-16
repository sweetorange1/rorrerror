cmake_minimum_required(VERSION 3.22 FATAL_ERROR)

project(GridButtonsPluginProject VERSION 1.2.0 LANGUAGES C CXX)

# =========================
# Toolchain / 编译选项（必要最小集）
# =========================
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC：静态运行库 + UTF-8（便于中文注释）
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options("/utf-8")
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

# Windows：把VST3复制目录设置到无需管理员权限的位置（可选但常用）
if(WIN32)
  set_directory_properties(PROPERTIES JUCE_VST3_COPY_DIR "$ENV{LOCALAPPDATA}/Programs/Common/VST3")
endif()

# =========================
# JUCE（使用CMake自带FetchContent拉取，避免依赖旧项目的cmake/cpm.cmake等脚本）
# =========================
include(FetchContent)

FetchContent_Declare(
  juce
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 8.0.12
)

FetchContent_MakeAvailable(juce)

# =========================
# Plugin: GridButtons
# =========================
set(GRID_BUTTONS_PLUGIN_NAME "rorrerror")

juce_add_plugin(
  GridButtonsPlugin

  PRODUCT_NAME
    ${GRID_BUTTONS_PLUGIN_NAME}
  VERSION
    1.2.0
  COMPANY_NAME
    "iisaacbeats.cn"

  PLUGIN_MANUFACTURER_CODE
    Wfsd
  PLUGIN_CODE
    Grdb

  FORMATS
    VST3
    Standalone

  IS_SYNTH
    FALSE
  NEEDS_MIDI_INPUT
    FALSE
  NEEDS_MIDI_OUTPUT
    FALSE

  VST3_AUTO_MANIFEST
    FALSE
  COPY_PLUGIN_AFTER_BUILD
    TRUE
)

file(GLOB GRID_GLITCH_FRAMES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/assets/glitched_frames/*.png"
)

juce_add_binary_data(
  GridButtonsPlugin_BinaryData
  SOURCES
    assets/background.png
    assets/background_pnglist.png
    assets/png/1_1.png
    assets/png/1_2.png
    assets/png/1_3.png
    assets/png/1_4.png
    assets/png/2_1.png
    assets/png/2_2.png
    assets/png/2_3.png
    assets/png/2_4.png
    assets/png/2_5.png
    assets/png/2_6.png
    assets/png/2_7.png
    assets/png/2_8.png
    assets/png/2_9.png
    assets/png/2_10.png
    assets/png/2_11.png
    assets/png/2_12.png
    assets/png/2_13.png
    assets/png/2_14.png
    assets/png/2_15.png
    assets/png/3_1.png
    assets/png/3_2.png
    assets/png/3_3.png
    assets/png/3_4.png
    assets/png/3_5.png
    assets/png/3_6.png
    assets/png/3_7.png
    assets/png/3_8.png
    assets/png/3_9.png
    assets/png/3_10.png
    assets/rand.png
    assets/about.png
    ${GRID_GLITCH_FRAMES}
)

# 常规瘦身：关闭不需要的JUCE功能
target_compile_definitions(GridButtonsPlugin
  PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=1
    JUCE_VST3_CAN_REPLACE_VST2=0
)

# 你的自定义模块（包含Processor/Editor）
juce_add_module(grid_plugin ALIAS_NAMESPACE grid)

target_compile_definitions(grid_plugin
  INTERFACE
    GRID_PLUGIN_NAME="${GRID_BUTTONS_PLUGIN_NAME}"
)

target_link_libraries(grid_plugin
  INTERFACE
    GridButtonsPlugin_BinaryData
)

target_link_libraries(
  GridButtonsPlugin
  PRIVATE
    GridButtonsPlugin_BinaryData
    grid::grid_plugin
    juce::juce_audio_devices
    juce::juce_audio_utils
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)

# Debug时避免断言弹窗/中断（可按需删掉）
target_compile_definitions(GridButtonsPlugin PRIVATE
  $<$<CONFIG:Debug>:JUCE_DISABLE_ASSERTIONS=1>
  $<$<CONFIG:Debug>:JUCE_DISABLE_CAUTIOUS_PARAMETER_ID_CHECKING=1>
)

# Visual Studio: source grouping
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR})