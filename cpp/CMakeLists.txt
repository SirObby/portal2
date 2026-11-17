cmake_minimum_required(VERSION 3.16)
project(portal2_rl_plugin CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Portal 2 is a 32-bit engine on both Linux and Windows; the plugin must be
# built as a 32-bit shared library to load into it.
# ---------------------------------------------------------------------------
if(NOT WIN32)
    add_compile_options(-m32)
    add_link_options(-m32)
endif()

# ---------------------------------------------------------------------------
# nlohmann/json for control protocol message (de)serialization. We fetch
# the single-header release directly (rather than the full repo/release
# tarball) since it avoids a build step and redirect-following that can be
# blocked by restrictive outbound network/proxy configurations.
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    json
    URL https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp
    DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_MakeAvailable(json)

add_library(nlohmann_json INTERFACE)
add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
file(MAKE_DIRECTORY ${json_SOURCE_DIR}/nlohmann)
file(COPY ${json_SOURCE_DIR}/json.hpp DESTINATION ${json_SOURCE_DIR}/nlohmann)
target_include_directories(nlohmann_json INTERFACE ${json_SOURCE_DIR})

# ---------------------------------------------------------------------------
# Vendored SourceAutoRecord engine-interaction layer (MIT licensed, see
# vendor/sar/LICENSE). This subset gives us: vtable-based interface
# resolution (Interface), raw memory utilities and the 5-byte JMP hook
# primitive (Memory, Hook), the engine offset table for our target Portal 2
# build (Offsets, see vendor/sar/Offsets/Portal 2 9568.hpp), and named
# datamap/SendTable field access on entities (Entity).
# ---------------------------------------------------------------------------
set(SAR_VENDOR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/sar)

add_library(sar_vendor STATIC
    ${SAR_VENDOR_DIR}/Entity.cpp
    ${SAR_VENDOR_DIR}/Interface.cpp
    ${SAR_VENDOR_DIR}/Offsets.cpp
    ${SAR_VENDOR_DIR}/Utils/Memory.cpp
    ${SAR_VENDOR_DIR}/Utils/SDK/ServerPlugin.cpp
    ${SAR_VENDOR_DIR}/Modules/Console.cpp
)
target_include_directories(sar_vendor PUBLIC ${SAR_VENDOR_DIR})

# ---------------------------------------------------------------------------
# Plugin target
# ---------------------------------------------------------------------------
add_library(portal2_rl_plugin SHARED
    src/Plugin.cpp
    src/Engine.cpp
    src/RLState.cpp
    src/RLInput.cpp
    src/RLBridge.cpp
    src/DemoCapture.cpp
)

target_include_directories(portal2_rl_plugin PRIVATE src)
target_link_libraries(portal2_rl_plugin PRIVATE sar_vendor nlohmann_json::nlohmann_json)

if(WIN32)
    target_link_libraries(portal2_rl_plugin PRIVATE ws2_32)
    set_target_properties(portal2_rl_plugin PROPERTIES
        OUTPUT_NAME "portal2_rl_plugin"
        SUFFIX ".dll"
    )
else()
    set_target_properties(portal2_rl_plugin PROPERTIES
        OUTPUT_NAME "portal2_rl_plugin"
        PREFIX ""
        SUFFIX ".so"
    )
    # -fpermissive: the vendored SAR headers use a handful of patterns
    # (anonymous unions in bit-field structs, etc.) that are technically
    # non-standard but compile fine as warnings under GCC/Clang with this
    # flag, matching how SAR's own build is configured.
    target_compile_options(portal2_rl_plugin PRIVATE -fpermissive -Wno-write-strings)
    target_compile_options(sar_vendor PRIVATE -fpermissive -Wno-write-strings)
endif()
