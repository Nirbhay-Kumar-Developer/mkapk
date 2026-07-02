# --- Compiler & Flags ---
CXX = g++
CXXFLAGS = -O3 -std=c++20 -I./src -I$(JAVA_HOME)/include -I$(JAVA_HOME)/include/linux -I./include

# --- Library Paths ---
# We define the JVM lib path as a variable to keep the linking line clean
JVM_LIB_PATH = $(JAVA_HOME)/lib/server

# LDFLAGS now includes -Wl,-rpath which bakes the search path into the ELF binary
LDFLAGS = -L$(JVM_LIB_PATH) \
          -Wl,-rpath=$(JVM_LIB_PATH) \
          -ljvm -lcrypto -lssl -lzip -lpthread -landroid-spawn

# --- Directories ---
BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj
TARGET = $(BUILD_DIR)/mkapk

# --- Source Discovery ---
PROVIDER_SRCS = $(wildcard src/modules/*.cpp)
HELPER_SRCS = src/mkapk_env.cpp src/mkapk_helper.cpp src/mkapk_compiler.cpp
MAIN_SRC = src/main.cpp

# --- Object Mapping ---
PROVIDER_OBJS = $(patsubst src/modules/%.cpp, $(OBJ_DIR)/modules/%.o, $(PROVIDER_SRCS))
HELPER_OBJS = $(patsubst src/%.cpp, $(OBJ_DIR)/%.o, $(HELPER_SRCS))
MAIN_OBJ = $(OBJ_DIR)/main.o

FINAL_PROVIDER_OBJ = $(OBJ_DIR)/providers.o
FINAL_HELPER_OBJ = $(OBJ_DIR)/helpers.o

all: $(TARGET)

# 1. Link the final binary using all individual objects
$(TARGET): $(MAIN_OBJ) $(HELPER_OBJS) $(PROVIDER_OBJS)
	@echo ">> Linking Final Binary with RPATH..."
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)
	@$(MAKE) package_objs

# 2. Partial Linking (Creates the single .o files for your /usr/lib/mkapk)
package_objs: $(FINAL_PROVIDER_OBJ) $(FINAL_HELPER_OBJ)
	@echo ">> Mega-objects ready for packaging."

$(FINAL_PROVIDER_OBJ): $(PROVIDER_OBJS)
	@echo ">> Creating combined Providers object..."
	ld -r $^ -o $@

$(FINAL_HELPER_OBJ): $(HELPER_OBJS)
	@echo ">> Creating combined Helpers object..."
	ld -r $^ -o $@

# 3. Pattern Rules for Individual Compilation
$(OBJ_DIR)/modules/%.o: src/modules/%.cpp
	@mkdir -p $(dir $@)
	@echo "   [CXX] $<"
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	@echo "   [CXX] $<"
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	@echo ">> Cleaning build artifacts..."
	rm -rf $(BUILD_DIR)