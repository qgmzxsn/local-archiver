cmake_minimum_required(VERSION 3.16)
project(local-archiver VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT SEVENZIP_SRC)
  message(FATAL_ERROR
    "Set -DSEVENZIP_SRC= to the 7-Zip 26.04 source tree.\n"
    "Run scripts/fetch-7zip.sh. The core includes 7-Zip's interface headers\n"
    "and loads 7z.so / 7z.dll / lib7z.dylib at runtime. It does not link 7-Zip in.")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_library(archiver SHARED
  core/src/path_safety.cpp
  core/src/safe_write.cpp
  core/src/engine.cpp
)
target_include_directories(archiver
  PUBLIC core/include
  PRIVATE ${SEVENZIP_SRC}/CPP
)
target_compile_definitions(archiver PRIVATE ARCHIVER_BUILDING _FILE_OFFSET_BITS=64)
target_compile_options(archiver PRIVATE -Wall -Wextra -Wno-deprecated -Wno-unused-parameter)
if(WIN32)
  # Static C++ runtime only. 7-Zip stays a separate DLL.
  target_link_libraries(archiver PRIVATE ole32 oleaut32 uuid)
  target_link_options(archiver PRIVATE -static-libgcc -static-libstdc++)
else()
  target_link_libraries(archiver PRIVATE ${CMAKE_DL_LIBS})
endif()
set_target_properties(archiver PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  POSITION_INDEPENDENT_CODE ON
)

function(archiver_origin target)
  if(UNIX AND NOT APPLE)
    set_target_properties(${target} PROPERTIES
      BUILD_RPATH "\$ORIGIN"
      INSTALL_RPATH "\$ORIGIN")
  elseif(APPLE)
    set_target_properties(${target} PROPERTIES
      BUILD_RPATH "@loader_path"
      INSTALL_RPATH "@loader_path")
  endif()
endfunction()

if(NOT ANDROID)
  add_executable(archiver-cli tools/cli/main.cpp)
  target_link_libraries(archiver-cli PRIVATE archiver)
  target_compile_options(archiver-cli PRIVATE -Wall -Wextra)
  archiver_origin(archiver-cli)

  add_executable(test_path_safety tests/test_path_safety.cpp)
  target_link_libraries(test_path_safety PRIVATE archiver)
  target_compile_options(test_path_safety PRIVATE -Wall -Wextra)
  archiver_origin(test_path_safety)
  if(WIN32)
    target_link_options(archiver-cli PRIVATE -static-libgcc -static-libstdc++)
    target_link_options(test_path_safety PRIVATE -static-libgcc -static-libstdc++)
  endif()
endif()

if(ANDROID)
  # NDK sysroot provides jni.h. This is the .so the Kotlin shell loads.
  add_library(archiver_jni SHARED apps/android/app/src/main/cpp/archiver_jni.cpp)
  target_include_directories(archiver_jni PRIVATE core/include)
  target_link_libraries(archiver_jni PRIVATE archiver)
  target_compile_options(archiver_jni PRIVATE -Wall -Wextra)
elseif(NOT CMAKE_CROSSCOMPILING)
  find_package(JNI QUIET)
  if(JNI_FOUND)
    add_library(archiver_jni_check OBJECT apps/android/app/src/main/cpp/archiver_jni.cpp)
    target_include_directories(archiver_jni_check PRIVATE ${JNI_INCLUDE_DIRS} core/include)
    target_compile_options(archiver_jni_check PRIVATE -Wall -Wextra)
  endif()
endif()

# Host Qt is an ELF library. A Windows cross build links Qt only when
# ARCHIVER_WINDOWS_QT=ON and CMAKE_PREFIX_PATH points at a MinGW Qt kit.
if(NOT CMAKE_CROSSCOMPILING OR ARCHIVER_WINDOWS_QT)
  find_package(Qt6 QUIET COMPONENTS Widgets)
  if(Qt6_FOUND)
    add_subdirectory(apps/desktop)
  else()
    message(STATUS "Qt 6 Widgets not found. The shared desktop shell was not built.")
  endif()
else()
  message(STATUS "Cross build: Qt shell is not linked. Pass -DARCHIVER_WINDOWS_QT=ON with a MinGW Qt kit to try it.")
endif()
