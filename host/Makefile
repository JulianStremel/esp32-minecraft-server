# Native (host) build of the server core: unit tests and a PC server binary.
#   make test      build + run unit tests
#   make server    build the PC server (host/build/mcserver)
ROOT    := $(abspath ..)
CORE    := $(ROOT)/lib/mcore/src
BUILD   := build
CXX     ?= g++
CXXFLAGS ?= -std=gnu++17 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
CXXFLAGS += -I$(CORE) -I$(ROOT)/host -DMC_HOST=1

CORE_SRCS := $(shell find $(CORE) -name '*.cpp')
CORE_OBJS := $(patsubst $(CORE)/%.cpp,$(BUILD)/core/%.o,$(CORE_SRCS))
PLAT_OBJ  := $(BUILD)/platform_posix.o
TEST_SRCS := $(wildcard tests/*.cpp)
TEST_OBJS := $(patsubst tests/%.cpp,$(BUILD)/tests/%.o,$(TEST_SRCS))

all: server

$(BUILD)/core/%.o: $(CORE)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -c $< -o $@

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -c $< -o $@

$(BUILD)/tests/%.o: tests/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -c $< -o $@

$(BUILD)/unit_tests: $(TEST_OBJS) $(CORE_OBJS) $(PLAT_OBJ)
	$(CXX) $^ -o $@ -lz

test: $(BUILD)/unit_tests
	./$(BUILD)/unit_tests

$(BUILD)/mcserver: $(BUILD)/main_host.o $(CORE_OBJS) $(PLAT_OBJ)
	$(CXX) $^ -o $@

server: $(BUILD)/mcserver

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
.PHONY: all test server clean
