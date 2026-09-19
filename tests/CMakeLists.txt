# Hotspot Governor test suite.
# Copyright 2026 Summon Software Labs.
# SPDX-License-Identifier: Apache-2.0

set(HGM_TEST_SOURCES
  testing.cpp
  main.cpp
  test_foundation.cpp
  test_evidence.cpp
  test_frame.cpp
  test_detection.cpp
  test_attribution.cpp
  test_intervention.cpp
  test_governor.cpp
  test_persistence.cpp
  test_worker.cpp
  test_transport.cpp
  test_concurrency.cpp
  test_property.cpp
  test_adversarial.cpp
  test_hardening.cpp
)

add_executable(hgm_tests ${HGM_TEST_SOURCES})
target_link_libraries(hgm_tests PRIVATE hotspot_governor)
target_include_directories(hgm_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(hgm_tests PRIVATE ${HGM_WARNINGS})

add_test(NAME hgm_tests COMMAND hgm_tests)

foreach(hgm_suite foundation evidence frame detection attribution intervention governor persistence worker transport concurrency property adversarial hardening)
  add_test(NAME hgm_suite_${hgm_suite} COMMAND hgm_tests --suite ${hgm_suite})
endforeach()

if(HGM_BUILD_MULTIPROCESS_TESTS AND HGM_BUILD_TOOLS)
  add_executable(hgm_multiprocess_tests mp_main.cpp process.cpp testing.cpp)
  target_link_libraries(hgm_multiprocess_tests PRIVATE hotspot_governor)
  target_include_directories(hgm_multiprocess_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_options(hgm_multiprocess_tests PRIVATE ${HGM_WARNINGS})
  add_test(NAME hgm_multiprocess COMMAND hgm_multiprocess_tests)
  set_tests_properties(hgm_multiprocess PROPERTIES ENVIRONMENT
    "HGM_COORDINATOR_BIN=$<TARGET_FILE:hgm_coordinator>;HGM_PUBLISHER_BIN=$<TARGET_FILE:hgm_publisher>")
endif()
