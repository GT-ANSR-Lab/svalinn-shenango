ROOT_PATH = ../
BUILD_PATH=./build

include $(ROOT_PATH)/build/shared.mk

# Type of msemaphore (select only one)
CFLAGS += -DM_SEM_MAB_EPSILON_GREEDY
# CFLAGS += -DM_SEM_MAB_THOMPSON_SAMPLING

# Debug build
# CFLAGS += -DM_SEM_DEBUG

# Includes
CFLAGS += -I./inc
CFLAGS += -I./deps/pcm/src
CFLAGS += -I$(ROOT_PATH)/bindings
CFLAGS += -I./bindings
CFLAGS += -I./utils

# Core files
msem_src = $(wildcard src/*.cpp)
msem_obj = $(patsubst src/%.cpp, $(BUILD_PATH)/%.o, $(msem_src))

# Utility files
msem_utils_src = $(wildcard utils/*.cpp)
msem_utils_obj = $(patsubst utils/%.cpp, $(BUILD_PATH)/%.o, $(msem_utils_src))
msem_obj += $(msem_utils_obj)

# C Bindings
msem_c_bindings_src = $(wildcard bindings/c/*.cpp)
msem_c_obj = $(patsubst bindings/c/%.cpp, $(BUILD_PATH)/%.o, $(msem_c_bindings_src))
msem_obj += $(msem_c_obj)

# Final target (static library)
msem_tgt = $(BUILD_PATH)/libmsem.a

all: $(msem_tgt)

$(msem_tgt): $(msem_obj)
	$(AR) rcs $@ $^

# general build rules for all targets
src = $(msem_src) $(msem_c_bindings_src)
obj = $(msem_obj)
dep = $(patsubst $(BUILD_PATH)/%.o, $(BUILD_PATH)/%.d, $(msem_obj))

ifneq ($(MAKECMDGOALS),clean)
-include $(dep)		# include all dep files in the makefile
endif

# rule to generate a dep file by using the C preprocessor
$(BUILD_PATH)/%.d: src/%.cpp
	mkdir -p $(BUILD_PATH)
	@$(CXX) $(CFLAGS) $< -MM -MT $(@:.d=.o) >$@
$(BUILD_PATH)/%.o: src/%.cpp
	mkdir -p $(BUILD_PATH)
	@$(CXX) $(CFLAGS) -c $< -o $@
$(BUILD_PATH)/%.d: utils/%.cpp
	mkdir -p $(BUILD_PATH)
	@$(CXX) $(CFLAGS) $< -MM -MT $(@:.d=.o) >$@
$(BUILD_PATH)/%.o: utils/%.cpp
	mkdir -p $(BUILD_PATH)
	@$(CXX) $(CFLAGS) -c $< -o $@
$(BUILD_PATH)/%.d: bindings/c/%.cpp
	mkdir -p $(BUILD_PATH)
	@$(CXX) $(CFLAGS) $< -MM -MT $(@:.d=.o) >$@
$(BUILD_PATH)/%.o: bindings/c/%.cpp
	mkdir -p $(BUILD_PATH)
	@$(CXX) $(CFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -rf $(BUILD_PATH)

.PHONY: submodules
submodules:
	bash build_deps.sh
