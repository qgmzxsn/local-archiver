find_package(Qt6 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()

qt_add_executable(local-archiver
  main.cpp
  MainWindow.cpp
  MainWindow.hpp
)
target_link_libraries(local-archiver PRIVATE Qt6::Widgets archiver)
target_compile_options(local-archiver PRIVATE -Wall -Wextra)
if(UNIX AND NOT APPLE)
  set_target_properties(local-archiver PROPERTIES
    BUILD_RPATH "\$ORIGIN"
    INSTALL_RPATH "\$ORIGIN")
elseif(APPLE)
  set_target_properties(local-archiver PROPERTIES
    BUILD_RPATH "@loader_path"
    INSTALL_RPATH "@loader_path")
endif()
