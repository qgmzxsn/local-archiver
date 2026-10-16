find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent)
qt_standard_project_setup()

add_library(archiver_desktop STATIC
  MainWindow.cpp
  MainWindow.hpp
)
target_link_libraries(archiver_desktop PUBLIC Qt6::Widgets Qt6::Concurrent archiver)
target_include_directories(archiver_desktop PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
archiver_warnings(archiver_desktop)
qt_add_executable(local-archiver main.cpp)
set_target_properties(local-archiver PROPERTIES WIN32_EXECUTABLE TRUE)
qt_add_resources(local-archiver app_icon PREFIX "/" BASE "${PROJECT_SOURCE_DIR}/assets"
  FILES "${PROJECT_SOURCE_DIR}/assets/local-archiver.ico")
if(WIN32)
  enable_language(RC)
  set(ARCHIVER_ICON_PATH "${PROJECT_SOURCE_DIR}/assets/local-archiver.ico")
  configure_file("${PROJECT_SOURCE_DIR}/assets/application.rc.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/application.rc" @ONLY)
  target_sources(local-archiver PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/application.rc")
endif()
target_link_libraries(local-archiver PRIVATE archiver_desktop)
archiver_warnings(local-archiver)
if(NOT CMAKE_CROSSCOMPILING AND ARCHIVER_TEST_ENGINE AND ARCHIVER_TEST_7Z)
  find_package(Qt6 QUIET COMPONENTS Test)
  if(TARGET Qt6::Test)
    add_executable(test_desktop ../../tests/test_desktop.cpp)
    target_link_libraries(test_desktop PRIVATE archiver_desktop Qt6::Test)
    archiver_warnings(test_desktop)
    archiver_origin(test_desktop)
    add_test(NAME desktop_workflows
      COMMAND test_desktop "${ARCHIVER_TEST_7Z}" "${ARCHIVER_TEST_ENGINE}")
    get_filename_component(qt_prefix "${Qt6_DIR}/../../.." ABSOLUTE)
    set_tests_properties(desktop_workflows PROPERTIES
      ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${qt_prefix}/plugins"
      TIMEOUT 60)
  endif()
endif()
if(UNIX AND NOT APPLE)
  set_target_properties(local-archiver PROPERTIES
    BUILD_RPATH "\$ORIGIN"
    INSTALL_RPATH "\$ORIGIN")
elseif(APPLE)
  set_target_properties(local-archiver PROPERTIES
    BUILD_RPATH "@loader_path"
    INSTALL_RPATH "@loader_path")
endif()
