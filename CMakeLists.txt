cmake_minimum_required(VERSION 3.28)

project(pdwclap LANGUAGES C CXX)

# wclap-bridge is C++ (its Wasmtime embedding), and its own CMakeLists only
# sets C++20 when it's the top-level project — which it isn't here.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT GENERATOR_IS_MULTI_CONFIG AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

include(FetchContent)

# CLAP headers — the plugin standard wclap-bridge speaks.
FetchContent_Declare(clap URL https://github.com/free-audio/clap/archive/refs/tags/1.2.6.zip)
FetchContent_MakeAvailable(clap)

# wclap-bridge: loads a WCLAP (a CLAP plugin compiled to wasm32) through
# Wasmtime and hands back an ordinary clap_plugin_factory_t, so the host code
# in src/wclap_host.c is a normal CLAP host. Its GUI module is dropped by the
# patch — see cmake/patch-wclap-bridge.cmake.
FetchContent_Declare(wclap-bridge
    GIT_REPOSITORY https://github.com/WebCLAP/wclap-bridge.git
    GIT_TAG cd11d22afbe2af350f24cd56e6e0536e5ca86452
    GIT_SUBMODULES_RECURSE TRUE
    PATCH_COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_LIST_DIR}/cmake/patch-wclap-bridge.cmake
)
FetchContent_MakeAvailable(wclap-bridge)
target_include_directories(wclap-bridge PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/vendor/webview-gui-stub)
if(WIN32)
  # wasmtime-c-api is fetched from inside wclap-bridge's own CMakeLists (one
  # scope deeper), so its *_SOURCE_DIR isn't a normal variable here yet;
  # FetchContent_GetProperties re-reads it from FetchContent's bookkeeping.
  FetchContent_GetProperties(wasmtime-c-api)
endif()

# --- Pd headers -------------------------------------------------------------
# Only m_pd.h (and a couple of neighbours) are needed to compile an external;
# they're in the Pd app bundle on macOS and in pd-dev/pd packages on Linux.
# Failing that, pull the headers out of a Pd source release — the external
# doesn't link against Pd at all, it resolves Pd's symbols when Pd loads it.

set(PD_INCLUDE_DIR "" CACHE PATH "Directory containing Pd's m_pd.h")

if(NOT PD_INCLUDE_DIR)
  file(GLOB pd_apps /Applications/Pd*.app)
  foreach(app ${pd_apps})
    if(EXISTS "${app}/Contents/Resources/src/m_pd.h")
      set(PD_INCLUDE_DIR "${app}/Contents/Resources/src")
      break()
    endif()
  endforeach()
endif()
if(NOT PD_INCLUDE_DIR)
  foreach(dir /usr/include/pd /usr/local/include/pd /opt/homebrew/include/pd /opt/local/include/pd)
    if(EXISTS "${dir}/m_pd.h")
      set(PD_INCLUDE_DIR "${dir}")
      break()
    endif()
  endforeach()
endif()
if(NOT PD_INCLUDE_DIR)
  set(pd_headers ${CMAKE_BINARY_DIR}/pd-headers)
  if(NOT EXISTS ${pd_headers}/m_pd.h)
    message(STATUS "No Pd headers found — downloading them from a Pd source release")
    file(DOWNLOAD https://github.com/pure-data/pure-data/archive/refs/tags/0.56-0.tar.gz
        ${CMAKE_BINARY_DIR}/pd-source.tar.gz SHOW_PROGRESS)
    file(ARCHIVE_EXTRACT INPUT ${CMAKE_BINARY_DIR}/pd-source.tar.gz DESTINATION ${CMAKE_BINARY_DIR}/pd-source)
    file(GLOB pd_source_dirs ${CMAKE_BINARY_DIR}/pd-source/*/src)
    file(MAKE_DIRECTORY ${pd_headers})
    foreach(dir ${pd_source_dirs})
      file(COPY ${dir}/ DESTINATION ${pd_headers} FILES_MATCHING PATTERN "*.h")
    endforeach()
  endif()
  set(PD_INCLUDE_DIR ${pd_headers})
endif()
message(STATUS "Pd headers: ${PD_INCLUDE_DIR}")

# --- external ---------------------------------------------------------------

if(WIN32)
  set(WCLAP_SUFFIX ".dll")
elseif(APPLE)
  set(WCLAP_SUFFIX ".pd_darwin")
else()
  set(WCLAP_SUFFIX ".pd_linux")
endif()

# Pd looks for an external in the patch's own directory first, so the built
# plugin lands next to the patches in this checkout (wclap~-help.pd, your own
# examples.pd) instead of in build/ — opening them then needs no -path setup.
# It is a build artifact all the same, and .gitignore knows it.
set(WCLAP_PD_OUTPUT_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE PATH "Where the built wclap~ external goes")

# Output name carries the ~, so Pd finds wclap~_setup() in wclap~.pd_darwin.
add_library(wclap MODULE src/wclap_host.c src/wclap_tilde.c)
set_target_properties(wclap PROPERTIES
    PREFIX ""
    OUTPUT_NAME "wclap~"
    SUFFIX "${WCLAP_SUFFIX}"
    LIBRARY_OUTPUT_DIRECTORY ${WCLAP_PD_OUTPUT_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${WCLAP_PD_OUTPUT_DIR}
    # wclap-bridge is C++; the final link has to be driven by the C++ driver.
    LINKER_LANGUAGE CXX
)
target_include_directories(wclap PRIVATE
    ${PD_INCLUDE_DIR}
    ${clap_SOURCE_DIR}/include
    ${wclap-bridge_SOURCE_DIR}/include
)
target_link_libraries(wclap PRIVATE wclap-bridge)
if(APPLE)
  # Pd's symbols are resolved when Pd dlopen()s the external.
  target_link_options(wclap PRIVATE "LINKER:-undefined,dynamic_lookup")
endif()
if(WIN32)
  target_compile_definitions(wclap PRIVATE WIN32_LEAN_AND_MEAN)
  if(MSVC)
    target_link_options(wclap PRIVATE /FORCE:UNRESOLVED)
  endif()
endif()

# Windows links wasmtime dynamically (see patch-wclap-bridge.cmake), so both
# the harness and Pd need wasmtime.dll next to them.
if(WIN32)
  foreach(target wclap)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${wasmtime-c-api_SOURCE_DIR}/lib/wasmtime.dll"
            "$<TARGET_FILE_DIR:${target}>"
    )
  endforeach()
endif()
