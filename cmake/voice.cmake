# Voice commands: speech recognition on the streamer's own computer.
#
# sherpa-onnx runs Silero VAD and a Moonshine model on ONNX Runtime. None of it is
# vendored into this repository: the runtime, the two models and their licenses
# are fetched once into the build folder from each project's own release, pinned
# to an exact version and checked by SHA-256, then staged as data/voice beside the
# plugin.
#
# The DLL build of sherpa-onnx, not the static one. The static libraries are
# compiled with a newer MSVC standard library than Visual Studio 17.10 links
# against, and a DLL has no such coupling. The DLLs live in data/voice/bin rather
# than beside the plugin, and are delay-loaded by full path: see load_runtime in
# src/voice.cpp for why that matters when other plugins bring ONNX Runtime too.
#
# Windows only for now. Elsewhere voice.cpp builds as a stub that says so.

set(VOICE_SHERPA_VERSION "1.13.8")
set(VOICE_DEPS "${CMAKE_BINARY_DIR}/voice-deps")
set(VOICE_DOWNLOADS "${VOICE_DEPS}/downloads")
set(VOICE_STAGE "${VOICE_DEPS}/stage/voice")
set(VOICE_RELEASES "https://github.com/k2-fsa/sherpa-onnx/releases/download")
set(VOICE_RAW "https://raw.githubusercontent.com")

function(voice_fetch url destination hash)
  if(EXISTS "${destination}")
    file(SHA256 "${destination}" existing)

    if(existing STREQUAL "${hash}")
      return()
    endif()
  endif()

  message(STATUS "Voice: downloading ${url}")
  file(DOWNLOAD "${url}" "${destination}" EXPECTED_HASH SHA256=${hash} TLS_VERIFY ON STATUS status)
  list(GET status 0 code)

  if(NOT code EQUAL 0)
    list(GET status 1 reason)
    message(FATAL_ERROR "Voice: could not download ${url}: ${reason}")
  endif()
endfunction()

function(voice_extract archive destination)
  if(NOT EXISTS "${destination}/.extracted")
    file(REMOVE_RECURSE "${destination}")
    file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${destination}")
    file(TOUCH "${destination}/.extracted")
  endif()
endfunction()

set(_voice_lib_name "sherpa-onnx-v${VOICE_SHERPA_VERSION}-win-x64-shared-MD-Release-no-tts-lib")
set(_voice_tiny "sherpa-onnx-moonshine-tiny-en-quantized-2026-02-27")
set(_voice_base "sherpa-onnx-moonshine-base-en-quantized-2026-02-27")

voice_fetch(
  "${VOICE_RELEASES}/v${VOICE_SHERPA_VERSION}/${_voice_lib_name}.tar.bz2"
  "${VOICE_DOWNLOADS}/${_voice_lib_name}.tar.bz2"
  "b90992b888710715d613a4fedcd75d5b4db294ef64f3194e4f77ea0a8b387441"
)
voice_fetch(
  "${VOICE_RAW}/k2-fsa/sherpa-onnx/v${VOICE_SHERPA_VERSION}/sherpa-onnx/c-api/c-api.h"
  "${VOICE_DEPS}/include/sherpa-onnx/c-api/c-api.h"
  "2a1b95084be8fd1deb3228fcad2fd3f7f0258b64582f7402281ec174c7b7f4ce"
)
voice_fetch(
  "${VOICE_RELEASES}/asr-models/${_voice_tiny}.tar.bz2"
  "${VOICE_DOWNLOADS}/${_voice_tiny}.tar.bz2"
  "9ec31b342d8fa3240c3b81b8f82e1cf7e3ac467c93ca5a999b741d5887164f8d"
)
voice_fetch(
  "${VOICE_RELEASES}/asr-models/${_voice_base}.tar.bz2"
  "${VOICE_DOWNLOADS}/${_voice_base}.tar.bz2"
  "43232c1d13013d37317163baec3135bd771a186a4356f28c889bab453bb0e891"
)
voice_fetch(
  "${VOICE_RELEASES}/asr-models/silero_vad.onnx"
  "${VOICE_STAGE}/silero_vad.onnx"
  "9e2449e1087496d8d4caba907f23e0bd3f78d91fa552479bb9c23ac09cbb1fd6"
)
voice_fetch(
  "${VOICE_RAW}/k2-fsa/sherpa-onnx/v${VOICE_SHERPA_VERSION}/LICENSE"
  "${VOICE_STAGE}/licenses/sherpa-onnx-LICENSE.txt"
  "cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30"
)
voice_fetch(
  "${VOICE_RAW}/microsoft/onnxruntime/v1.20.0/LICENSE"
  "${VOICE_STAGE}/licenses/onnxruntime-LICENSE.txt"
  "2f07c72751aed99790b8a4869cf2311df85a860b22ded05fa22803587a48922c"
)
voice_fetch(
  "${VOICE_RAW}/snakers4/silero-vad/v5.1.2/LICENSE"
  "${VOICE_STAGE}/licenses/silero-vad-LICENSE.txt"
  "2e63e9a38b6e8fc0c7bc37ce174caca1862870856c6daf5697cfb785e925520b"
)

