# --- Build Mode Configuration ---
BUILD_MODE ?= debug

ifeq ($(BUILD_MODE), release)
    MODE_FLAGS = -O3
    RELEASE_LDFLAGS = -s
else
    MODE_FLAGS = -O0 -g
    RELEASE_LDFLAGS =
endif

# --- Compiler & Flags ---
CXX = g++
DEPFLAGS = -MMD -MP
CXXFLAGS = $(MODE_FLAGS) -std=c++20 \
           -I./include \
           -I./src \
           -I$(JAVA_HOME)/include \
           -I$(JAVA_HOME)/include/linux \
           $(DEPFLAGS)

# --- Library Paths & Linker Flags ---
LDFLAGS = $(RELEASE_LDFLAGS) -lcrypto -lssl -lzip -lpthread -landroid-spawn

# --- Directories & Targets ---
BUILD_DIR = build/$(BUILD_MODE)
OBJ_DIR   = $(BUILD_DIR)/obj
TARGET    = $(BUILD_DIR)/mkapk

# --- Source Discovery ---
SRCS := $(shell find src -type f -name "*.cpp")
OBJS := $(patsubst src/%.cpp, $(OBJ_DIR)/%.o, $(SRCS))
DEPS := $(OBJS:.o=.d)

.PHONY: all clean clean-all

all: $(TARGET)

# 1. Link the final binary
$(TARGET): $(OBJS)
	@echo ">> Linking Final Binary ($(BUILD_MODE))..."
	@mkdir -p $(dir $@)
	@$(CXX) $(OBJS) -o $@ $(LDFLAGS)

# 2. Universal Pattern Rule (automatically recreates nested subdirectories in obj/)
$(OBJ_DIR)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	@echo "   [CXX] $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

# 3. Include auto-generated header dependency rules (.d)
-include $(DEPS)

# --- Clean Targets ---
clean:
	@echo ">> Cleaning $(BUILD_MODE) build artifacts..."
	@rm -rf $(BUILD_DIR)

clean-all:
	@echo ">> Cleaning all build artifacts..."
	@rm -rf build
