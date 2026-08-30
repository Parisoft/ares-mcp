set(CONSOLE TRUE)

add_executable(mcp
  main.cpp
  core-host.cpp
  server.cpp
)

target_include_directories(mcp PRIVATE ${CMAKE_SOURCE_DIR})

target_link_libraries(
  mcp
  PRIVATE ares::ares ares::mia ares::nall
)

set_target_properties(mcp PROPERTIES OUTPUT_NAME ares-mcp)
ares_configure_executable(mcp)
