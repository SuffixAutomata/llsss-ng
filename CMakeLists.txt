cmake_minimum_required(VERSION 3.20)
project(rlife_llsss LANGUAGES CXX)

find_package(OpenMP REQUIRED)

add_library(rlife_indexed_executor STATIC src/rlife/indexed_executor.cpp)
target_compile_features(rlife_indexed_executor PRIVATE cxx_std_20)
target_compile_options(rlife_indexed_executor PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(rlife_indexed_executor PRIVATE OpenMP::OpenMP_CXX)

add_executable(rlife_llsss src/main.cpp)
target_include_directories(rlife_llsss PRIVATE src)
target_compile_features(rlife_llsss PRIVATE cxx_std_20)
target_compile_options(rlife_llsss PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(rlife_llsss PRIVATE rlife_indexed_executor)

include(CTest)
if(BUILD_TESTING)
  add_test(
    NAME parallel_regression
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/parallel_regression.sh
            $<TARGET_FILE:rlife_llsss>)
endif()

option(RLIFE_NATIVE "Optimize for the build host CPU" ON)
if(RLIFE_NATIVE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(rlife_llsss PRIVATE -march=native)
  target_compile_options(rlife_indexed_executor PRIVATE -march=native)
endif()
