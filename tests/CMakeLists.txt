# unit.<name>  one C++ executable per module, tests/unit/<name>.cpp
# client       the TS client's suite against a freshly built bridge, tests/client

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")
foreach(_cfg ${CMAKE_CONFIGURATION_TYPES})
  string(TOUPPER "${_cfg}" _CFG)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_CFG} "${CMAKE_CURRENT_BINARY_DIR}/bin")
endforeach()

foreach(name codec config frame)
  add_executable(unit_${name} unit/${name}.cpp)
  target_link_libraries(unit_${name} PRIVATE rant_bridge_core)
  add_test(NAME unit.${name} COMMAND unit_${name})
endforeach()

find_program(NODE node)
if(NODE)
  add_test(NAME client COMMAND "${NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/client/run.mjs" "$<TARGET_FILE:rant-bridge>"
           WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/..")
endif()
