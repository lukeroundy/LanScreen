cmake_minimum_required(VERSION 3.16)
project(lanscreen VERSION 0.2.25 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network DBus)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
  gstreamer-1.0
  gstreamer-video-1.0
  gstreamer-app-1.0
)

add_executable(lanscreen
  src/main.cpp
  src/common.cpp
  src/netutil.cpp
  src/probe.cpp
  src/pipeline.cpp
  src/discovery.cpp
  src/control.cpp
  src/inputinject.cpp
  src/outputs.cpp
  src/kdisplay.cpp
  src/portal.cpp
  src/hostsession.cpp
  src/clientsession.cpp
  src/selftest.cpp
  src/ui/videowidget.cpp
  src/ui/hostpage.cpp
  src/ui/clientpage.cpp
  src/ui/mainwindow.cpp
)

target_include_directories(lanscreen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(lanscreen PRIVATE
  LANSCREEN_VERSION="${PROJECT_VERSION}"
)
target_link_libraries(lanscreen PRIVATE
  Qt6::Core
  Qt6::Widgets
  Qt6::Network
  Qt6::DBus
  PkgConfig::GST
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(lanscreen PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

install(TARGETS lanscreen RUNTIME DESTINATION bin)
install(FILES scripts/99-lanscreen-uinput.rules
        DESTINATION lib/udev/rules.d
        OPTIONAL)
