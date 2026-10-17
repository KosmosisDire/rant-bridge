# unit.<name>    one C++ executable per module, tests/unit/<name>.cpp
# tunnel         the tunnel against a freshly built bridge through Rant's TunnelNet, tests/tunnel
# tunnel.native  the same through Rant's native client, tests/tunnel/native.c
# find           two bridges finding each other over mDNS on loopback, tests/find

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")
foreach(_cfg ${CMAKE_CONFIGURATION_TYPES})
  string(TOUPPER "${_cfg}" _CFG)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_CFG} "${CMAKE_CURRENT_BINARY_DIR}/bin")
endforeach()

foreach(name auth config mdns)
  add_executable(unit_${name} unit/${name}.cpp)
  target_link_libraries(unit_${name} PRIVATE rant_bridge_core)
  add_test(NAME unit.${name} COMMAND unit_${name})
endforeach()

find_program(NODE node)
if(NODE)
  find_package(Threads REQUIRED)
  add_executable(tunnel_native tunnel/native.c)
  target_link_libraries(tunnel_native PRIVATE rant::rant Threads::Threads)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(tunnel_native PRIVATE rt)
  endif()
  add_test(NAME tunnel.native COMMAND "${NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/tunnel/native.mjs"
           "$<TARGET_FILE:rant-bridge>" "$<TARGET_FILE:tunnel_native>")

  # Rant's web package: the local checkout's when building against one, else the pinned
  # release's, fetched once.
  if(CPM_rant_SOURCE AND EXISTS "${CPM_rant_SOURCE}/bindings/web/dist/rant_wasm.mjs")
    set(RANT_WEB_PACKAGE "${CPM_rant_SOURCE}/bindings/web")
  else()
    set(_v "${CPM_PACKAGE_rant_VERSION}")
    set(RANT_WEB_PACKAGE "${CMAKE_BINARY_DIR}/deps/rant-middleware-${_v}/package")
    if(NOT EXISTS "${RANT_WEB_PACKAGE}/dist/rant_wasm.mjs")
      file(DOWNLOAD "https://github.com/KosmosisDire/Rant/releases/download/v${_v}/rant-middleware-${_v}.tgz"
           "${CMAKE_BINARY_DIR}/deps/rant-middleware-${_v}.tgz" STATUS _status)
      list(GET _status 0 _code)
      if(_code EQUAL 0)
        file(ARCHIVE_EXTRACT INPUT "${CMAKE_BINARY_DIR}/deps/rant-middleware-${_v}.tgz"
             DESTINATION "${CMAKE_BINARY_DIR}/deps/rant-middleware-${_v}")
      endif()
    endif()
  endif()
  add_test(NAME find COMMAND "${NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/find/test.mjs" "$<TARGET_FILE:rant-bridge>"
           "$<$<BOOL:${RANT_WEB_PACKAGE}>:${RANT_WEB_PACKAGE}>")
  if(EXISTS "${RANT_WEB_PACKAGE}/dist/rant_wasm.mjs")
    add_test(NAME tunnel COMMAND "${NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/tunnel/test.mjs" "$<TARGET_FILE:rant-bridge>"
             "${RANT_WEB_PACKAGE}")
  else()
    message(WARNING "no tunnel test: this Rant has no web package with the binding (${RANT_WEB_PACKAGE})")
  endif()
endif()
