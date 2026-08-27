#include "rlife/solver.hpp"

#include <exception>
#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
  try {
    if(argc >= 2 && std::string_view(argv[1]) == "partition")
      return rlife::llsss::run_partition_command(argc, argv);
    auto options = rlife::llsss::parse_cli(argc, argv);
    return rlife::llsss::Solver(std::move(options)).run();
  } catch(const std::exception& error) {
    std::cerr << "rlife: " << error.what() << '\n';
    return 2;
  }
}
