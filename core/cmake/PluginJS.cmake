# Explicit optional backend. No FetchContent, network or host-library fallback.
set(_gt_js_root "${CMAKE_CURRENT_LIST_DIR}/../../vendor/quickjs-ng")
if(NOT EXISTS "${_gt_js_root}/quickjs.c")
  message(FATAL_ERROR "Plugin JS enabled: initialize pinned vendor/quickjs-ng submodule")
endif()
find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${_gt_js_root}" rev-parse HEAD OUTPUT_VARIABLE _gt_js_head OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _gt_js_result)
if(NOT _gt_js_result EQUAL 0 OR NOT _gt_js_head STREQUAL "d7215b0ad3fc2c562e8317fd9bca058fd4863804")
  message(FATAL_ERROR "QuickJS-NG commit differs from the pinned 0.18.0 source")
endif()
set(_gt_js_files
  "LICENSE:96f73f9d2a16c21a36b418f06073be26e7d6d5e7c1bc99756b21a4f2c74ef171"
  "builtin-array-fromasync.h:d313ac1e47eaa08f39b2cb0929a512d88e492906c400f650e141d00b79faa905"
  "builtin-iterator-zip-keyed.h:5b015a8063088987f0fb19357c9da5ec44a7197493fdbb23740fde8f470049a4"
  "builtin-iterator-zip.h:54834b615ddc59541515a7aa6d2c66fbfbf3213608f52e463d2469d63d395465"
  "cutils.h:dffec8bea17f08108da5a91c1178804754698bbbb6202dc100268e78e8c5b770"
  "dtoa.c:a8d2cf2d04db406e0c16acdd95ab0e709d309902865722f41dac07a1ba5676af"
  "dtoa.h:1c4a4540c0038632c8a4c225472fbc21bd476fb35c4da4a857a11965b58231b9"
  "libregexp-opcode.h:2a98d646089f3a72f25480116297c5fdb5b28fd9f815447273951f4ade4a115e"
  "libregexp.c:af13e996abb1767fe0cdfda123c45b811d53688d403a2ac47142e97bf79f1fae"
  "libregexp.h:53ff95a038f7001b752c455d195d998e2955f0ae4ed62a002c13eca798db3ceb"
  "libunicode-table.h:5840a921ac11664b505ee37b1219f518fff2a04972fe848b4bdf1e85c5a0bd83"
  "libunicode.c:c4724e77ec5f122373d2635b6d481cfb5d96024a38453a54a0830007c4b5f3c9"
  "libunicode.h:ad13f66aaea3fab1616e84b9c048f2a315e1bdbab628b966617a7a3e55bcba2b"
  "list.h:cb6e24e8ee54bccdd66234fe54cd702d1160b9841db2b9b9adeb5003afe301d5"
  "quickjs-atom.h:8045b870825a43ecdb50692aac7ad21b43139b288df466ca5f7b780bde4ac13f"
  "quickjs-c-atomics.h:29883c742a5c512278fce54d19b1b9af949006e1d7dfc9ffa2e0946b184d8c5c"
  "quickjs-libc.h:55c6b4504124941903fc09c35aecedddb88fa45c27d5e62c65962bb670ae17d1"
  "quickjs-opcode.h:0054b0c45fd091afa308c3fb93b5862e0b071235918d3d73e413d23fce388bbc"
  "quickjs.c:51b1d3919ffe812d0e01793f85d347bcea48c924c11262c5c5f00df1e4759d74"
  "quickjs.h:6d3879c856551653a1c64fabe52d6d07dc84e97bac3df45848df626702fc30de"
  "unicode_gen_def.h:02449c8cf482b2811a32f528d72a0ff4602dc8ffc4135e330eb05bae05d359d8"
 )
foreach(_gt_entry IN LISTS _gt_js_files)
  string(REPLACE ":" ";" _gt_parts "${_gt_entry}")
  list(GET _gt_parts 0 _gt_file)
  list(GET _gt_parts 1 _gt_expected)
  if(NOT EXISTS "${_gt_js_root}/${_gt_file}")
    message(FATAL_ERROR "Pinned QuickJS-NG source missing: ${_gt_file}")
  endif()
  file(SHA256 "${_gt_js_root}/${_gt_file}" _gt_actual)
  if(NOT _gt_actual STREQUAL _gt_expected)
    message(FATAL_ERROR "Pinned QuickJS-NG source modified: ${_gt_file}")
  endif()
endforeach()
add_library(ghosterm_quickjs STATIC
  "${_gt_js_root}/quickjs.c" "${_gt_js_root}/dtoa.c"
  "${_gt_js_root}/libregexp.c" "${_gt_js_root}/libunicode.c")
set_target_properties(ghosterm_quickjs PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
target_include_directories(ghosterm_quickjs SYSTEM PUBLIC "${_gt_js_root}")
target_compile_definitions(ghosterm_quickjs PRIVATE _GNU_SOURCE)
target_compile_options(ghosterm_quickjs PRIVATE -fwrapv)
target_link_libraries(ghosterm_quickjs PUBLIC Threads::Threads m)
target_compile_definitions(ghosterm_core PRIVATE GT_HAVE_PLUGIN_JS=1)
target_link_libraries(ghosterm_core PUBLIC ghosterm_quickjs)
