CXX ?= c++
CXXFLAGS ?= -O3 -DNDEBUG -std=c++20 -Wall -Wextra -Wpedantic
NATIVE_FLAGS ?= -march=native
CPPFLAGS ?=
LDFLAGS ?=

TARGET := rlife_llsss
SOURCES := src/main.cpp
OBJECTS := src/main.o src/rlife/indexed_executor.o
HEADERS := $(wildcard src/rlife/*.hpp)

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(OBJECTS) $(LDFLAGS) -fopenmp -o $@

src/main.o: src/main.cpp $(HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(NATIVE_FLAGS) -Isrc -c $< -o $@

src/rlife/indexed_executor.o: src/rlife/indexed_executor.cpp src/rlife/indexed_executor.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(NATIVE_FLAGS) -fopenmp -Isrc -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJECTS)

test: $(TARGET)
	bash tests/parallel_regression.sh ./$(TARGET)
