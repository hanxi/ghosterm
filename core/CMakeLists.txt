cmake_minimum_required(VERSION 3.19)
project(ghosterm_core C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

find_package(Threads REQUIRED)
if(CMAKE_CROSSCOMPILING)
  set(GHOSTERM_TESTS_DEFAULT OFF)
else()
  set(GHOSTERM_TESTS_DEFAULT ON)
endif()
option(GHOSTERM_BUILD_TESTS "Build host executables and register tests" ${GHOSTERM_TESTS_DEFAULT})

add_library(ghosterm_core STATIC
  src/base/secure-random.c
  src/base/sha256.c
  src/extension/stream.c
  src/extension/fd-stream.c
  src/extension/frame.c
  src/extension/wire.c
  src/extension/session.c
  src/extension/rpc.c
  src/extension/resource.c
  src/extension/transfer.c
  src/extension/source.c
  src/extension/channel.c
  src/extension/route.c
  src/extension/backends/ssh_exec.c
  src/core.c
  src/config.c
  src/known_hosts.c
  src/known_hosts_store.c
  src/transport.c
  src/io/pty_posix.c
  src/io/ssh_shell.c
  src/ssh.c
  src/ssh/authenticator.c
  src/tmux/command.c
  src/tmux/tmux.c
  src/sftp.c
)
target_include_directories(ghosterm_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(ghosterm_core PRIVATE -Wall -Wextra -Werror)
target_link_libraries(ghosterm_core PUBLIC Threads::Threads)

# SSH/SFTP 后端（libssh2）——开关是**显式的，不静默降级**（铁律 #6）。
# 默认 ON：主机消费锁定补丁库，Android 传入显式交叉构建 target。
# 无 SSH 依赖时显式配置 OFF，入口均返回 GT_ERR_UNSUPPORTED；不自动换回未修复库。
option(GHOSTERM_WITH_PLUGIN_CHANNEL "Expose native plugin channel providers (not JS runtime)" ON)
if(GHOSTERM_WITH_PLUGIN_CHANNEL)
  target_compile_definitions(ghosterm_core PRIVATE GT_HAVE_PLUGIN_CHANNEL=1)
endif()
option(GHOSTERM_WITH_LIBSSH2 "Build the libssh2 SSH/SFTP backend" ON)
set(GHOSTERM_SSH2_TARGET "" CACHE STRING "Explicit target for a cross-compiled libssh2 backend")
set(SSH2_FOUND FALSE)
if(GHOSTERM_WITH_LIBSSH2)
 if(GHOSTERM_SSH2_TARGET)
  if(NOT TARGET ${GHOSTERM_SSH2_TARGET})
    message(FATAL_ERROR "GHOSTERM_SSH2_TARGET must name an existing target")
  endif()
  target_compile_definitions(ghosterm_core PRIVATE GT_HAVE_LIBSSH2=1)
  target_link_libraries(ghosterm_core PUBLIC ${GHOSTERM_SSH2_TARGET})
  set(SSH2_FOUND TRUE)
 elseif(CMAKE_CROSSCOMPILING)
  message(FATAL_ERROR "Cross builds require GHOSTERM_SSH2_TARGET; host pkg-config is forbidden")
 else()
  include(cmake/HostSSH2.cmake)
 endif()
endif()



if(GHOSTERM_BUILD_TESTS)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
enable_testing()
add_executable(test_ext_channel tests/test_ext_channel.c)
target_link_libraries(test_ext_channel PRIVATE ghosterm_core)
target_compile_options(test_ext_channel PRIVATE -Wall -Wextra -Werror)
if(GHOSTERM_WITH_PLUGIN_CHANNEL)
  add_executable(test_ext_channel_boundary tests/test_ext_channel_boundary.c)
  target_include_directories(test_ext_channel_boundary PRIVATE src)
  target_link_libraries(test_ext_channel_boundary PRIVATE ghosterm_core)
  target_compile_definitions(test_ext_channel_boundary PRIVATE GT_HAVE_PLUGIN_CHANNEL=1)
  target_compile_options(test_ext_channel_boundary PRIVATE -Wall -Wextra -Werror)
  foreach(mode allocation constructor-failure wait-stop draining close-error post-write-stop post-read-stop counter)
    add_test(NAME ext_channel_boundary_${mode} COMMAND test_ext_channel_boundary ${mode})
    set_tests_properties(ext_channel_boundary_${mode} PROPERTIES TIMEOUT 20)
  endforeach()
  foreach(mode invalid copy partial quantum eof close-queued stop failure-stop rejected-fd capacity)
    add_test(NAME ext_channel_${mode} COMMAND test_ext_channel ${mode} ${CMAKE_CURRENT_SOURCE_DIR}/../tmp)
    set_tests_properties(ext_channel_${mode} PROPERTIES TIMEOUT 20)
  endforeach()
  if(SSH2_FOUND)
    target_sources(test_ext_channel PRIVATE tests/ext_channel_dial.c)
    target_include_directories(test_ext_channel PRIVATE src ${SSH2_INCLUDE_DIRS})
    target_compile_definitions(test_ext_channel PRIVATE GT_HAVE_LIBSSH2=1)
    add_test(NAME ext_channel_ssh COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test-plugin-channel.py --binary $<TARGET_FILE:test_ext_channel>)
    set_tests_properties(ext_channel_ssh PROPERTIES TIMEOUT 280)
  else()
    add_test(NAME ext_channel_unsupported_ssh COMMAND test_ext_channel unsupported-ssh ${CMAKE_CURRENT_SOURCE_DIR}/../tmp)
  endif()
else()
  add_test(NAME ext_channel_disabled COMMAND test_ext_channel disabled ${CMAKE_CURRENT_SOURCE_DIR}/../tmp)
endif()
add_executable(test_bridge_source tests/test_bridge_source.c)
target_include_directories(test_bridge_source PRIVATE src ${SSH2_INCLUDE_DIRS})
target_link_libraries(test_bridge_source PRIVATE ghosterm_core)
target_compile_options(test_bridge_source PRIVATE -Wall -Wextra -Werror)
if(SSH2_FOUND)
  target_compile_definitions(test_bridge_source PRIVATE GT_HAVE_LIBSSH2=1)
  add_test(NAME bridge_source_ssh COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test-plugin-session.py --source --binary $<TARGET_FILE:test_bridge_source>)
  set_tests_properties(bridge_source_ssh PROPERTIES TIMEOUT 280)
endif()
foreach(mode roundtrip large empty short literal hardlink max-depth backpressure rpc-progress absolute parent utf8 control c1 long-name long-path deep missing mime limit negotiated-limit root-file symlink symlink-dir directory fifo unsupported ungranted scope revoked waiting open-error swap-open move-open stop-open eintr again read-error stat-error early-eof eof-extra digest stop-read stop-stat stop-send cancel close-pending lost rename replace unlink change-ready change-backpressure grow truncate edit allocation close-error close-root mid-read second-eof second-error queued-change failure-close)
  add_test(NAME bridge_source_${mode} COMMAND test_bridge_source ${mode} ${CMAKE_CURRENT_SOURCE_DIR}/../tmp)
  set_tests_properties(bridge_source_${mode} PROPERTIES TIMEOUT 20)
endforeach()
add_executable(test_bridge_transfer tests/test_bridge_transfer.c)
target_include_directories(test_bridge_transfer PRIVATE src ${SSH2_INCLUDE_DIRS})
target_link_libraries(test_bridge_transfer PRIVATE ghosterm_core)
target_compile_options(test_bridge_transfer PRIVATE -Wall -Wextra -Werror)
if(SSH2_FOUND)
  target_compile_definitions(test_bridge_transfer PRIVATE GT_HAVE_LIBSSH2=1)
  add_test(NAME bridge_transfer_ssh COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test-plugin-session.py --resources --binary $<TARGET_FILE:test_bridge_transfer>)
  set_tests_properties(bridge_transfer_ssh PROPERTIES TIMEOUT 280)
endif()
foreach(mode roundtrip large unknown empty partial snapshot bidirectional queued-cancel cancel receiver-cancel denied unsupported feature lost stop deferred-credit pending-window rpc-progress io native-scope source-incomplete integrity duplicate receipt-conflict snapshot-quota retired destroy-retry stop-write stop-finish bad-success possible-cancel offset overspend gap epoch instance allocation counter exit-before-receipt credit-overflow fairness source-digest peer-id limit duplicate-conflict release-io release-last-io)
  add_test(NAME bridge_transfer_${mode} COMMAND test_bridge_transfer ${mode} ${CMAKE_CURRENT_SOURCE_DIR}/../tmp)
  set_tests_properties(bridge_transfer_${mode} PROPERTIES TIMEOUT 20)
endforeach()
add_executable(test_bridge_resource tests/test_bridge_resource.c)
target_include_directories(test_bridge_resource PRIVATE src)
target_link_libraries(test_bridge_resource PRIVATE ghosterm_core)
target_compile_options(test_bridge_resource PRIVATE -Wall -Wextra -Werror)
foreach(mode roundtrip partial unknown pins integrity offset budget quota scope seal malformed filesystem allocation io-write io-read io-cleanup cleanup-race exhaustion hash crash shared large)
  add_test(NAME bridge_resource_${mode} COMMAND test_bridge_resource ${mode} ${CMAKE_CURRENT_SOURCE_DIR}/../tmp)
  set_tests_properties(bridge_resource_${mode} PROPERTIES TIMEOUT 10)
endforeach()
add_test(NAME bridge_resource_hash_reference COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test-plugin-resource.py --binary $<TARGET_FILE:test_bridge_resource>)
set_tests_properties(bridge_resource_hash_reference PROPERTIES TIMEOUT 60)
add_executable(test_bridge_rpc tests/test_bridge_rpc.c)
target_include_directories(test_bridge_rpc PRIVATE src ${SSH2_INCLUDE_DIRS})
target_link_libraries(test_bridge_rpc PRIVATE ghosterm_core)
target_compile_options(test_bridge_rpc PRIVATE -Wall -Wextra -Werror)
if(SSH2_FOUND)
  target_compile_definitions(test_bridge_rpc PRIVATE GT_HAVE_LIBSSH2=1)
  add_test(NAME bridge_rpc_ssh COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test-plugin-session.py --rpc --binary $<TARGET_FILE:test_bridge_rpc>)
  set_tests_properties(bridge_rpc_ssh PROPERTIES TIMEOUT 90)
endif()
foreach(mode roundtrip full unknown denied missing-cap remote-invalid queued-cancel queued-timeout validation scope cancel late-result lost stop duplicate conflict retired feature allocation credit pending-duplicate cancel-before-take cancel-no-credit credit-overflow overspend gap response-conflict counter select-expiry held-credit incoming-timeout)
  add_test(NAME bridge_rpc_${mode} COMMAND test_bridge_rpc ${mode})
  set_tests_properties(bridge_rpc_${mode} PROPERTIES TIMEOUT 10)
endforeach()
add_executable(test_bridge_session tests/test_bridge_session.c)
target_include_directories(test_bridge_session PRIVATE src ${SSH2_INCLUDE_DIRS})
target_link_libraries(test_bridge_session PRIVATE ghosterm_core)
target_compile_options(test_bridge_session PRIVATE -Wall -Wextra -Werror)
if(SSH2_FOUND)
  target_compile_definitions(test_bridge_session PRIVATE GT_HAVE_LIBSSH2=1)
  add_test(NAME bridge_session_ssh COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test-plugin-session.py --binary $<TARGET_FILE:test_bridge_session>)
  set_tests_properties(bridge_session_ssh PROPERTIES TIMEOUT 90)
endif()
foreach(mode allow deny late-deny consent-time expired clock lost revoke stop stop-before-step allocation bad-nonce bad-scope old-epoch gap replay wrong-digest wrong-cap budget-upgrade bad-accept bad-ready truncated early-message active-message late-read late-write expired-after-allow)
  add_test(NAME bridge_session_${mode} COMMAND test_bridge_session ${mode})
  set_tests_properties(bridge_session_${mode} PROPERTIES TIMEOUT 10)
endforeach()
add_executable(test_bridge_wire tests/test_bridge_wire.c)
target_include_directories(test_bridge_wire PRIVATE src)
target_link_libraries(test_bridge_wire PRIVATE ghosterm_core)
target_compile_options(test_bridge_wire PRIVATE -Wall -Wextra -Werror)
add_test(NAME bridge_wire COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test-plugin-wire.py --binary $<TARGET_FILE:test_bridge_wire>)
set_tests_properties(bridge_wire PROPERTIES TIMEOUT 45)
add_test(NAME bridge_wire_generated COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/generate-plugin-wire.py --check
  --output ${CMAKE_CURRENT_SOURCE_DIR}/src/extension/wire-schema.inc)
add_executable(test_bridge_route tests/test_bridge_route.c)
target_include_directories(test_bridge_route PRIVATE src ${SSH2_INCLUDE_DIRS})
target_link_libraries(test_bridge_route PRIVATE ghosterm_core)
target_compile_options(test_bridge_route PRIVATE -Wall -Wextra -Werror)
if(SSH2_FOUND)
  target_compile_definitions(test_bridge_route PRIVATE GT_HAVE_LIBSSH2=1)
  add_test(NAME bridge_route_e2e COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test-plugin-route.py --binary $<TARGET_FILE:test_bridge_route>)
  set_tests_properties(bridge_route_e2e PROPERTIES TIMEOUT 90)
else()
  add_test(NAME bridge_route_unsupported COMMAND test_bridge_route unsupported)
endif()
add_executable(test_bridge_ssh tests/test_bridge_ssh.c)
target_include_directories(test_bridge_ssh PRIVATE src)
target_link_libraries(test_bridge_ssh PRIVATE ghosterm_core)
target_compile_options(test_bridge_ssh PRIVATE -Wall -Wextra -Werror)
if(SSH2_FOUND)
  target_compile_definitions(test_bridge_ssh PRIVATE GT_HAVE_LIBSSH2=1)
  add_test(NAME bridge_ssh_e2e COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test-plugin-ssh-stream.py --binary $<TARGET_FILE:test_bridge_ssh>)
  set_tests_properties(bridge_ssh_e2e PROPERTIES TIMEOUT 90)
  add_test(NAME bridge_exec_e2e COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test-plugin-ssh-stream.py --binary $<TARGET_FILE:test_bridge_ssh> --legacy)
  set_tests_properties(bridge_exec_e2e PROPERTIES TIMEOUT 90)
else()
  add_test(NAME bridge_ssh_unsupported COMMAND test_bridge_ssh unsupported)
endif()
add_executable(test_bridge_stream tests/test_bridge_stream.c)
target_include_directories(test_bridge_stream PRIVATE src)
target_link_libraries(test_bridge_stream PRIVATE ghosterm_core)
target_compile_options(test_bridge_stream PRIVATE -Wall -Wextra -Werror)
foreach(mode pipe socket codec limits allocation stop deadline lifecycle invalid)
  add_test(NAME bridge_stream_${mode} COMMAND test_bridge_stream ${mode})
  set_tests_properties(bridge_stream_${mode} PROPERTIES TIMEOUT 15)
endforeach()
add_test(NAME public_headers
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/check-public-headers.py
                 --cc ${CMAKE_C_COMPILER})

add_executable(test_known_hosts_snapshot tests/test_known_hosts_snapshot.c)
target_link_libraries(test_known_hosts_snapshot PRIVATE ghosterm_core)
target_compile_options(test_known_hosts_snapshot PRIVATE -Wall -Wextra -Werror)
add_test(NAME known_hosts_snapshot COMMAND test_known_hosts_snapshot)
add_executable(test_known_hosts_store tests/test_known_hosts_store.c)
target_link_libraries(test_known_hosts_store PRIVATE ghosterm_core)
target_compile_options(test_known_hosts_store PRIVATE -Wall -Wextra -Werror)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../tmp")
add_test(NAME known_hosts_store COMMAND test_known_hosts_store "${CMAKE_CURRENT_SOURCE_DIR}/../tmp")

add_executable(test_sftp_remove_stub tests/test_sftp_remove_stub.c)
target_include_directories(test_sftp_remove_stub PRIVATE src)
target_link_libraries(test_sftp_remove_stub PRIVATE ghosterm_core)
target_compile_options(test_sftp_remove_stub PRIVATE -Wall -Wextra -Werror)
add_test(NAME sftp_remove_unsupported COMMAND test_sftp_remove_stub)

add_executable(test_config tests/test_config.c)
target_link_libraries(test_config PRIVATE ghosterm_core)
add_test(NAME sshconfig_golden
         COMMAND test_config ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures)

add_executable(test_config_read tests/test_config_read.c)
target_link_libraries(test_config_read PRIVATE ghosterm_core)
add_test(NAME sshconfig_read_error
         COMMAND test_config_read ${CMAKE_CURRENT_SOURCE_DIR}/../tmp)

add_executable(test_config_metadata tests/test_config_metadata.c)
target_link_libraries(test_config_metadata PRIVATE ghosterm_core)
target_compile_options(test_config_metadata PRIVATE -Wall -Wextra -Werror)
add_test(NAME sshconfig_metadata COMMAND test_config_metadata ${CMAKE_CURRENT_SOURCE_DIR}/../tmp)

add_executable(test_config_identities tests/test_config_identities.c)
target_link_libraries(test_config_identities PRIVATE ghosterm_core)
target_compile_options(test_config_identities PRIVATE -Wall -Wextra -Werror)
add_test(NAME sshconfig_identities COMMAND test_config_identities ${CMAKE_CURRENT_SOURCE_DIR}/../tmp)

add_executable(test_config_rename tests/test_config_rename.c)
target_link_libraries(test_config_rename PRIVATE ghosterm_core)
target_compile_options(test_config_rename PRIVATE -Wall -Wextra -Werror)
add_test(NAME sshconfig_rename COMMAND test_config_rename ${CMAKE_CURRENT_SOURCE_DIR}/../tmp)
add_executable(test_config_remove tests/test_config_remove.c)
target_link_libraries(test_config_remove PRIVATE ghosterm_core)
target_compile_options(test_config_remove PRIVATE -Wall -Wextra -Werror)
add_test(NAME sshconfig_remove COMMAND test_config_remove ${CMAKE_CURRENT_SOURCE_DIR}/../tmp)

add_executable(test_connection_config tests/test_connection_config.c)
target_link_libraries(test_connection_config PRIVATE ghosterm_core)
target_compile_options(test_connection_config PRIVATE -Wall -Wextra -Werror)
add_test(NAME connection_config COMMAND test_connection_config ${CMAKE_CURRENT_SOURCE_DIR}/../tmp)

add_executable(test_api tests/test_api.c)
target_link_libraries(test_api PRIVATE ghosterm_core)
add_test(NAME api_smoke COMMAND test_api)

add_executable(test_transport tests/test_transport.c)
target_link_libraries(test_transport PRIVATE ghosterm_core)
# 传工程根的 tmp/ 进去：登录 shell 那节要 mkdtemp 一个干净 HOME、还要在里面
# **执行**两个假 shell 脚本。AGENTS.md 规定临时文件只落 tmp/，且不从系统 /tmp
# 执行（那里起的进程会被判死，exit 137）。tmp/ 是 git 忽略的，新克隆里没有。
file(MAKE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../tmp)
get_filename_component(GHOSTERM_TEST_TMP ${CMAKE_CURRENT_SOURCE_DIR}/../tmp REALPATH)
add_test(NAME pty_transport
         COMMAND test_transport ${CMAKE_CURRENT_SOURCE_DIR}/../tmp)
# 加超时：这套断言里有「写绝不无限等」——若回归成 poll(-1)，套件会一直卡着。
# 有超时就会**显式失败**而不是把整条闸门挂死（变异 P4 靠它变红）。
set_tests_properties(pty_transport PROPERTIES TIMEOUT 300)

add_executable(test_pty_inheritance tests/test_pty_inheritance.c)
target_link_libraries(test_pty_inheritance PRIVATE ghosterm_core)
target_compile_options(test_pty_inheritance PRIVATE -Wall -Wextra -Werror)
add_test(NAME pty_inheritance COMMAND test_pty_inheritance)
set_tests_properties(pty_inheritance PROPERTIES TIMEOUT 15)

add_executable(test_pty_dimensions tests/test_pty_dimensions.c)
target_include_directories(test_pty_dimensions PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_pty_dimensions PRIVATE ghosterm_core)
target_compile_options(test_pty_dimensions PRIVATE -Wall -Wextra -Werror)
foreach(mode flow defaults admission)
  add_test(NAME pty_dimensions_${mode} COMMAND test_pty_dimensions ${mode})
  set_tests_properties(pty_dimensions_${mode} PROPERTIES TIMEOUT 15)
endforeach()

add_executable(test_sftp_open tests/test_sftp_open.c)
target_include_directories(test_sftp_open PRIVATE src)
target_link_libraries(test_sftp_open PRIVATE ghosterm_core)
target_compile_options(test_sftp_open PRIVATE -Wall -Wextra -Werror)
if(SSH2_FOUND)
  add_test(NAME sftp_open_e2e COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/ssh_e2e.sh $<TARGET_FILE:test_sftp_open>)
  add_executable(test_sftp_preview tests/test_sftp_preview.c)
  target_include_directories(test_sftp_preview PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(test_sftp_preview PRIVATE ghosterm_core ${CMAKE_DL_LIBS})
  target_compile_options(test_sftp_preview PRIVATE -Wall -Wextra -Werror)
  add_test(NAME sftp_preview_e2e COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/ssh_e2e.sh $<TARGET_FILE:test_sftp_preview>)
  set_tests_properties(sftp_preview_e2e PROPERTIES TIMEOUT 30)
  add_executable(test_sftp_browse tests/test_sftp_browse.c)
  target_include_directories(test_sftp_browse PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(test_sftp_browse PRIVATE ghosterm_core ${CMAKE_DL_LIBS})
  target_compile_options(test_sftp_browse PRIVATE -Wall -Wextra -Werror)
  add_test(NAME sftp_browse_e2e COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/ssh_e2e.sh $<TARGET_FILE:test_sftp_browse>)
  set_tests_properties(sftp_browse_e2e PROPERTIES TIMEOUT 30)
  add_executable(test_sftp_receive tests/test_sftp_receive.c)
  target_include_directories(test_sftp_receive PRIVATE src)
  target_compile_definitions(test_sftp_receive PRIVATE GT_HAVE_LIBSSH2=1)
  target_link_libraries(test_sftp_receive PRIVATE ghosterm_core)
  target_compile_options(test_sftp_receive PRIVATE -Wall -Wextra -Werror)
  add_test(NAME sftp_receive_e2e COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/ssh_e2e.sh $<TARGET_FILE:test_sftp_receive>)
  set_tests_properties(sftp_receive_e2e PROPERTIES TIMEOUT 60)
  add_executable(test_sftp_remove tests/test_sftp_remove.c)
  target_include_directories(test_sftp_remove PRIVATE src)
  target_compile_definitions(test_sftp_remove PRIVATE GT_HAVE_LIBSSH2=1)
  target_link_libraries(test_sftp_remove PRIVATE ghosterm_core)
  target_compile_options(test_sftp_remove PRIVATE -Wall -Wextra -Werror)
  add_test(NAME sftp_remove_e2e COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/ssh_e2e.sh $<TARGET_FILE:test_sftp_remove>)
  set_tests_properties(sftp_remove_e2e PROPERTIES TIMEOUT 60)
  add_executable(test_sftp_mkdir tests/test_sftp_mkdir.c)
  target_include_directories(test_sftp_mkdir PRIVATE src)
  target_compile_definitions(test_sftp_mkdir PRIVATE GT_HAVE_LIBSSH2=1)
  target_link_libraries(test_sftp_mkdir PRIVATE ghosterm_core)
  target_compile_options(test_sftp_mkdir PRIVATE -Wall -Wextra -Werror)
  add_test(NAME sftp_mkdir_e2e COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/ssh_e2e.sh $<TARGET_FILE:test_sftp_mkdir>)
  set_tests_properties(sftp_mkdir_e2e PROPERTIES TIMEOUT 60)
  add_executable(test_sftp_publish tests/test_sftp_publish.c)
  target_include_directories(test_sftp_publish PRIVATE src)
  target_compile_definitions(test_sftp_publish PRIVATE GT_HAVE_LIBSSH2=1)
  target_link_libraries(test_sftp_publish PRIVATE ghosterm_core)
  target_compile_options(test_sftp_publish PRIVATE -Wall -Wextra -Werror)
  add_test(NAME sftp_publish_e2e COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/ssh_e2e.sh $<TARGET_FILE:test_sftp_publish>)
  set_tests_properties(sftp_publish_e2e PROPERTIES TIMEOUT 60)
  add_executable(test_sftp_create tests/test_sftp_create.c)
  target_include_directories(test_sftp_create PRIVATE src)
  target_link_libraries(test_sftp_create PRIVATE ghosterm_core)
  target_compile_options(test_sftp_create PRIVATE -Wall -Wextra -Werror)
  add_test(NAME sftp_create_e2e COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/ssh_e2e.sh $<TARGET_FILE:test_sftp_create>)
  set_tests_properties(sftp_create_e2e PROPERTIES TIMEOUT 60)
  add_executable(test_sftp_transfer tests/test_sftp_transfer.c)
  target_include_directories(test_sftp_transfer PRIVATE src)
  target_compile_definitions(test_sftp_transfer PRIVATE GT_HAVE_LIBSSH2=1)
  target_link_libraries(test_sftp_transfer PRIVATE ghosterm_core)
  target_compile_options(test_sftp_transfer PRIVATE -Wall -Wextra -Werror)
  add_test(NAME sftp_transfer_e2e COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/ssh_e2e.sh $<TARGET_FILE:test_sftp_transfer>)
  set_tests_properties(sftp_transfer_e2e PROPERTIES TIMEOUT 60)
  add_executable(test_sftp_submit tests/test_sftp_submit.c)
  target_include_directories(test_sftp_submit PRIVATE src)
  target_compile_definitions(test_sftp_submit PRIVATE GT_HAVE_LIBSSH2=1)
  target_link_libraries(test_sftp_submit PRIVATE ghosterm_core)
  target_compile_options(test_sftp_submit PRIVATE -Wall -Wextra -Werror -fsanitize=address -fno-omit-frame-pointer)
  target_link_options(test_sftp_submit PRIVATE -fsanitize=address)
  add_test(NAME sftp_submit_e2e COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/ssh_e2e.sh $<TARGET_FILE:test_sftp_submit>)
  # libssh2's pre-SFTP handshake allocations are separately tracked; this test
  # specifically checks use-after-free in submit_job, not whole-library leaks.
  set_tests_properties(sftp_submit_e2e PROPERTIES TIMEOUT 30 ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")


endif()

if(GHOSTERM_WITH_LIBSSH2 AND SSH2_FOUND)
  add_executable(test_exec_boundary tests/test_exec_boundary.c)
  target_include_directories(test_exec_boundary PRIVATE src ${SSH2_INCLUDE_DIRS})
  target_compile_definitions(test_exec_boundary PRIVATE GT_HAVE_LIBSSH2=1)
  target_link_libraries(test_exec_boundary PRIVATE ghosterm_core)
  target_compile_options(test_exec_boundary PRIVATE -Wall -Wextra -Werror)
  add_test(NAME ssh_exec_boundary COMMAND test_exec_boundary)
  add_executable(test_ssh_read tests/test_ssh_read.c)
  target_include_directories(test_ssh_read PRIVATE src ${SSH2_INCLUDE_DIRS})
  target_compile_definitions(test_ssh_read PRIVATE GT_HAVE_LIBSSH2=1)
  target_link_libraries(test_ssh_read PRIVATE ghosterm_core)
  target_compile_options(test_ssh_read PRIVATE -Wall -Wextra -Werror)
  add_test(NAME ssh_shell_read COMMAND test_ssh_read)
endif()

add_executable(test_ssh tests/test_ssh.c)
target_link_libraries(test_ssh PRIVATE ghosterm_core)
if(SSH2_FOUND)
  target_include_directories(test_ssh PRIVATE src)
  target_compile_definitions(test_ssh PRIVATE GT_HAVE_LIBSSH2=1)
endif()
add_test(NAME ssh_e2e
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/ssh_e2e.sh
                 $<TARGET_FILE:test_ssh>)

# known_hosts 读侧语义（同一条 e2e 脚手架，另起一个二进制——两者验的东西
# 不同：test_ssh 验连接/认证/三态，test_known_hosts 专验「已知主机不再问用户」）
add_executable(test_known_hosts tests/test_known_hosts.c)
target_link_libraries(test_known_hosts PRIVATE ghosterm_core)
add_test(NAME known_hosts_e2e
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/ssh_e2e.sh
                 $<TARGET_FILE:test_known_hosts>)

# known_hosts 的算法覆盖：起一个只发 ECDSA 主机密钥的 sshd
# （常规 e2e 用 ed25519，盖不到「按 ssh 前缀过滤会把 ECDSA 整条跳过」那条路径）
add_test(NAME known_hosts_ecdsa
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/ecdsa_e2e.sh
                 $<TARGET_FILE:test_known_hosts>)

# 编辑 API 的写回对拍（夹具 writeback.json 为唯一事实来源）
add_executable(gtb tests/gtb.c)
target_link_libraries(gtb PRIVATE ghosterm_core)
add_test(NAME sshconfig_writeback
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/writeback_check.py
                 $<TARGET_FILE:gtb>
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures)

add_executable(test_thread_options tests/test_thread_options.c)
target_include_directories(test_thread_options PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_thread_options PRIVATE Threads::Threads)
target_compile_options(test_thread_options PRIVATE -Wall -Wextra -Werror)
foreach(mode attributes invalid live)
  add_test(NAME thread_options_${mode} COMMAND test_thread_options ${mode})
  set_tests_properties(thread_options_${mode} PROPERTIES TIMEOUT 10)
endforeach()

add_executable(test_pty_budget tests/test_pty_budget.c)
target_include_directories(test_pty_budget PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_pty_budget PRIVATE ghosterm_core)
target_compile_options(test_pty_budget PRIVATE -Wall -Wextra -Werror)
foreach(mode flow failures pressure invalid thread)
  add_test(NAME pty_budget_${mode} COMMAND test_pty_budget ${mode})
  set_tests_properties(pty_budget_${mode} PROPERTIES TIMEOUT 15)
endforeach()

add_executable(test_ssh_connector tests/test_ssh_connector.c)
target_include_directories(test_ssh_connector PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_ssh_connector PRIVATE ghosterm_core)
target_compile_options(test_ssh_connector PRIVATE -Wall -Wextra -Werror)
if(GHOSTERM_WITH_LIBSSH2 AND SSH2_FOUND)
  foreach(mode deadline cancel clock real-deadline allocation)
    add_test(NAME ssh_connector_${mode} COMMAND test_ssh_connector ${mode})
    set_tests_properties(ssh_connector_${mode} PROPERTIES TIMEOUT 5)
  endforeach()
  add_library(ssh_connector_erase_object OBJECT src/ssh.c)
  target_include_directories(ssh_connector_erase_object PRIVATE include src ${SSH2_INCLUDE_DIRS})
  target_compile_definitions(ssh_connector_erase_object PRIVATE GT_HAVE_LIBSSH2=1 gt__secure_erase=gt_connect_test_erase)
  target_compile_options(ssh_connector_erase_object PRIVATE -Wall -Wextra -Werror)
  add_executable(test_ssh_connector_erase tests/test_ssh_connector.c $<TARGET_OBJECTS:ssh_connector_erase_object>)
  target_include_directories(test_ssh_connector_erase PRIVATE src)
  target_compile_definitions(test_ssh_connector_erase PRIVATE GT_CONNECT_ERASE_TEST=1)
  target_compile_options(test_ssh_connector_erase PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(test_ssh_connector_erase PRIVATE ghosterm_core)
  add_test(NAME ssh_connector_secrets COMMAND test_ssh_connector_erase secrets)
  add_test(NAME ssh_connector_e2e COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/ssh_e2e.sh $<TARGET_FILE:test_ssh_connector>)
  set_tests_properties(ssh_connector_e2e PROPERTIES TIMEOUT 30)
else()
  add_test(NAME ssh_connector_unsupported COMMAND test_ssh_connector unsupported)
endif()

add_executable(test_ssh_auth_bridge tests/test_ssh_auth_bridge.c)
target_include_directories(test_ssh_auth_bridge PRIVATE src)
target_compile_options(test_ssh_auth_bridge PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_ssh_auth_bridge PRIVATE ghosterm_core)
add_test(NAME ssh_auth_bridge COMMAND test_ssh_auth_bridge)
set_tests_properties(ssh_auth_bridge PROPERTIES TIMEOUT 10)

add_executable(test_tmux tests/test_tmux.c)
target_include_directories(test_tmux PRIVATE src)
target_compile_options(test_tmux PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_tmux PRIVATE ghosterm_core)
add_test(NAME tmux_discovery COMMAND ${Python3_EXECUTABLE}
         ${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/test-tmux-discovery.py $<TARGET_FILE:test_tmux>)
set_tests_properties(tmux_discovery PROPERTIES TIMEOUT 40)
if(SSH2_FOUND)
  add_test(NAME tmux_login_ssh COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/ssh_e2e.sh
           ${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/test-tmux-login-ssh.py)
  set_tests_properties(tmux_login_ssh PROPERTIES TIMEOUT 60
      ENVIRONMENT "GT_TMUX_TEST_BINARY=$<TARGET_FILE:test_tmux>")
endif()
add_test(NAME tmux_command COMMAND test_tmux command)
set_tests_properties(tmux_command PROPERTIES TIMEOUT 15)

add_test(NAME tmux_adversarial COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test-tmux.py --fake --binary $<TARGET_FILE:test_tmux>)
set_tests_properties(tmux_adversarial PROPERTIES TIMEOUT 30)
endif()
