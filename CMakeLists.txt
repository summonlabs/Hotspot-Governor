cmake_minimum_required(VERSION 3.20)

project(HotspotGovernor
        VERSION 1.0.0
        DESCRIPTION "Vendor-neutral C++20 runtime for generation-bound identification and bounded mitigation intent for localized fabric saturation"
        HOMEPAGE_URL "https://github.com/summonlabs/Hotspot-Governor"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(HGM_BUILD_TESTS "Build the Hotspot Governor test suite" ON)
option(HGM_BUILD_TOOLS "Build the Hotspot Governor command line tools" ON)
option(HGM_BUILD_BENCH "Build the synthetic benchmark harness" ON)
option(HGM_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(HGM_ENABLE_ASAN "Enable AddressSanitizer/UndefinedBehaviorSanitizer (GCC/Clang)" OFF)
option(HGM_ENABLE_MSVC_ANALYZE "Run MSVC /analyze as part of the build" OFF)
option(HGM_BUILD_MULTIPROCESS_TESTS "Build tests that spawn real child OS processes" ON)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

find_package(Threads REQUIRED)

if(WIN32)
  # Applies to this project's own targets only; nothing is exported to
  # consumers.
  add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
endif()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
set(HGM_PUBLIC_HEADERS
  include/hgm/authority.hpp
  include/hgm/byteio.hpp
  include/hgm/capacity.hpp
  include/hgm/checked.hpp
  include/hgm/coordinator.hpp
  include/hgm/detection.hpp
  include/hgm/digest.hpp
  include/hgm/engine.hpp
  include/hgm/enums.hpp
  include/hgm/error.hpp
  include/hgm/explanation.hpp
  include/hgm/frame.hpp
  include/hgm/governor.hpp
  include/hgm/ids.hpp
  include/hgm/intervention.hpp
  include/hgm/journal.hpp
  include/hgm/limits.hpp
  include/hgm/metrics.hpp
  include/hgm/paths.hpp
  include/hgm/policy.hpp
  include/hgm/provenance.hpp
  include/hgm/synthetic.hpp
  include/hgm/signals.hpp
  include/hgm/store.hpp
  include/hgm/time.hpp
  include/hgm/topology.hpp
  include/hgm/transport.hpp
  include/hgm/traffic.hpp
  include/hgm/version.hpp
  include/hgm/worker.hpp
)

set(HGM_SOURCES
  src/authority.cpp
  src/byteio.cpp
  src/capacity.cpp
  src/coordinator.cpp
  src/detection.cpp
  src/digest.cpp
  src/engine.cpp
  src/enums.cpp
  src/error.cpp
  src/explanation.cpp
  src/file_io.cpp
  src/frame.cpp
  src/governor.cpp
  src/intervention.cpp
  src/journal.cpp
  src/metrics.cpp
  src/paths.cpp
  src/policy.cpp
  src/provenance.cpp
  src/synthetic.cpp
  src/signals.cpp
  src/store.cpp
  src/time.cpp
  src/topology.cpp
  src/transport.cpp
  src/traffic.cpp
  src/version.cpp
  src/worker.cpp
)

add_library(hotspot_governor STATIC ${HGM_SOURCES} ${HGM_PUBLIC_HEADERS})
add_library(HotspotGovernor::hotspot_governor ALIAS hotspot_governor)

target_include_directories(hotspot_governor
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(hotspot_governor PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(hotspot_governor PUBLIC ws2_32)
endif()

target_compile_definitions(hotspot_governor PUBLIC HGM_STATIC=1)

set_target_properties(hotspot_governor PROPERTIES
  OUTPUT_NAME hotspot_governor
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR})

# ---------------------------------------------------------------------------
# Warnings
# ---------------------------------------------------------------------------
if(MSVC)
  set(HGM_WARNINGS /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
  if(HGM_WARNINGS_AS_ERRORS)
    list(APPEND HGM_WARNINGS /WX)
  endif()
  if(HGM_ENABLE_MSVC_ANALYZE)
    list(APPEND HGM_WARNINGS /analyze)
  endif()
else()
  set(HGM_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast)
  if(HGM_WARNINGS_AS_ERRORS)
    list(APPEND HGM_WARNINGS -Werror)
  endif()
endif()

target_compile_options(hotspot_governor PRIVATE ${HGM_WARNINGS})

# Sanitizers are propagated to consumers so that test and tool binaries are
# instrumented together with the library. On MSVC the runtime is inferred by
# the linker; on GCC/Clang the flags must also be passed at link time.
if(HGM_ENABLE_ASAN)
  if(MSVC)
    target_compile_options(hotspot_governor PUBLIC /fsanitize=address)
  else()
    target_compile_options(hotspot_governor PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(hotspot_governor PUBLIC -fsanitize=address,undefined)
  endif()
endif()

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------
if(HGM_BUILD_TOOLS)
  add_executable(hgm_cli tools/hgm_cli.cpp)
  target_link_libraries(hgm_cli PRIVATE hotspot_governor)
  target_compile_options(hgm_cli PRIVATE ${HGM_WARNINGS})

  add_executable(hgm_coordinator tools/hgm_coordinator.cpp)
  target_link_libraries(hgm_coordinator PRIVATE hotspot_governor)
  target_compile_options(hgm_coordinator PRIVATE ${HGM_WARNINGS})

  add_executable(hgm_publisher tools/hgm_publisher.cpp)
  target_link_libraries(hgm_publisher PRIVATE hotspot_governor)
  target_compile_options(hgm_publisher PRIVATE ${HGM_WARNINGS})
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------
if(HGM_BUILD_BENCH)
  add_executable(hgm_bench bench/hgm_bench.cpp)
  target_link_libraries(hgm_bench PRIVATE hotspot_governor)
  target_compile_options(hgm_bench PRIVATE ${HGM_WARNINGS})
  if(HGM_BUILD_TESTS)
    add_test(NAME hgm_bench_smoke COMMAND hgm_bench --quick)
  endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(HGM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
install(TARGETS hotspot_governor
  EXPORT HotspotGovernorTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT HotspotGovernorTargets
  FILE HotspotGovernorTargets.cmake
  NAMESPACE HotspotGovernor::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HotspotGovernor)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/HotspotGovernorConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/HotspotGovernorConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HotspotGovernor)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/HotspotGovernorConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/HotspotGovernorConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/HotspotGovernorConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HotspotGovernor)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
  DESTINATION ${CMAKE_INSTALL_DATADIR}/HotspotGovernor)
