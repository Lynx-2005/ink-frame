cmake_minimum_required(VERSION 3.21)
project(InkFrame VERSION 0.1.0 LANGUAGES CXX)

# Production-grade defaults, Krita-inspired module split but 100% clean-room code.
# No Krita source is compiled or copied here. krita-reference/ is docs-only.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(INKFRAME_BUILD_TESTS "Build headless engine tests (no Qt required)" ON)
option(INKFRAME_BUILD_UI "Build Qt6 UI (canvas, timeline docks)" ON)
option(INKFRAME_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

if(INKFRAME_WARNINGS_AS_ERRORS)
  add_compile_options(-Werror)
endif()
add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)

# --- Engine: pure C++20, no Qt dependency (mirrors Krita libs/image+pigment split) ---
file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS
  src/core/*.cpp
  src/pigment/*.cpp
  src/brush/*.cpp
  src/animation/*.cpp
  src/io/*.cpp
  src/resources/*.cpp
)
add_library(inkframe_engine STATIC ${ENGINE_SOURCES})
target_include_directories(inkframe_engine PUBLIC src)
target_compile_features(inkframe_engine PUBLIC cxx_std_20)
find_package(Threads REQUIRED)
target_link_libraries(inkframe_engine PUBLIC Threads::Threads)

# --- UI: Qt6 only, optional so engine CI works without Qt (this machine has none) ---
if(INKFRAME_BUILD_UI)
  find_package(Qt6 QUIET COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets Svg)
  if(Qt6_FOUND)
    message(STATUS "Qt6 found: building InkFrame UI + app")
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTOUIC ON)
    set(CMAKE_AUTORCC ON)
    file(GLOB_RECURSE UI_SOURCES CONFIGURE_DEPENDS src/ui/*.cpp)
    add_library(inkframe_ui STATIC ${UI_SOURCES})
    target_link_libraries(inkframe_ui PUBLIC inkframe_engine
      Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets Qt6::Svg)
    target_include_directories(inkframe_ui PUBLIC src)
    add_executable(inkframe_app src/app/main.cpp)
    target_link_libraries(inkframe_app PRIVATE inkframe_ui)
  else()
    message(WARNING "Qt6 NOT found — skipping inkframe_ui/inkframe_app. Engine + tests still build. Install qt6-base-dev + libqt6opengl6-dev on your desktop to get canvas/timeline UI.")
  endif()
endif()

if(INKFRAME_BUILD_TESTS)
  enable_testing()
  file(GLOB TEST_SOURCES CONFIGURE_DEPENDS tests/test_*.cpp)
  foreach(t ${TEST_SOURCES})
    get_filename_component(tname ${t} NAME_WE)
    add_executable(${tname} ${t})
    target_link_libraries(${tname} PRIVATE inkframe_engine)
    # Never let NDEBUG silence assert() in tests (CMake sets it for
    # Release/RelWithDebInfo). Tests use INK_CHECK anyway; this is backup.
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(${tname} PRIVATE -UNDEBUG)
    endif()
    add_test(NAME ${tname} COMMAND ${tname})
  endforeach()

  # UI-level tests (tests/ui/*): these drive the tool layer, so they need Qt and
  # the offscreen platform plugin to run from a terminal.
  if(INKFRAME_BUILD_UI AND Qt6_FOUND)
    file(GLOB UI_TEST_SOURCES CONFIGURE_DEPENDS tests/ui/test_*.cpp)
    foreach(t ${UI_TEST_SOURCES})
      get_filename_component(tname ${t} NAME_WE)
      add_executable(${tname} ${t})
      target_link_libraries(${tname} PRIVATE inkframe_ui)
      if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${tname} PRIVATE -UNDEBUG)
      endif()
      add_test(NAME ${tname} COMMAND ${tname})
      set_tests_properties(${tname} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endforeach()
  endif()
endif()
