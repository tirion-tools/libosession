cmake_minimum_required(VERSION 3.16)
project(osession
    VERSION 0.1.0
    DESCRIPTION "Compact SQLite-backed container for SQL Server plan capture data"
    LANGUAGES C CXX
)

option(OSESSION_BUILD_TESTS "Build libosession tests" ON)
# OFF by default - miniz is linked privately and isn't in our export set,
# which trips CMake's install(EXPORT). Re-enable when we add a proper
# install treatment for the bundled miniz.
option(OSESSION_INSTALL "Install libosession targets" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 99)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Vendored miniz for plan-blob deflate compression. When libosession is
# pulled in alongside another project that already vendors miniz (e.g.
# sqlsentry / openplan), reuse that target instead of double-registering.
if(NOT TARGET miniz)
    add_subdirectory(third_party/miniz EXCLUDE_FROM_ALL)
endif()

# SQLite. Linux: install libsqlite3-dev. Windows: vcpkg / installer bundle.
find_package(SQLite3 REQUIRED)

add_library(osession STATIC src/file.cpp)
add_library(osession::osession ALIAS osession)

target_include_directories(osession
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        third_party/miniz
)
target_compile_features(osession PUBLIC cxx_std_17)
target_link_libraries(osession
    PRIVATE
        SQLite::SQLite3
        miniz
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(osession PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

if(OSESSION_BUILD_TESTS)
    enable_testing()
    add_executable(osession_test_basic tests/test_basic.cpp)
    target_link_libraries(osession_test_basic PRIVATE osession)
    add_test(NAME osession_round_trip
             COMMAND osession_test_basic)
endif()

if(OSESSION_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)
    install(TARGETS osession EXPORT osessionTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(EXPORT osessionTargets
        FILE osessionTargets.cmake
        NAMESPACE osession::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/osession)
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/osessionConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)
    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/osessionConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/osessionConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/osession)
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/osessionConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/osessionConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/osession)
endif()
