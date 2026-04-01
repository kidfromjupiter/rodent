cmake_minimum_required(VERSION 3.20)
project(rodent LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(BUSCTL_EXECUTABLE NAMES busctl REQUIRED)
find_program(SDBUS_XML2CPP_EXECUTABLE NAMES sdbus-c++-xml2cpp REQUIRED)

pkg_check_modules(SDBUSCPP REQUIRED IMPORTED_TARGET sdbus-c++)

add_executable(
    rodent
    main.cpp
    src/bluez/BluezClient.cpp
)

target_include_directories(
    rodent
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(
    rodent
    PRIVATE
        PkgConfig::SDBUSCPP
)

add_custom_target(
    refresh_bluez_codegen
    COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/refresh_bluez_codegen.py
        --xml-root ${CMAKE_CURRENT_SOURCE_DIR}/dbus/bluez
        --generated-root ${CMAKE_CURRENT_SOURCE_DIR}/generated/bluez
        --xml2cpp ${SDBUS_XML2CPP_EXECUTABLE}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Refresh BlueZ introspection XML and sdbus-c++ glue from the live system bus"
    VERBATIM
)
