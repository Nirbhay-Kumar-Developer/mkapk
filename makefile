# --- Build Mode Configuration ---
BUILD_MODE ?= debug

ifeq ($(BUILD_MODE), release)
    MODE_FLAGS = -O3
else
    MODE_FLAGS = -O0 -g
endif

# --- Compiler & Flags ---
CXX = g++
DEPFLAGS = -MMD -MP
CXXFLAGS = $(MODE_FLAGS) -std=c++20 -I./src -I$(JAVA_HOME)/include -I$(JAVA_HOME)/include/linux -I./include $(DEPFLAGS)

# --- Library Paths ---
JVM_LIB_PATH = $(JAVA_HOME)/lib/server

LDFLAGS = -L$(JVM_LIB_PATH) \
          -Wl,-rpath=$(JVM_LIB_PATH) \
          -ljvm -lcrypto -lssl -lzip -lpthread -landroid-spawn

# --- Directories ---
BUILD_DIR = build/$(BUILD_MODE)
OBJ_DIR   = $(BUILD_DIR)/obj
TARGET    = $(BUILD_DIR)/mkapk

# --- Deep Discovery Source Engine ---
ALL_SRCS      := $(shell find src -type f -name "*.cpp")
MAIN_SRC      := src/main.cpp

CORE_SRCS     := $(filter-out $(MAIN_SRC), $(ALL_SRCS))
PROVIDER_SRCS := $(filter src/modules/%, $(CORE_SRCS))
HELPER_SRCS   := $(filter-out src/modules/%, $(CORE_SRCS))

# --- Object Mapping ---
PROVIDER_OBJS := $(patsubst src/%.cpp, $(OBJ_DIR)/%.o, $(PROVIDER_SRCS))
HELPER_OBJS   := $(patsubst src/%.cpp, $(OBJ_DIR)/%.o, $(HELPER_SRCS))
MAIN_OBJ      := $(OBJ_DIR)/main.o

ALL_OBJS      := $(MAIN_OBJ) $(HELPER_OBJS) $(PROVIDER_OBJS)
DEPS          := $(ALL_OBJS:.o=.d)

FINAL_PROVIDER_OBJ = $(OBJ_DIR)/providers.o
FINAL_HELPER_OBJ   = $(OBJ_DIR)/helpers.o

.PHONY: all clean clean-all package_objs

# Build target binary and packaged mega-objects
all: $(TARGET) package_objs

# 1. Link the final binary using all discovered objects
$(TARGET): $(ALL_OBJS)
	@echo ">> Linking Final Binary with RPATH ($BUILD_MODE)..."
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(ALL_OBJS) -o $@ $(LDFLAGS)

# 2. Partial Linking
package_objs: $(FINAL_PROVIDER_OBJ) $(FINAL_HELPER_OBJ)
	@echo ">> Mega-objects ready for packaging ($BUILD_MODE)."

$(FINAL_PROVIDER_OBJ): $(PROVIDER_OBJS)
	@echo ">> Creating combined Providers object..."
	@mkdir -p $(dir $@)
	ld -r $^ -o $@

$(FINAL_HELPER_OBJ): $(HELPER_OBJS)
	@echo ">> Creating combined Helpers object..."
	@mkdir -p $(dir $@)
	ld -r $^ -o $@

# 3. Universal Pattern Rule
$(OBJ_DIR)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	@echo "   [CXX] $<"
	$(CXX) $(CXXFLAGS) -c $< -o $@

# 4. Include automatic header dependencies
-include $(DEPS)

# Clean specific build mode
clean:
	@echo ">> Cleaning $(BUILD_MODE) build artifacts..."
	rm -rf $(BUILD_DIR)

# Clean all modes entirely
clean-all:
	@echo ">> Cleaning all build artifacts..."
	rm -rf build