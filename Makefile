CXX ?= c++
CXXFLAGS ?= -O3 -DNDEBUG -std=c++20 -Wall -Wextra -Wpedantic
NATIVE_FLAGS ?= -march=native
CPPFLAGS ?=
LDFLAGS ?=

TARGET := rlife_llsss
SOURCES := src/main.cpp
HEADERS := $(wildcard src/rlife/*.hpp)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(NATIVE_FLAGS) -Isrc $(SOURCES) $(LDFLAGS) -o $@

clean:
	rm -f $(TARGET)
