cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

# Both on, unlike the stock template, and both are load-bearing.
#
# The frontend API is how the plugin learns that streaming started, which is the
# only moment the main encoder exists to borrow. libobs alone has no notion of
# "the user pressed Start Streaming" in the UI.
#
# Qt is the dock. A multistream plugin with no interface would mean editing a
# JSON file by hand, which is not a thing to ask of somebody who is about to go
# live.
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
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

# libcurl, for the one HTTP GET that syncs destinations.
#
# NOT Qt Network, which was the first attempt and cannot do HTTPS inside OBS. Qt
# loads a TLS backend from a "tls" plugin directory, and OBS ships "platforms",
# "styles" and "imageformats" and not that one, so every https request failed
# before leaving the process.
#
# curl is not a new dependency: OBS ships libcurl.dll and uses it itself.
find_package(CURL REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE CURL::libcurl)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/config.cpp
    src/config.hpp
    src/dock.cpp
    src/dock.hpp
    src/http.cpp
    src/http.hpp
    src/outputs.cpp
    src/outputs.hpp
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
