CXX ?= c++
CXXFLAGS ?= -O3 -DNDEBUG -std=c++20 -Wall -Wextra -Wpedantic
HOST_ARCH := $(shell uname -m)
ifneq ($(filter arm64 aarch64,$(HOST_ARCH)),)
NATIVE_FLAGS ?= -mcpu=native
else
NATIVE_FLAGS ?= -march=native
endif
CPPFLAGS ?=
LDFLAGS ?=

TARGET := rlife
SOURCES := src/main.cpp src/rlife/partition.cpp
OBJECTS := src/main.o src/rlife/partition.o src/rlife/indexed_executor.o
HEADERS := $(wildcard src/rlife/*.hpp)

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(OBJECTS) $(LDFLAGS) -fopenmp -o $@

src/main.o: src/main.cpp $(HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(NATIVE_FLAGS) -Isrc -c $< -o $@

src/rlife/partition.o: src/rlife/partition.cpp $(HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(NATIVE_FLAGS) -Isrc -c $< -o $@

src/rlife/indexed_executor.o: src/rlife/indexed_executor.cpp src/rlife/indexed_executor.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(NATIVE_FLAGS) -fopenmp -Isrc -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJECTS)

test: $(TARGET)
	bash tests/search_regression.sh ./$(TARGET)
