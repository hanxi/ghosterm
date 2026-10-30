# Isolated Core copies under tmp/ share the enclosing repository's verified
# dependency, never an unrelated Homebrew library. An explicit prefix is allowed
# for test/build infrastructure, but must pass the same source/patch receipt.
set(_ssh2_root "${CMAKE_CURRENT_SOURCE_DIR}")
while(NOT EXISTS "${_ssh2_root}/patches/libssh2/1.11.1/WHY.md")
  get_filename_component(_ssh2_parent "${_ssh2_root}" DIRECTORY)
  if(_ssh2_parent STREQUAL _ssh2_root)
    message(FATAL_ERROR "Cannot locate pinned libssh2 patches; configure within the repository")
  endif()
  set(_ssh2_root "${_ssh2_parent}")
endwhile()
set(GHOSTERM_HOST_SSH2_PREFIX "${_ssh2_root}/build/host-native" CACHE PATH "Verified patched host libssh2 prefix")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/verify-ssh2.py"
  --root "${_ssh2_root}" --prefix "${GHOSTERM_HOST_SSH2_PREFIX}"
  RESULT_VARIABLE _ssh2_status OUTPUT_VARIABLE _ssh2_output ERROR_VARIABLE _ssh2_error)
if(NOT _ssh2_status EQUAL 0)
  message(FATAL_ERROR "${_ssh2_error}\nPrepare dependencies with python3 scripts/build-host-ssh2.py --fetch; offline gates do not download")
endif()
add_library(ghosterm_host_ssh2 SHARED IMPORTED)
set_target_properties(ghosterm_host_ssh2 PROPERTIES
  IMPORTED_LOCATION "${GHOSTERM_HOST_SSH2_PREFIX}/lib/${CMAKE_SHARED_LIBRARY_PREFIX}ssh2${CMAKE_SHARED_LIBRARY_SUFFIX}"
  INTERFACE_INCLUDE_DIRECTORIES "${GHOSTERM_HOST_SSH2_PREFIX}/include")
set(SSH2_INCLUDE_DIRS "${GHOSTERM_HOST_SSH2_PREFIX}/include")
set(SSH2_FOUND TRUE)
target_compile_definitions(ghosterm_core PRIVATE GT_HAVE_LIBSSH2=1)
target_link_libraries(ghosterm_core PUBLIC ghosterm_host_ssh2)
message(STATUS "ghosterm_core: verified patched libssh2 1.11.1")
