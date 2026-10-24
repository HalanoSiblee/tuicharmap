CXX      := g++
PROFILE  ?= release

# Profile-specific flags and binary names
ifeq ($(PROFILE),debug)
  CXXFLAGS := -std=c++17 -Wall -Wextra -pthread -g -O0 -DDEBUG
  LDFLAGS  := -pthread
  BIN      := tuicharmap-debug
else
  CXXFLAGS := -std=c++17 -Wall -Wextra -pthread -O3 -DNDEBUG -flto
  LDFLAGS  := -pthread -flto -s
  BIN      := tuicharmap
endif

# Dependencies via pkg-config
PKG_CONFIG := pkg-config
ifneq ($(shell which $(PKG_CONFIG) 2>/dev/null),)
  CXXFLAGS += $(shell $(PKG_CONFIG) --cflags ncursesw icu-uc)
  LDFLAGS  += $(shell $(PKG_CONFIG) --libs ncursesw icu-uc)
else
  CXXFLAGS += -I/usr/include/ncursesw
  LDFLAGS  += -lncursesw -licuuc -licudata
endif

SRCS      := src/main.cpp src/unicode.cpp src/clipboard.cpp src/search.cpp
BUILD_DIR := build/$(PROFILE)
OBJS      := $(patsubst src/%.cpp, $(BUILD_DIR)/%.o, $(SRCS))

.PHONY: all clean debug release

# Default target
all: $(BIN)

# Aliases that explicitly call the 'all' target to avoid infinite recursion
debug:
	@$(MAKE) PROFILE=debug all

release:
	@$(MAKE) PROFILE=release all

$(BIN): $(OBJS)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS)

# Compile rule ensuring the build directory exists
$(BUILD_DIR)/%.o: src/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

clean:
	rm -rf build tuicharmap tuicharmap-debug
