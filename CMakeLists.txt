cmake_minimum_required(VERSION 3.20)
project(rlife LANGUAGES CXX)

find_package(OpenMP REQUIRED)

add_library(rlife_indexed_executor STATIC src/rlife/indexed_executor.cpp)
target_compile_features(rlife_indexed_executor PRIVATE cxx_std_20)
target_compile_options(rlife_indexed_executor PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(rlife_indexed_executor PRIVATE OpenMP::OpenMP_CXX)

add_executable(rlife src/main.cpp src/rlife/partition.cpp)
target_include_directories(rlife PRIVATE src)
target_compile_features(rlife PRIVATE cxx_std_20)
target_compile_options(rlife PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(rlife PRIVATE rlife_indexed_executor)

option(RLIFE_NATIVE "Optimize for the build host CPU" ON)
if(RLIFE_NATIVE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$")
    set(RLIFE_NATIVE_FLAG -mcpu=native)
  else()
    set(RLIFE_NATIVE_FLAG -march=native)
  endif()
  target_compile_options(rlife PRIVATE ${RLIFE_NATIVE_FLAG})
  target_compile_options(rlife_indexed_executor PRIVATE ${RLIFE_NATIVE_FLAG})
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(structural_regression tests/structural_regression.cpp)
  target_include_directories(structural_regression PRIVATE src)
  target_compile_features(structural_regression PRIVATE cxx_std_20)
  target_compile_options(structural_regression PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(structural_regression PRIVATE rlife_indexed_executor)
  if(RLIFE_NATIVE_FLAG)
    target_compile_options(structural_regression PRIVATE ${RLIFE_NATIVE_FLAG})
  endif()
  add_test(NAME structural_regression COMMAND structural_regression)
  add_test(
    NAME search_regression_full
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/search_regression.sh $<TARGET_FILE:rlife> full
  )
  add_test(
    NAME search_smoke
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/search_regression.sh $<TARGET_FILE:rlife> smoke
  )
  set_tests_properties(search_regression_full PROPERTIES TIMEOUT 300 LABELS "full;regression")
  set_tests_properties(search_smoke PROPERTIES TIMEOUT 60 LABELS "smoke;regression")
  set_tests_properties(structural_regression PROPERTIES TIMEOUT 120 LABELS "regression")
endif()