voice_extract("${VOICE_DOWNLOADS}/${_voice_lib_name}.tar.bz2" "${VOICE_DEPS}/sherpa")
voice_extract("${VOICE_DOWNLOADS}/${_voice_tiny}.tar.bz2" "${VOICE_DEPS}/models/fast")
voice_extract("${VOICE_DOWNLOADS}/${_voice_base}.tar.bz2" "${VOICE_DEPS}/models/accurate")

set(_voice_lib "${VOICE_DEPS}/sherpa/${_voice_lib_name}/lib")

file(
  COPY "${_voice_lib}/onnxruntime.dll" "${_voice_lib}/onnxruntime_providers_shared.dll" "${_voice_lib}/sherpa-onnx-c-api.dll"
  DESTINATION "${VOICE_STAGE}/bin"
)

# The model files only. The archives also carry sample recordings nobody needs.
foreach(_voice_model IN ITEMS fast accurate)
  if(_voice_model STREQUAL "fast")
    set(_voice_from "${VOICE_DEPS}/models/fast/${_voice_tiny}")
  else()
    set(_voice_from "${VOICE_DEPS}/models/accurate/${_voice_base}")
  endif()

  file(
    COPY "${_voice_from}/encoder_model.ort" "${_voice_from}/decoder_model_merged.ort" "${_voice_from}/tokens.txt"
    DESTINATION "${VOICE_STAGE}/models/${_voice_model}"
  )
endforeach()

file(COPY "${VOICE_DEPS}/models/fast/${_voice_tiny}/LICENSE" DESTINATION "${VOICE_STAGE}/licenses")
file(RENAME "${VOICE_STAGE}/licenses/LICENSE" "${VOICE_STAGE}/licenses/moonshine-LICENSE.txt")

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE PLASMASTREAM_VOICE SHERPA_ONNX_BUILD_SHARED_LIBS)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${VOICE_DEPS}/include")
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE "${_voice_lib}/sherpa-onnx-c-api.lib" delayimp)
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE "/DELAYLOAD:sherpa-onnx-c-api.dll")

add_custom_command(
  TARGET ${CMAKE_PROJECT_NAME}
  POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>/${CMAKE_PROJECT_NAME}/voice"
  COMMAND
    "${CMAKE_COMMAND}" -E copy_directory "${VOICE_STAGE}"
    "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>/${CMAKE_PROJECT_NAME}/voice"
  COMMENT "Copy voice runtime and models to rundir"
  VERBATIM
)

install(DIRECTORY "${VOICE_STAGE}" DESTINATION "${CMAKE_PROJECT_NAME}/data")
