cmake_minimum_required(VERSION 3.16)
project(proone_tablet_daemon VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)

add_executable(proone-tablet-daemon src/main.cpp)
target_link_libraries(proone-tablet-daemon PRIVATE PkgConfig::LIBUSB)
target_compile_options(proone-tablet-daemon PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS proone-tablet-daemon RUNTIME DESTINATION bin)
