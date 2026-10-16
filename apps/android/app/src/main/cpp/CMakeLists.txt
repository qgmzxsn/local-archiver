cmake_minimum_required(VERSION 3.16)
project(archiver_jni)

# Not built in the phase 1 environment. Android Studio would compile this
# after 7z.so itself has been built with the NDK and placed in jniLibs.
if(NOT SEVENZIP_SRC)
  message(FATAL_ERROR "SEVENZIP_SRC is required. This Android native build was not run.")
endif()

add_library(archiver_jni SHARED
  archiver_jni.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../../../../../../core/src/path_safety.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../../../../../../core/src/safe_write.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../../../../../../core/src/engine.cpp
)
target_include_directories(archiver_jni PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../../../../../../core/include
  ${SEVENZIP_SRC}/CPP
)
target_compile_definitions(archiver_jni PRIVATE ARCHIVER_BUILDING _FILE_OFFSET_BITS=64)
