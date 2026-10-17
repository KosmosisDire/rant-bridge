# Everything but main() is one static library, so the unit tests link the same code.

add_library(rant_bridge_core STATIC
  config.cpp
  rtc.cpp
  server.cpp
  session.cpp
  tunnel.cpp)
target_include_directories(rant_bridge_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(rant_bridge_core PUBLIC ixwebsocket::ixwebsocket nlohmann_json::nlohmann_json yaml-cpp::yaml-cpp
                      LibDataChannel::LibDataChannelStatic)
target_compile_definitions(rant_bridge_core PUBLIC RANT_BRIDGE_VERSION="${PROJECT_VERSION}")
if(WIN32)
  target_link_libraries(rant_bridge_core PUBLIC iphlpapi)     # the tunnel lists this host's interfaces
endif()
if(MSVC)
  target_compile_options(rant_bridge_core PRIVATE /W4 /permissive-)
  # getenv and sscanf are used as the standard defines them
  target_compile_definitions(rant_bridge_core PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(rant_bridge_core PRIVATE -Wall -Wextra)
endif()

add_executable(rant-bridge main.cpp)
target_link_libraries(rant-bridge PRIVATE rant_bridge_core)
# Windowless on Windows, so a service start opens no console. It attaches to the terminal
# that starts it by hand.
if(WIN32)
  set_target_properties(rant-bridge PROPERTIES WIN32_EXECUTABLE TRUE)
  if(MSVC)
    target_link_options(rant-bridge PRIVATE /ENTRY:mainCRTStartup)
  else()
    target_link_options(rant-bridge PRIVATE -mwindows)
  endif()
endif()
