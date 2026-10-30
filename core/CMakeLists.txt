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
  add_executable(test_sftp_browse tests/test_sftp_browse.c)
  target_include_directories(test_sftp_browse PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(test_sftp_browse PRIVATE ghosterm_core ${CMAKE_DL_LIBS})
  target_compile_options(test_sftp_browse PRIVATE -Wall -Wextra -Werror)
  add_test(NAME sftp_browse_e2e COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/ssh_e2e.sh $<TARGET_FILE:test_sftp_browse>)
  set_tests_properties(sftp_browse_e2e PROPERTIES TIMEOUT 30)
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
add_test(NAME tmux_command COMMAND test_tmux command)
set_tests_properties(tmux_command PROPERTIES TIMEOUT 15)

add_test(NAME tmux_adversarial COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test-tmux.py --fake --binary $<TARGET_FILE:test_tmux>)
set_tests_properties(tmux_adversarial PROPERTIES TIMEOUT 30)
endif()
