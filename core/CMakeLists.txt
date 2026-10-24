cmake_minimum_required(VERSION 3.19)
project(ghosterm_core C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(GHOSTERM_CLIENT_SOURCES
  src/base/secure-random.c
  src/ipc/wire.c
  src/ipc/writer.c
  src/ipc/connection.c
  src/ipc/socket-stream.c
  src/ipc/unix-endpoint.c
  src/ipc/control-info.c
  src/ipc/bootstrap-wire.c
  src/ipc/client-bootstrap.c
  src/ipc/control-reply-wire.c
  src/ipc/control-client.c
  src/session/create-spec.c
  src/ipc/data-wire.c
  src/ipc/user-wire.c
  src/ipc/attachment-client.c
  src/ipc/session-client.c
  src/ipc/local-session-provider.c
)
add_library(ghosterm_client_objects OBJECT ${GHOSTERM_CLIENT_SOURCES})
target_include_directories(ghosterm_client_objects PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(ghosterm_client_objects PRIVATE -Wall -Wextra -Werror)
# A standalone client target must not inherit libssh2, PTY, host runtime or engine linkage.
add_library(ghosterm_client_protocol STATIC $<TARGET_OBJECTS:ghosterm_client_objects>)
target_include_directories(ghosterm_client_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(ghosterm_core STATIC
  $<TARGET_OBJECTS:ghosterm_client_objects>
  src/ipc/tokens.c
  src/ipc/handshake.c
  src/ipc/control.c
  src/ipc/server.c
  src/ipc/attachment-owner.c
  src/ipc/user-dispatcher.c
  src/ipc/data-router.c
  src/session/creation.c
  src/session/deploy.c
  src/session/client-api.c
  src/session/setup-api.c
  src/session/runtime.c
  src/core.c
  src/config.c
  src/transport.c
  src/ipc/attachment-peer.c
  src/session/state-owner.c
  src/session/command-status.c
  src/session/program-status.c
  src/session/worker.c
  src/session/manager.c
  src/session/checkpoint-task.c
  src/session/attachment.c
  src/session/restore-stream.c
  src/io/pty_posix.c
  src/io/ssh_shell.c
  src/ssh.c
  src/ipc/ssh-link.c
  src/ipc/ssh-session-provider.c
  src/sftp.c
)
target_include_directories(ghosterm_core
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_options(ghosterm_core PRIVATE -Wall -Wextra -Werror)
target_link_libraries(ghosterm_core PUBLIC Threads::Threads)

# SSH/SFTP 后端（libssh2）——开关是**显式的，不静默降级**（铁律 #6）。
# 默认 ON：探测到 libssh2 就编入；探测不到则 GT_HAVE_LIBSSH2 不定义，
# 核心退化为「所有 SSH 入口显式 GT_ERR_UNSUPPORTED」，闸门仍可离线跑。
option(GHOSTERM_WITH_LIBSSH2 "Build the libssh2 SSH/SFTP backend" ON)
if(GHOSTERM_WITH_LIBSSH2)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(SSH2 QUIET libssh2)
  endif()
  if(SSH2_FOUND)
    target_compile_definitions(ghosterm_core PRIVATE GT_HAVE_LIBSSH2=1)
    target_include_directories(ghosterm_core PRIVATE ${SSH2_INCLUDE_DIRS})
    target_link_libraries(ghosterm_core PUBLIC ${SSH2_LIBRARIES})
    target_link_directories(ghosterm_core PUBLIC ${SSH2_LIBRARY_DIRS})
    message(STATUS "ghosterm_core: libssh2 ${SSH2_VERSION} 已编入 SSH/SFTP 后端")
  else()
    message(STATUS "ghosterm_core: 未探测到 libssh2 —— SSH 入口将返回 GT_ERR_UNSUPPORTED")
  endif()
endif()



enable_testing()
add_executable(test_client_bootstrap tests/test_client_bootstrap.c)
target_include_directories(test_client_bootstrap PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_client_bootstrap PRIVATE ghosterm_client_protocol)
target_compile_options(test_client_bootstrap PRIVATE -Wall -Wextra -Werror)
foreach(mode management data rejections malformed deadlines control control-bad control-timeout heartbeat allocation)
  add_test(NAME client_bootstrap_${mode} COMMAND test_client_bootstrap ${mode})
  set_tests_properties(client_bootstrap_${mode} PROPERTIES TIMEOUT 10)
endforeach()
add_test(NAME client_bootstrap_dependencies COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/check-attachment-client-deps.py $<TARGET_FILE:test_client_bootstrap>)
add_executable(test_attachment_client tests/test_attachment_client.c)
target_include_directories(test_attachment_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_attachment_client PRIVATE ghosterm_client_protocol)
target_compile_options(test_attachment_client PRIVATE -Wall -Wextra -Werror)
foreach(mode flow readonly commands malformed applied deadlines error invalid backpressure replies clocks encoded process)
  add_test(NAME attachment_client_${mode} COMMAND test_attachment_client ${mode})
  set_tests_properties(attachment_client_${mode} PROPERTIES TIMEOUT 10)
endforeach()
if(APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_library(test_endpoint_peer_object OBJECT src/ipc/unix-endpoint.c)
  target_include_directories(test_endpoint_peer_object PRIVATE src include)
  target_compile_options(test_endpoint_peer_object PRIVATE -Wall -Wextra -Werror)
  target_compile_definitions(test_endpoint_peer_object PRIVATE fcntl=gt_test_fcntl accept=gt_test_accept)
  if(APPLE)
    target_compile_definitions(test_endpoint_peer_object PRIVATE getpeereid=gt_test_getpeereid)
  else()
    target_compile_definitions(test_endpoint_peer_object PRIVATE getsockopt=gt_test_getsockopt)
  endif()
  add_executable(test_endpoint_peer tests/test_endpoint_peer.c $<TARGET_OBJECTS:test_endpoint_peer_object>)
  target_include_directories(test_endpoint_peer PRIVATE src include)
  target_compile_options(test_endpoint_peer PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME attachment_client_dependencies COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/check-attachment-client-deps.py $<TARGET_FILE:test_attachment_client>)

add_test(NAME public_headers
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/check-public-headers.py
                 --cc ${CMAKE_C_COMPILER})

add_executable(test_config tests/test_config.c)
target_link_libraries(test_config PRIVATE ghosterm_core)
add_test(NAME sshconfig_golden
         COMMAND test_config ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures)

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

# SSH 后端 e2e（真 sshd，环回）；无 sshd/未编 libssh2 时脚本显式 SKIP
add_executable(test_deploy tests/test_deploy.c)
target_include_directories(test_deploy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(test_deploy PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_deploy PRIVATE ghosterm_core)
add_test(NAME session_bundle_catalog COMMAND test_deploy --catalog ${CMAKE_CURRENT_SOURCE_DIR}/../tmp)
if(GHOSTERM_WITH_LIBSSH2 AND SSH2_FOUND)
  add_library(deploy_test_io OBJECT src/session/deploy.c)
  target_compile_definitions(deploy_test_io PRIVATE GT_HAVE_LIBSSH2=1 libssh2_sftp_open_ex=gt_deploy_test_open libssh2_sftp_close_handle=gt_deploy_test_close)
  target_include_directories(deploy_test_io PRIVATE src include ${SSH2_INCLUDE_DIRS})
  target_compile_options(deploy_test_io PRIVATE -Wall -Wextra -Werror)
  add_executable(test_deploy_shim tests/test_deploy.c tests/deploy-io.c $<TARGET_OBJECTS:deploy_test_io>)
  target_include_directories(test_deploy_shim PRIVATE src ${SSH2_INCLUDE_DIRS})
  target_compile_definitions(test_deploy_shim PRIVATE GT_DEPLOY_SHIM=1)
  target_compile_options(test_deploy_shim PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(test_deploy_shim PRIVATE ghosterm_core)
else()
  add_test(NAME session_deploy_unsupported COMMAND test_deploy --unsupported)
endif()

add_executable(test_ssh_link tests/test_ssh_link.c)
target_include_directories(test_ssh_link PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(test_ssh_link PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_ssh_link PRIVATE ghosterm_core)
if(GHOSTERM_WITH_LIBSSH2 AND SSH2_FOUND)
  add_test(NAME ssh_link_e2e COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/ssh_e2e.sh $<TARGET_FILE:test_ssh_link>)
  set_tests_properties(ssh_link_e2e PROPERTIES TIMEOUT 45)
else()
  add_test(NAME ssh_link_unsupported COMMAND test_ssh_link --unsupported)
endif()
add_executable(test_ssh tests/test_ssh.c)
target_link_libraries(test_ssh PRIVATE ghosterm_core)
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

add_executable(test_ipc_wire tests/test_ipc_wire.c)
target_include_directories(test_ipc_wire PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_ipc_wire PRIVATE ghosterm_core)
target_compile_options(test_ipc_wire PRIVATE -Wall -Wextra -Werror)
foreach(mode golden flow malformed limits eof invalid empty binary)
  add_test(NAME ipc_wire_${mode} COMMAND test_ipc_wire ${mode})
  set_tests_properties(ipc_wire_${mode} PROPERTIES TIMEOUT 10)
endforeach()

add_executable(test_ipc_writer tests/test_ipc_writer.c)
target_include_directories(test_ipc_writer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_ipc_writer PRIVATE ghosterm_core)
target_compile_options(test_ipc_writer PRIVATE -Wall -Wextra -Werror)
foreach(mode short replacement invalid cancel empty)
  add_test(NAME ipc_writer_${mode} COMMAND test_ipc_writer ${mode})
  set_tests_properties(ipc_writer_${mode} PROPERTIES TIMEOUT 10)
endforeach()

add_executable(test_unix_endpoint tests/test_unix_endpoint.c)
target_include_directories(test_unix_endpoint PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_unix_endpoint PRIVATE ghosterm_core)
target_compile_options(test_unix_endpoint PRIVATE -Wall -Wextra -Werror)
if(TARGET test_endpoint_peer)
  foreach(mode configuration identity capacity)
    add_test(NAME endpoint_peer_${mode} COMMAND test_endpoint_peer ${mode} ${GHOSTERM_TEST_TMP})
    set_tests_properties(endpoint_peer_${mode} PROPERTIES TIMEOUT 10)
  endforeach()
endif()
foreach(mode flow locks stale live replacement permissions symlinks nodes lock-nodes peer allocation directory-replaced invalid inheritance nested)
  add_test(NAME unix_endpoint_${mode} COMMAND test_unix_endpoint ${mode} ${GHOSTERM_TEST_TMP})
  set_tests_properties(unix_endpoint_${mode} PROPERTIES TIMEOUT 15)
endforeach()

add_executable(test_ipc_connection tests/test_ipc_connection.c)
target_include_directories(test_ipc_connection PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_ipc_connection PRIVATE ghosterm_core)
target_compile_options(test_ipc_connection PRIVATE -Wall -Wextra -Werror)
foreach(mode flow receive-deadline send-deadline fairness buffers invalid-io cancel clock allocation protocol invalid real-flow real-pressure real-eof sigpipe hup erasure)
  add_test(NAME ipc_connection_${mode} COMMAND test_ipc_connection ${mode} ${GHOSTERM_TEST_TMP})
  set_tests_properties(ipc_connection_${mode} PROPERTIES TIMEOUT 15)
endforeach()

add_executable(test_ipc_tokens tests/test_ipc_tokens.c)
target_include_directories(test_ipc_tokens PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_ipc_tokens PRIVATE ghosterm_core)
target_compile_options(test_ipc_tokens PRIVATE -Wall -Wextra -Werror)
foreach(mode lifecycle binding capacity replay revocation entropy clock allocation os-random invalid idle-expiry)
  add_test(NAME ipc_tokens_${mode} COMMAND test_ipc_tokens ${mode})
  set_tests_properties(ipc_tokens_${mode} PROPERTIES TIMEOUT 15)
endforeach()

add_executable(test_ipc_handshake tests/test_ipc_handshake.c)
target_include_directories(test_ipc_handshake PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_ipc_handshake PRIVATE ghosterm_core)
target_compile_options(test_ipc_handshake PRIVATE -Wall -Wextra -Werror)
foreach(mode management data native compatibility features instance challenge disconnect stale unclaimed expired timeout clock entropy codec transaction-deadline schema invalid)
  add_test(NAME ipc_handshake_${mode} COMMAND test_ipc_handshake ${mode})
  set_tests_properties(ipc_handshake_${mode} PROPERTIES TIMEOUT 15)
endforeach()

add_executable(test_ipc_server tests/test_ipc_server.c)
target_include_directories(test_ipc_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_ipc_server PRIVATE ghosterm_core)
target_compile_options(test_ipc_server PRIVATE -Wall -Wextra -Werror)
foreach(mode management unsupported request-id malformed capacity idle handshake-timeout clock allocation issuer expiry data stale codec fairness shutdown kill invalid missing ended data-disabled data-lease data-failure full-list create create-lost create-malformed create-expiry create-principal create-disabled create-config create-codec)
  add_test(NAME ipc_server_${mode} COMMAND test_ipc_server ${mode} ${GHOSTERM_TEST_TMP})
  set_tests_properties(ipc_server_${mode} PROPERTIES TIMEOUT 15)
endforeach()

add_executable(test_creation tests/test_creation.c)
target_include_directories(test_creation PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_creation PRIVATE ghosterm_core)
target_compile_options(test_creation PRIVATE -Wall -Wextra -Werror)
foreach(mode lifecycle replay expiry failures asynchronous clock allocation codec invalid launch-failures limits unsupported registration)
  add_test(NAME creation_${mode} COMMAND test_creation ${mode} ${GHOSTERM_TEST_TMP})
  set_tests_properties(creation_${mode} PROPERTIES TIMEOUT 15)
endforeach()

add_executable(test_data_wire tests/test_data_wire.c)
target_include_directories(test_data_wire PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_data_wire PRIVATE ghosterm_core)
target_compile_options(test_data_wire PRIVATE -Wall -Wextra -Werror)
foreach(mode vectors events malformed terminal limits exact)
  add_test(NAME ipc_data_wire_${mode} COMMAND test_data_wire ${mode})
  set_tests_properties(ipc_data_wire_${mode} PROPERTIES TIMEOUT 15)
endforeach()

add_executable(test_user_wire tests/test_user_wire.c)
target_include_directories(test_user_wire PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_user_wire PRIVATE ghosterm_core)
target_compile_options(test_user_wire PRIVATE -Wall -Wextra -Werror)
foreach(mode requests malformed replies limits)
  add_test(NAME ipc_user_wire_${mode} COMMAND test_user_wire ${mode})
  set_tests_properties(ipc_user_wire_${mode} PROPERTIES TIMEOUT 10)
endforeach()

add_executable(test_session_runtime tests/test_session_runtime.c)
target_include_directories(test_session_runtime PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_session_runtime PRIVATE ghosterm_core)
target_compile_options(test_session_runtime PRIVATE -Wall -Wextra -Werror)
foreach(mode lifecycle stream pending limits failures allocation concurrency invalid service attach-flow attach-expiry attach-issuer attach-stale attach-failures attach-capacity attach-malformed attach-codec attach-config attach-close attach-lost attach-lease user-policy user-stale-queue user-readonly user-dimensions wire-flow wire-disconnected wire-pressure wire-failure wire-malformed wire-config wire-backpressure final-retained final-unavailable final-publication final-errors route-flow route-final route-cancel route-error route-timeout route-unavailable route-crossing route-config route-malformed route-clock route-error-deadline route-invalid-blob route-capture-pending route-capture-ending)
  add_test(NAME session_runtime_${mode} COMMAND test_session_runtime ${mode} ${GHOSTERM_TEST_TMP})
  set_tests_properties(session_runtime_${mode} PROPERTIES TIMEOUT 20)
endforeach()

add_executable(test_state_owner tests/test_state_owner.c)
target_include_directories(test_state_owner PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_state_owner PRIVATE ghosterm_core)
target_compile_options(test_state_owner PRIVATE -Wall -Wextra -Werror)
add_test(NAME session_state_owner COMMAND test_state_owner)
add_test(NAME session_state_owner_capture_failure COMMAND test_state_owner control)
set_tests_properties(session_state_owner session_state_owner_capture_failure
                     PROPERTIES TIMEOUT 10)

add_executable(test_session_worker tests/test_session_worker.c)
target_include_directories(test_session_worker PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_session_worker PRIVATE ghosterm_core)
target_compile_options(test_session_worker PRIVATE -Wall -Wextra -Werror)

add_executable(test_worker_final tests/test_worker_final.c)
target_include_directories(test_worker_final PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_worker_final PRIVATE ghosterm_core)
target_compile_options(test_worker_final PRIVATE -Wall -Wextra -Werror)
foreach(mode flow zero failure allocation null startup backend invalid-state stop disabled)
  add_test(NAME worker_final_${mode} COMMAND test_worker_final ${mode})
  set_tests_properties(worker_final_${mode} PROPERTIES TIMEOUT 10)
endforeach()

add_executable(test_worker_authorization tests/test_worker_authorization.c)
target_include_directories(test_worker_authorization PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_worker_authorization PRIVATE ghosterm_core)
target_compile_options(test_worker_authorization PRIVATE -Wall -Wextra -Werror)
foreach(mode metadata denial capability backend cancel allocation)
  add_test(NAME worker_authorization_${mode} COMMAND test_worker_authorization ${mode})
  set_tests_properties(worker_authorization_${mode} PROPERTIES TIMEOUT 15)
endforeach()
add_test(NAME session_worker_pty COMMAND test_session_worker)
add_test(NAME session_worker_pressure COMMAND test_session_worker pressure)
add_test(NAME session_worker_bytes COMMAND test_session_worker bytes)
set_tests_properties(session_worker_pty session_worker_pressure session_worker_bytes
                     PROPERTIES TIMEOUT 15)

add_test(NAME session_worker_failure COMMAND test_session_worker failure)
set_tests_properties(session_worker_failure PROPERTIES TIMEOUT 15)

foreach(mode replies reply-overflow reply-write-failure reply-thread reply-init allocator)
  add_test(NAME session_worker_${mode} COMMAND test_session_worker ${mode})
  set_tests_properties(session_worker_${mode} PROPERTIES TIMEOUT 15)
endforeach()

add_executable(test_checkpoint_task tests/test_checkpoint_task.c)
target_include_directories(test_checkpoint_task PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_checkpoint_task PRIVATE ghosterm_core)
target_compile_options(test_checkpoint_task PRIVATE -Wall -Wextra -Werror)
foreach(mode success cancel-running cancel-queued cancel-ready failure empty close invalid allocator)
  add_test(NAME checkpoint_task_${mode} COMMAND test_checkpoint_task ${mode})
  set_tests_properties(checkpoint_task_${mode} PROPERTIES TIMEOUT 12)
endforeach()

add_executable(test_attachment tests/test_attachment.c)
target_include_directories(test_attachment PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_attachment PRIVATE ghosterm_core)
target_compile_options(test_attachment PRIVATE -Wall -Wextra -Werror)
foreach(mode nojob race borrowed running ready failure failure-stale backpressure invalid overflow early-ready allocator ended end-wait unknown-end)
  add_test(NAME attachment_${mode} COMMAND test_attachment ${mode})
  set_tests_properties(attachment_${mode} PROPERTIES TIMEOUT 12)
endforeach()

add_executable(test_restore_stream tests/test_restore_stream.c)
target_include_directories(test_restore_stream PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_restore_stream PRIVATE ghosterm_core)
target_compile_options(test_restore_stream PRIVATE -Wall -Wextra -Werror)
foreach(mode flow empty limits protocol phases reset interrupt invalid allocator ended)
  add_test(NAME restore_stream_${mode} COMMAND test_restore_stream ${mode})
  set_tests_properties(restore_stream_${mode} PROPERTIES TIMEOUT 10)
endforeach()

add_executable(test_attachment_peer tests/test_attachment_peer.c)
target_include_directories(test_attachment_peer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_attachment_peer PRIVATE ghosterm_core)
target_compile_options(test_attachment_peer PRIVATE -Wall -Wextra -Werror)
foreach(mode flow kill stale pressure signals invalidated-fd running borrowed exec invalid eof send-close pending allocator)
  add_test(NAME attachment_peer_${mode} COMMAND test_attachment_peer ${mode})
  set_tests_properties(attachment_peer_${mode} PROPERTIES TIMEOUT 12)
endforeach()

add_executable(test_program_status tests/test_program_status.c)
target_include_directories(test_program_status PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_program_status PRIVATE ghosterm_core)
target_compile_options(test_program_status PRIVATE -Wall -Wextra -Werror)
foreach(mode copy clear aliases limits states invalid prompt allocator domains)
  add_test(NAME session_program_status_${mode} COMMAND test_program_status ${mode})
  set_tests_properties(session_program_status_${mode} PROPERTIES TIMEOUT 10)
endforeach()

add_executable(test_command_status tests/test_command_status.c)
target_include_directories(test_command_status PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_command_status PRIVATE ghosterm_core)
target_compile_options(test_command_status PRIVATE -Wall -Wextra -Werror)
foreach(mode lifecycle unknown clock copy restart invalid)
  add_test(NAME session_command_status_${mode} COMMAND test_command_status ${mode})
  set_tests_properties(session_command_status_${mode} PROPERTIES TIMEOUT 10)
endforeach()

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

add_executable(test_worker_result tests/test_worker_result.c)
target_include_directories(test_worker_result PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_worker_result PRIVATE ghosterm_core)
target_compile_options(test_worker_result PRIVATE -Wall -Wextra -Werror)
foreach(mode state-open backend-open read input resize apply reply capture capture-null stop eof-known eof-unknown eof-signal eof-unavailable eof-invalid eof-zero invalid)
  add_test(NAME worker_result_${mode} COMMAND test_worker_result ${mode})
  set_tests_properties(worker_result_${mode} PROPERTIES TIMEOUT 15)
endforeach()

add_executable(test_worker_scheme tests/test_worker_scheme.c)
target_include_directories(test_worker_scheme PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_worker_scheme PRIVATE ghosterm_core)
target_compile_options(test_worker_scheme PRIVATE -Wall -Wextra -Werror)
foreach(mode flow unsupported failure invalid)
  add_test(NAME worker_scheme_${mode} COMMAND test_worker_scheme ${mode})
  set_tests_properties(worker_scheme_${mode} PROPERTIES TIMEOUT 12)
endforeach()

add_executable(test_worker_wake tests/test_worker_wake.c)
target_include_directories(test_worker_wake PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_worker_wake PRIVATE ghosterm_core)
target_compile_options(test_worker_wake PRIVATE -Wall -Wextra -Werror)
foreach(mode input stop pending legacy real failure)
  add_test(NAME worker_wake_${mode} COMMAND test_worker_wake ${mode})
  set_tests_properties(worker_wake_${mode} PROPERTIES TIMEOUT 15)
endforeach()

add_executable(test_session_manager tests/test_session_manager.c)
target_include_directories(test_session_manager PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_session_manager PRIVATE ghosterm_core)
target_compile_options(test_session_manager PRIVATE -Wall -Wextra -Werror)
foreach(mode lifecycle startup allocation capacity joining creating)
  add_test(NAME session_manager_${mode} COMMAND test_session_manager ${mode})
  set_tests_properties(session_manager_${mode} PROPERTIES TIMEOUT 20)
endforeach()

add_executable(test_session_client tests/test_session_client.c)
target_include_directories(test_session_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_session_client PRIVATE ghosterm_client_protocol)
target_compile_options(test_session_client PRIVATE -Wall -Wextra -Werror)
foreach(mode flow uncertain deadlines busy heartbeat identity sink readonly fatal clock allocation cancel network generation restore-deadline compatibility validation)
  add_test(NAME session_client_${mode} COMMAND test_session_client ${mode})
  set_tests_properties(session_client_${mode} PROPERTIES TIMEOUT 10)
endforeach()

add_executable(test_session_management tests/test_session_management.c)
target_include_directories(test_session_management PRIVATE src)
target_link_libraries(test_session_management PRIVATE ghosterm_client_protocol)
target_compile_options(test_session_management PRIVATE -Wall -Wextra -Werror)
foreach(mode pending admission catalog catalog-network stop stop-unknown validation)
  add_test(NAME session_management_${mode} COMMAND test_session_management ${mode})
  set_tests_properties(session_management_${mode} PROPERTIES TIMEOUT 10)
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

add_executable(test_ssh_session_provider tests/test_ssh_session_provider.c)
target_include_directories(test_ssh_session_provider PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_ssh_session_provider PRIVATE ghosterm_core)
target_compile_options(test_ssh_session_provider PRIVATE -Wall -Wextra -Werror)
if(GHOSTERM_WITH_LIBSSH2 AND SSH2_FOUND)
  foreach(mode requests stale deadline budget cancel rejection admission clock reentry invalid)
    add_test(NAME ssh_provider_${mode} COMMAND test_ssh_session_provider ${mode})
    set_tests_properties(ssh_provider_${mode} PROPERTIES TIMEOUT 5)
  endforeach()
  add_test(NAME ssh_provider_e2e COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/ssh_e2e.sh $<TARGET_FILE:test_ssh_session_provider>)
  set_tests_properties(ssh_provider_e2e PROPERTIES TIMEOUT 30)
else()
  add_test(NAME ssh_provider_unsupported COMMAND test_ssh_session_provider unsupported)
endif()

add_executable(test_proxy_status tests/test_proxy_status.c)
target_include_directories(test_proxy_status PRIVATE include src)
target_compile_options(test_proxy_status PRIVATE -Wall -Wextra -Werror)
add_test(NAME proxy_exit_status COMMAND test_proxy_status)

add_executable(test_ssh_proxy_status tests/test_ssh_proxy_status.c)
target_include_directories(test_ssh_proxy_status PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_ssh_proxy_status PRIVATE ghosterm_core)
target_compile_options(test_ssh_proxy_status PRIVATE -Wall -Wextra -Werror)
if(GHOSTERM_WITH_LIBSSH2 AND SSH2_FOUND)
  add_test(NAME ssh_proxy_status_e2e COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/ssh_e2e.sh $<TARGET_FILE:test_ssh_proxy_status>)
  set_tests_properties(ssh_proxy_status_e2e PROPERTIES TIMEOUT 30)
endif()

add_executable(test_local_provider tests/test_local_provider.c)
target_include_directories(test_local_provider PRIVATE src)
target_link_libraries(test_local_provider PRIVATE ghosterm_client_protocol)
target_compile_options(test_local_provider PRIVATE -Wall -Wextra -Werror)
foreach(case validation binary fd status missing cancel restart clock)
  add_test(NAME local_provider_${case} COMMAND test_local_provider ${case} ${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt)
  set_tests_properties(local_provider_${case} PROPERTIES TIMEOUT 10)
endforeach()

add_executable(test_session_api tests/test_session_api.c)
target_link_libraries(test_session_api PRIVATE ghosterm_core)
target_compile_options(test_session_api PRIVATE -Wall -Wextra -Werror)
add_test(NAME session_public_api COMMAND test_session_api ${CMAKE_CURRENT_SOURCE_DIR}/..)
set_tests_properties(session_public_api PROPERTIES TIMEOUT 10)
add_executable(test_session_api_bridge tests/test_session_api_bridge.c)
target_include_directories(test_session_api_bridge PRIVATE src)
target_link_libraries(test_session_api_bridge PRIVATE ghosterm_core)
target_compile_options(test_session_api_bridge PRIVATE -Wall -Wextra -Werror)
add_test(NAME session_public_bridge COMMAND test_session_api_bridge)
add_executable(test_session_setup_bridge tests/test_session_setup_bridge.c)
target_include_directories(test_session_setup_bridge PRIVATE src)
target_link_libraries(test_session_setup_bridge PRIVATE ghosterm_core)
target_compile_options(test_session_setup_bridge PRIVATE -Wall -Wextra -Werror)
add_test(NAME session_setup_bridge COMMAND test_session_setup_bridge)
add_executable(test_session_setup tests/test_session_setup.c)
target_link_libraries(test_session_setup PRIVATE ghosterm_core)
target_compile_options(test_session_setup PRIVATE -Wall -Wextra -Werror)
get_target_property(setup_core_definitions ghosterm_core COMPILE_DEFINITIONS)
if("GT_HAVE_LIBSSH2=1" IN_LIST setup_core_definitions)
  add_test(NAME session_setup_ssh COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/ssh_e2e.sh $<TARGET_FILE:test_session_setup>)
  set_tests_properties(session_setup_ssh PROPERTIES TIMEOUT 25)
else()
  add_test(NAME session_setup_unsupported COMMAND test_session_setup --unsupported)
endif()
set_tests_properties(session_setup_bridge PROPERTIES TIMEOUT 10)

add_executable(test_session_style_wire tests/test_session_style_wire.c)
target_include_directories(test_session_style_wire PRIVATE src)
target_link_libraries(test_session_style_wire PRIVATE ghosterm_core)
target_compile_options(test_session_style_wire PRIVATE -Wall -Wextra -Werror)
foreach(mode user data)
  add_test(NAME session_style_${mode} COMMAND test_session_style_wire ${mode})
  set_tests_properties(session_style_${mode} PROPERTIES TIMEOUT 15)
endforeach()

if(GHOSTERM_WITH_LIBSSH2 AND SSH2_FOUND)
  add_executable(test_session_deferred_auth tests/test_session_deferred_auth.c)
  target_include_directories(test_session_deferred_auth PRIVATE src)
  target_link_libraries(test_session_deferred_auth PRIVATE ghosterm_core)
  target_compile_options(test_session_deferred_auth PRIVATE -Wall -Wextra -Werror)
  add_test(NAME session_deferred_auth_e2e COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/session_deferred_auth_e2e.sh $<TARGET_FILE:test_session_deferred_auth>)
  set_tests_properties(session_deferred_auth_e2e PROPERTIES TIMEOUT 30)
endif()

add_executable(test_process_meta tests/test_process_meta.c)
target_include_directories(test_process_meta PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_process_meta PRIVATE ghosterm_core)
target_compile_options(test_process_meta PRIVATE -Wall -Wextra -Werror)
foreach(mode codec owner)
  add_test(NAME session_process_${mode} COMMAND test_process_meta ${mode})
  set_tests_properties(session_process_${mode} PROPERTIES TIMEOUT 12)
endforeach()
