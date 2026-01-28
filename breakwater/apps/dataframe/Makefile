# Makefile for dataframe
ROOT_PATH = ../../../
BUILD_PATH = build
include $(ROOT_PATH)/build/shared.mk

INC += -I$(ROOT_PATH)/bindings
INC += -I$(ROOT_PATH)/breakwater/inc/
INC += -I$(ROOT_PATH)/breakwater/bindings/cc/inc/
INC += -Ideps/DataFrame/include
INC += -I$(ROOT_PATH)/m-semaphore/inc
INC += -I$(ROOT_PATH)/m-semaphore/bindings
INC += -I$(ROOT_PATH)/m-semaphore/deps/pcm/src

LIBS += -lpthread -ldl -lnuma -lm

lib_df = deps/DataFrame/Release/lib/libDataFrame.a
lib_shim = $(ROOT_PATH)/shim/libshim.a
lib_rt = $(ROOT_PATH)/bindings/cc/librt++.a
libbw = $(ROOT_PATH)/breakwater/libbw.a
libbwpp = $(ROOT_PATH)/breakwater/bindings/cc/libbw++.a
libmsem = $(ROOT_PATH)/m-semaphore/build/libmsem.a
libpcm = $(ROOT_PATH)m-semaphore/deps/pcm/build/src/libpcm.a

df_server_src = dataframe_server.cc
df_server_obj = $(BUILD_PATH)/$(df_server_src:.cc=.o)
df_server_tgt = $(BUILD_PATH)/dataframe_server

df_client_src = dataframe_client.cc
df_client_obj = $(BUILD_PATH)/$(df_client_src:.cc=.o)
df_client_tgt = $(BUILD_PATH)/dataframe_client

src = $(df_server_src) $(df_client_src)
obj = $(df_server_obj) $(df_client_obj)
dep = $(obj:.o=.d)

# must be first
all: $(df_server_tgt) $(df_client_tgt)

server: $(df_server_tgt)

client: $(df_client_tgt)

$(df_server_tgt): $(df_server_obj) $(lib_df) $(lib_shim) $(lib_rt) $(libbw) $(libbwpp) $(libmsem) $(libpcm) $(RUNTIME_DEPS) $(LIBS)
	mkdir -p $(BUILD_PATH)
	$(LDXX) -o $@ $(LDFLAGS) -ldl $(df_server_obj) $(lib_df) $(lib_shim) $(lib_rt) $(libbw) $(libbwpp) $(libmsem) $(libpcm) $(RUNTIME_LIBS) $(LIBS)

$(df_client_tgt): $(df_client_obj) $(lib_shim) $(lib_rt) $(libbw) $(libbwpp) $(RUNTIME_DEPS) $(LIBS)
	mkdir -p $(BUILD_PATH)
	$(LDXX) -o $@ $(LDFLAGS) -ldl $(df_client_obj) $(lib_shim) $(lib_rt) $(libbw) $(libbwpp) $(RUNTIME_LIBS) $(LIBS)

ifneq ($(MAKECMDGOALS),clean)
-include $(dep)   # include all dep files in the makefile
endif

$(BUILD_PATH)/%.d: %.cc
	mkdir -p $(BUILD_PATH)
	@$(CXX) $(CXXFLAGS) -g $< -MM -MT $(@:.d=.o) >$@
$(BUILD_PATH)/%.o: %.cc
	mkdir -p $(BUILD_PATH)
	$(CXX) $(CXXFLAGS) -g -c $< -o $@
$(BUILD_PATH)/%.o: %.c
	mkdir -p $(BUILD_PATH)
	$(CC) $(CFLAGS) -g -c $< -o $@

.PHONY: clean
clean:
	rm -rf $(BUILD_PATH)
