cmake_minimum_required(VERSION 3.20)
project(rlife_llsss LANGUAGES CXX)

add_executable(rlife_llsss src/main.cpp)
target_include_directories(rlife_llsss PRIVATE src)
target_compile_features(rlife_llsss PRIVATE cxx_std_20)
target_compile_options(rlife_llsss PRIVATE -Wall -Wextra -Wpedantic)

option(RLIFE_NATIVE "Optimize for the build host CPU" ON)
if(RLIFE_NATIVE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(rlife_llsss PRIVATE -march=native)
endif()

include(CTest)
if(BUILD_TESTING)
  add_test(
    NAME search_smoke
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/search_regression.sh $<TARGET_FILE:rlife_llsss> smoke
  )
  set_tests_properties(search_smoke PROPERTIES TIMEOUT 60 LABELS "smoke;regression")
endif()
