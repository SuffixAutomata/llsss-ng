CXX ?= c++
CXXFLAGS ?= -O3 -DNDEBUG -std=c++20 -Wall -Wextra -Wpedantic
CPPFLAGS ?=
LDFLAGS ?=

TARGET := rlife_llsss
SOURCES := src/main.cpp
HEADERS := $(wildcard src/rlife/*.hpp)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -Isrc $(SOURCES) $(LDFLAGS) -o $@

clean:
	rm -f $(TARGET)
