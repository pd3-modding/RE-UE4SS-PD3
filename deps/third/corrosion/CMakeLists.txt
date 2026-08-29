include(FetchContent)

set(FETCHCONTENT_QUIET OFF)

FetchContent_MakeAvailable(corrosion)

# Import Rust crate
corrosion_import_crate(MANIFEST_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../first/patternsleuth_bind/Cargo.toml")

# Include the patternsleuth_bind CMakeLists.txt for folder organization
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../first/patternsleuth_bind" "${CMAKE_CURRENT_BINARY_DIR}/patternsleuth_bind")

# MCP server (rmcp + axum), hosted in-process and enabled from UE4SS-settings.ini [MCP].
# Only built when UE4SS_ENABLE_MCP is on: it pulls a tokio/axum dependency tree and adds
# noticeably to build time, so it stays opt-in.
if(UE4SS_ENABLE_MCP)
    corrosion_import_crate(MANIFEST_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../first/mcp_bind/Cargo.toml")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../first/mcp_bind" "${CMAKE_CURRENT_BINARY_DIR}/mcp_bind")
endif()
