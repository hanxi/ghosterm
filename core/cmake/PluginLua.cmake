# Official source archive and SHA-256 from https://www.lua.org/ftp/.
set(GHOSTERM_LUA_ARCHIVE "" CACHE FILEPATH "Local copy of official lua-5.5.1.tar.gz")
set(_gt_lua_sha "1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce")
if(NOT EXISTS "${GHOSTERM_LUA_ARCHIVE}" OR IS_DIRECTORY "${GHOSTERM_LUA_ARCHIVE}")
  message(FATAL_ERROR "Plugin Lua requires GHOSTERM_LUA_ARCHIVE pointing to lua-5.5.1.tar.gz")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GHOSTERM_LUA_ARCHIVE}")
file(SHA256 "${GHOSTERM_LUA_ARCHIVE}" _gt_lua_archive_sha)
if(NOT _gt_lua_archive_sha STREQUAL _gt_lua_sha)
  message(FATAL_ERROR "Lua 5.5.1 source archive SHA-256 mismatch")
endif()
file(ARCHIVE_EXTRACT INPUT "${GHOSTERM_LUA_ARCHIVE}" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")
set(_gt_lua_src "${CMAKE_CURRENT_BINARY_DIR}/lua-5.5.1/src")
set(_gt_lua_sources
  lapi.c lauxlib.c lbaselib.c lcode.c lctype.c ldebug.c ldo.c ldump.c
  lfunc.c lgc.c llex.c lmathlib.c lmem.c lobject.c lopcodes.c lparser.c
  lstate.c lstring.c lstrlib.c ltable.c ltablib.c ltm.c lundump.c lutf8lib.c
  lvm.c lzio.c
)
list(TRANSFORM _gt_lua_sources PREPEND "${_gt_lua_src}/")
add_library(ghosterm_lua STATIC ${_gt_lua_sources})
set_target_properties(ghosterm_lua PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(ghosterm_lua SYSTEM PUBLIC "${_gt_lua_src}")
target_link_libraries(ghosterm_lua PUBLIC m)
target_compile_definitions(ghosterm_core PRIVATE GT_HAVE_PLUGIN_LUA=1)
target_sources(ghosterm_core PRIVATE src/extension/lua-vm.c src/extension/lua-json.c src/extension/lua-rpc.c src/extension/lua-resource.c src/extension/lua-process.c)
target_link_libraries(ghosterm_core PRIVATE ghosterm_lua)

# Embed the trusted argument decoder. Package Lua never runs during preflight.
set(_gt_lua_decoder "${CMAKE_CURRENT_LIST_DIR}/../src/extension/lua-cli-decoder.lua")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_gt_lua_decoder}")
file(READ "${_gt_lua_decoder}" _gt_lua_decoder_hex HEX)
string(REGEX REPLACE "(..)" "0x\\1," _gt_lua_decoder_bytes "${_gt_lua_decoder_hex}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/gt-lua-cli-decoder.inc"
     CONTENT "${_gt_lua_decoder_bytes}\n" @ONLY)
target_include_directories(ghosterm_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
