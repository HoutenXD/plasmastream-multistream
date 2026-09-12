cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

# Both on, unlike the stock template. The frontend API is the only way to learn
# that streaming started, which is the only moment the encoders exist to borrow.
option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  # Svg as well, for the platform marks. OBS ships Qt6Svg.dll and the svg image
  # format plugin beside its own binary, so this adds nothing to what a user
  # already has, and it means the logos in the dock are the same paths the
  # website draws rather than an approximation of them.
  find_package(Qt6 COMPONENTS Widgets Core Svg)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets Qt6::Svg)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

# Not Qt Network: Qt loads its TLS backend from a "tls" plugin directory OBS
# does not ship, so https fails at handshake. OBS ships and uses libcurl itself.
find_package(CURL REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE CURL::libcurl)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/appearance.cpp
    src/appearance.hpp
    src/canvases.cpp
    src/canvases.hpp
    src/config.cpp
    src/config.hpp
    src/dock.cpp
    src/dock.hpp
    src/http.cpp
    src/http.hpp
    src/outputs.cpp
    src/outputs.hpp
    src/preview.cpp
    src/preview.hpp
    src/scenerec.cpp
    src/scenerec.hpp
    src/vertical.cpp
    src/vertical.hpp
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

# A dev tool, off unless asked for. See tools/dock-preview/main.cpp for why it
# exists: the dock is the one part of this plugin nobody can review by reading.
option(BUILD_DOCK_PREVIEW "Build the offscreen dock renderer" OFF)

if(BUILD_DOCK_PREVIEW AND ENABLE_QT)
  add_executable(dock-preview tools/dock-preview/main.cpp src/appearance.cpp src/appearance.hpp)
  target_link_libraries(dock-preview PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg)
  set_target_properties(dock-preview PROPERTIES AUTOMOC ON)
endif()
