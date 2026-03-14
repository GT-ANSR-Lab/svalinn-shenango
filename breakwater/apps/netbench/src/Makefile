# Makefile for netbench
ROOT_PATH=../../../../
BUILD_PATH = ../build
include $(ROOT_PATH)/build/shared.mk

INC += -I./
INC += -I$(ROOT_PATH)/bindings
INC += -I$(ROOT_PATH)/breakwater/inc
INC += -I$(ROOT_PATH)/breakwater/bindings/cc/inc
INC += -I$(ROOT_PATH)/m-semaphore/inc
INC += -I$(ROOT_PATH)/m-semaphore/utils
INC += -I$(ROOT_PATH)/m-semaphore/bindings
INC += -I$(ROOT_PATH)/m-semaphore/deps/pcm/src

librt = $(ROOT_PATH)/bindings/cc/librt++.a
libbw = $(ROOT_PATH)/breakwater/libbw.a
libbwpp = $(ROOT_PATH)/breakwater/bindings/cc/libbw++.a
libmsem = $(ROOT_PATH)/m-semaphore/build/libmsem.a
libpcm = $(ROOT_PATH)/m-semaphore/deps/pcm/build/src/libpcm.a

lib_src = synthetic_worker.cc util.cc
lib_obj = $(patsubst %.cc,$(BUILD_PATH)/%.o,$(lib_src))

netbench_src = netbench.cc
netbench_obj = $(BUILD_PATH)/$(netbench_src:.cc=.o)
netbench_tgt = $(BUILD_PATH)/netbench

netbench_ms_src = netbench_multi_server.cc
netbench_ms_obj = $(BUILD_PATH)/$(netbench_ms_src:.cc=.o)
netbench_ms_tgt = $(BUILD_PATH)/netbench_ms


RUNTIME_LIBS := $(RUNTIME_LIBS) -lnuma -lm

# must be first
all: $(netbench_tgt) $(netbench_ms_tgt)

$(netbench_tgt): $(lib_obj) $(netbench_obj) $(librt) $(libbw) $(libbwpp) $(libmsem) $(libpcm) $(RUNTIME_DEPS)
	mkdir -p $(BUILD_PATH)
	$(LDXX) -o $@ $(LDFLAGS) $(lib_obj) $(netbench_obj) \
	$(librt) $(libbw) $(libbwpp) $(libmsem) $(libpcm) $(RUNTIME_LIBS)

$(netbench_ms_tgt): $(lib_obj) $(netbench_ms_obj) $(librt) $(libbw) $(libbwpp) $(libmsem) $(libpcm) $(RUNTIME_DEPS)
	mkdir -p $(BUILD_PATH)
	$(LDXX) -o $@ $(LDFLAGS) $(lib_obj) $(netbench_ms_obj) \
	$(librt) $(libbw) $(libbwpp) $(libmsem) $(libpcm) $(RUNTIME_LIBS)

# general build rules for all targets
src = $(lib_src) $(netbench_src) $(netbench_ms_src)
obj = $(BUILD_PATH)/$(src:.cc=.o)
dep = $(BUILD_PATH)/$(obj:.o=.d)

ifneq ($(MAKECMDGOALS),clean)
-include $(dep)   # include all dep files in the makefile
endif

# rule to generate a dep file by using the C preprocessor
# (see man cpp for details on the -MM and -MT options)
$(BUILD_PATH)/%.d: %.cc
	mkdir -p $(BUILD_PATH)
	@$(CXX) $(CXXFLAGS) $< -MM -MT $(@:.d=.o) >$@
$(BUILD_PATH)/%.o: %.cc
	mkdir -p $(BUILD_PATH)
	$(CXX) $(CXXFLAGS) -c $< -o $@

.PHONY: clean
clean:
	rm -rf $(BUILD_PATH)
