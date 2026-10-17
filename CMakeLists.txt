# The Rant bridge: lends this host's UDP sockets to Rant nodes in pages, over WebSocket and
# WebRTC. CPM fetches IXWebSocket, nlohmann json, yaml-cpp, MbedTLS and libdatachannel, and Rant
# for its build helpers and for the client the tests run.

cmake_minimum_required(VERSION 3.21)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" RANT_BRIDGE_VERSION LIMIT_COUNT 1)
project(rant_bridge VERSION ${RANT_BRIDGE_VERSION} LANGUAGES C CXX)
# a bumped VERSION configures again, so the binary never reports the old one
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Release first so a bare multi config build is Release. Every config lands in bin/.
if(CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_CONFIGURATION_TYPES "Release;Debug;RelWithDebInfo;MinSizeRel"
      CACHE STRING "Build configs (Release first = the default)" FORCE)
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin")
foreach(_cfg ${CMAKE_CONFIGURATION_TYPES})
  string(TOUPPER "${_cfg}" _CFG)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_CFG} "${CMAKE_CURRENT_SOURCE_DIR}/bin")
endforeach()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/get_cpm.cmake")

# CPM patches with the first patch on PATH. On Windows that can be Strawberry Perl's old one,
# which crashes on ours, so take the one Git ships.
if(CMAKE_HOST_WIN32 AND NOT PATCH_EXECUTABLE)
  find_package(Git QUIET)
  if(GIT_EXECUTABLE)
    get_filename_component(git_dir "${GIT_EXECUTABLE}" DIRECTORY)
    find_program(PATCH_EXECUTABLE patch HINTS "${git_dir}/../usr/bin" "${git_dir}/../../usr/bin" NO_DEFAULT_PATH)
  endif()
endif()

# A release pins the newest Rant, whose tunnel client must speak this bridge's protocol. The
# local presets set CPM_rant_SOURCE to ../rant.
CPMAddPackage(NAME rant
  GITHUB_REPOSITORY KosmosisDire/Rant
  VERSION 0.0.30
  EXCLUDE_FROM_ALL YES
  SYSTEM YES)

# Static CRT: a copied exe must run with no VC++ redistributable. Before every dependency,
# so they all build against the same runtime. Linux stays dynamic: ICE resolves STUN host
# names, which a static glibc cannot do.
include("${rant_SOURCE_DIR}/tools/static_runtime.cmake")
# libstdc++ and libgcc go inside the Linux exe too, so it needs only a glibc as old as the build's.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_link_options(-static-libstdc++ -static-libgcc)
endif()

# TLS and zlib off: the bridge sits behind a tunnel or on localhost, and raw speed matters.
# The patch lets a connection read while it sends, drops wsock32, which would take the tunnel's
# socket calls on Windows and break its multicast, and keeps a second bridge off the port of a
# running one on Windows.
CPMAddPackage(NAME ixwebsocket
  URL https://github.com/machinezone/IXWebSocket/archive/refs/tags/v12.0.1.tar.gz
  VERSION 12.0.1
  EXCLUDE_FROM_ALL YES
  PATCHES "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ixwebsocket.patch"
  OPTIONS "USE_TLS OFF" "USE_ZLIB OFF" "IXWEBSOCKET_INSTALL OFF")

CPMAddPackage(NAME nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  VERSION 3.12.0
  EXCLUDE_FROM_ALL YES)

# yaml-cpp 0.8.0 asks for an older CMake than CMake 4 still accepts.
CPMAddPackage(NAME yaml-cpp
  GITHUB_REPOSITORY jbeder/yaml-cpp
  GIT_TAG 0.8.0
  EXCLUDE_FROM_ALL YES
  OPTIONS "YAML_CPP_BUILD_TESTS OFF" "YAML_CPP_BUILD_TOOLS OFF" "YAML_CPP_BUILD_CONTRIB OFF"
          "YAML_BUILD_SHARED_LIBS OFF" "YAML_CPP_INSTALL OFF" "CMAKE_POLICY_VERSION_MINIMUM 3.5")

# MbedTLS is the DTLS backend. Its release tarball ships generated sources, so
# no perl or python runs at build time. Not named mbedtls: CPM's stand in finder for that
# name would shadow our FindMbedTLS on a case insensitive file system.
set(MBEDTLS_USER_CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/mbedtls_user_config.h" CACHE FILEPATH "" FORCE)
CPMAddPackage(NAME mbedtls_release
  URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.4/mbedtls-3.6.4.tar.bz2
  VERSION 3.6.4
  EXCLUDE_FROM_ALL YES
  OPTIONS "ENABLE_PROGRAMS OFF" "ENABLE_TESTING OFF" "MBEDTLS_FATAL_WARNINGS OFF" "GEN_FILES OFF"
          "DISABLE_PACKAGE_CONFIG_AND_INSTALL ON")

# Our FindMbedTLS maps the fetched MbedTLS onto the names libdatachannel looks for. Data
# channels only, no media.
list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
set(CMAKE_SKIP_INSTALL_RULES ON)
set(BUILD_SHARED_LIBS OFF)
CPMAddPackage(NAME libdatachannel
  GITHUB_REPOSITORY paullouisageneau/libdatachannel
  GIT_TAG v0.23.3
  EXCLUDE_FROM_ALL YES
  GIT_SUBMODULES "deps/libjuice" "deps/usrsctp" "deps/plog"
  GIT_SUBMODULES_RECURSE FALSE
  OPTIONS "USE_MBEDTLS ON" "NO_WEBSOCKET ON" "NO_EXAMPLES ON" "NO_TESTS ON" "NO_MEDIA ON"
          "ENABLE_WARNINGS_AS_ERRORS OFF")

option(RANT_BRIDGE_TESTS "Build the tests" ${PROJECT_IS_TOP_LEVEL})
if(RANT_BRIDGE_TESTS)
  enable_testing()
endif()

add_subdirectory(src)

if(RANT_BRIDGE_TESTS)
  add_subdirectory(tests)
endif()
