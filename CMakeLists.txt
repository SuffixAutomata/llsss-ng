cmake_minimum_required(VERSION 3.20)
project(rlife_llsss LANGUAGES CXX)

add_executable(rlife_llsss src/main.cpp)
target_include_directories(rlife_llsss PRIVATE src)
target_compile_features(rlife_llsss PRIVATE cxx_std_20)
target_compile_options(rlife_llsss PRIVATE -Wall -Wextra -Wpedantic)
