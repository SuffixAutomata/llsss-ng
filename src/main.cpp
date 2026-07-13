#include "rlife/solver.hpp"

#include <exception>
#include <iostream>

int main(int argc, char** argv) {
    try {
        auto options = rlife::llsss::parse_cli(argc, argv);
        return rlife::llsss::Solver(std::move(options)).run();
    } catch (const std::exception& error) {
        std::cerr << "rlife_llsss: " << error.what() << '\n';
        return 2;
    }
}
