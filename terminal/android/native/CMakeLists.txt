cmake_minimum_required(VERSION 3.22.1)
project(ghosterm_vt_android C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
get_filename_component(GHOSTERM_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(GHOSTERM_DEPS "${GHOSTERM_ROOT}/build/android-native/${ANDROID_ABI}")
if(NOT EXISTS "${GHOSTERM_DEPS}/lib/libghostty-vt.a")
    message(FATAL_ERROR "Missing VT archive: run python3 scripts/build-android-native.py")
endif()
add_library(ghostty_vt STATIC IMPORTED)
set_target_properties(ghostty_vt PROPERTIES
    IMPORTED_LOCATION "${GHOSTERM_DEPS}/lib/libghostty-vt.a"
    INTERFACE_INCLUDE_DIRECTORIES "${GHOSTERM_ROOT}/vendor/ghostty/include")
add_library(ghosterm_vt_android SHARED vt_jni.c surface.c)
target_compile_options(ghosterm_vt_android PRIVATE -Wall -Wextra -Werror)
target_link_libraries(ghosterm_vt_android PRIVATE ghostty_vt m)
target_link_options(ghosterm_vt_android PRIVATE -Wl,-z,max-page-size=16384 -Wl,--exclude-libs,ALL)

target_include_directories(ghosterm_vt_android PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
