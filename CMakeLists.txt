cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

# Override the CMakePresets.json defaults which leave these OFF.
# Smart Selection requires both the frontend API and Qt6 Widgets.
set(ENABLE_FRONTEND_API ON CACHE BOOL "Use obs-frontend-api for UI functionality" FORCE)
set(ENABLE_QT           ON CACHE BOOL "Use Qt functionality"                      FORCE)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

# --- Core source list ---
# (plugin-support.c/h are handled automatically by helpers_common.cmake → don't list)
target_sources(${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.c
    src/smart-selection-source.cpp
    src/selection-overlay.cpp
    src/selection-overlay.hpp
)

# --- libobs (always required) ---
find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

# --- obs-frontend-api (for QApplication access via OBS UI) ---
if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME}
    PRIVATE OBS::obs-frontend-api)
endif()

# --- Qt6 (Widgets needed for the fullscreen overlay) ---
if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core Gui REQUIRED)
  set_target_properties(${CMAKE_PROJECT_NAME}
    PROPERTIES
      AUTOMOC ON
      AUTOUIC ON
      AUTORCC ON
  )
  target_link_libraries(${CMAKE_PROJECT_NAME}
    PRIVATE
      Qt6::Core
      Qt6::Gui
      Qt6::Widgets
  )
  target_compile_options(${CMAKE_PROJECT_NAME}
    PRIVATE
      $<$<C_COMPILER_ID:Clang,AppleClang,GNU>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
endif()

# --- C++17 (designated init not required; use ordered/