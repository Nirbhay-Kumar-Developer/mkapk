# --- Compiler & Flags ---
CXX = g++
CXXFLAGS = -O3 -std=c++20 -I./src -I$(JAVA_HOME)/include -I$(JAVA_HOME)/include/linux -I./include

# --- Library Paths ---
JVM_LIB_PATH = $(JAVA_HOME)/lib/server

LDFLAGS = -L$(JVM_LIB_PATH) \
          -Wl,-rpath=$(JVM_LIB_PATH) \
          -ljvm -lcrypto -lssl -lzip -lpthread -landroid-spawn

# --- Directories ---
BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj
TARGET = $(BUILD_DIR)/mkapk

# --- Deep Discovery Source Engine ---
ALL_SRCS      := $(shell find src -type f -name "*.cpp")
MAIN_SRC      := src/main.cpp

CORE_SRCS     := $(filter-out $(MAIN_SRC), $(ALL_SRCS))
PROVIDER_SRCS := $(filter src/modules/%, $(CORE_SRCS))
HELPER_SRCS   := $(filter-out src/modules/%, $(CORE_SRCS))

# --- Object Mapping (FIXED: Preserves deep nested file hierarchy maps) ---
PROVIDER_OBJS := $(patsubst src/%.cpp, $(OBJ_DIR)/%.o, $(PROVIDER_SRCS))
HELPER_OBJS   := $(patsubst src/%.cpp, $(OBJ_DIR)/%.o, $(HELPER_SRCS))
MAIN_OBJ      := $(OBJ_DIR)/main.o

ALL_OBJS      := $(MAIN_OBJ) $(HELPER_OBJS) $(PROVIDER_OBJS)

FINAL_PROVIDER_OBJ = $(OBJ_DIR)/providers.o
FINAL_HELPER_OBJ   = $(OBJ_DIR)/helpers.o

all: $(TARGET)

# 1. Link the final binary using all discovered objects
$(TARGET): $(ALL_OBJS)
	@echo ">> Linking Final Binary with RPATH..."
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(ALL_OBJS) -o $@ $(LDFLAGS)
	@$(MAKE) package_objs

# 2. Partial Linking
package_objs: $(FINAL_PROVIDER_OBJ) $(FINAL_HELPER_OBJ)
	@echo ">> Mega-objects ready for packaging."

$(FINAL_PROVIDER_OBJ): $(PROVIDER_OBJS)
	@echo ">> Creating combined Providers object..."
	ld -r $^ -o $@

$(FINAL_HELPER_OBJ): $(HELPER_OBJS)
	@echo ">> Creating combined Helpers object..."
	ld -r $^ -o $@

# 3. Universal Pattern Rule (FIXED: Correctly targets deep hierarchies like modules/dependency/%.o)
$(OBJ_DIR)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	@echo "   [CXX] $<"
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	@echo ">> Cleaning build artifacts..."
	rm -rf $(BUILD_DIR)

.PHONY: all clean package_objs