# Makefile for rocksdb
ROOT_PATH = ../../../
BUILD_PATH = ./build
include $(ROOT_PATH)/build/shared.mk

INC += -I$(ROOT_PATH)/bindings
INC += -I$(ROOT_PATH)/breakwater/inc/
INC += -I$(ROOT_PATH)/breakwater/bindings/cc/inc/
INC += -Ideps/rocksdb/include
INC += -I$(ROOT_PATH)/m-semaphore/inc
INC += -I$(ROOT_PATH)/m-semaphore/utils
INC += -I$(ROOT_PATH)/m-semaphore/bindings
INC += -I$(ROOT_PATH)/m-semaphore/deps/pcm/src

LIBS += -lsnappy -lz -llz4 -lbz2 -lzstd -lpthread -ljemalloc -ldl -lnuma -lm

lib_rocksdb = deps/rocksdb/librocksdb.a
# lib_shim = $(ROOT_PATH)/shim/libshim.a
librt_libs = $(ROOT_PATH)/bindings/cc/librt++.a
libbw = $(ROOT_PATH)/breakwater/libbw.a
libbwpp = $(ROOT_PATH)/breakwater/bindings/cc/libbw++.a
libmsem = $(ROOT_PATH)/m-semaphore/build/libmsem.a
libpcm = $(ROOT_PATH)m-semaphore/deps/pcm/build/src/libpcm.a


rocksdb_server_src = rocksdb_server.cc
rocksdb_server_obj = $(BUILD_PATH)/$(rocksdb_server_src:.cc=.o)
rocksdb_server_tgt = $(BUILD_PATH)/rocksdb_server

rocksdb_client_src = rocksdb_client.cc
rocksdb_client_obj = $(BUILD_PATH)/$(rocksdb_client_src:.cc=.o)
rocksdb_client_tgt = $(BUILD_PATH)/rocksdb_client

rocksdb_createdb_src = rocksdb_createdb.cc
rocksdb_createdb_obj = $(BUILD_PATH)/$(rocksdb_createdb_src:.cc=.o)
rocksdb_createdb_tgt = $(BUILD_PATH)/rocksdb_createdb

src = $(rocksdb_server_src) $(rocksdb_client_src) $(rocksdb_createdb_src)
obj = $(rocksdb_server_obj) $(rocksdb_client_obj) $(rocksdb_createdb_obj)
dep = $(obj:.o=.d)

# must be first
all: $(rocksdb_server_tgt) $(rocksdb_client_tgt) $(rocksdb_createdb_tgt)

server: $(rocksdb_server_tgt) $(rocksdb_createdb_tgt)

client: $(rocksdb_client_tgt)

$(rocksdb_server_tgt): $(rocksdb_server_obj) $(lib_rocksdb) $(lib_shim) $(librt_libs) $(libbw) $(libbwpp) $(libmsem) $(libpcm) $(RUNTIME_DEPS) $(LIBS)
	mkdir -p $(BUILD_PATH)
	$(LDXX) -o $@ $(LDFLAGS) -ldl $(rocksdb_server_obj) $(lib_rocksdb) $(lib_shim) $(librt_libs) $(libbw) $(libbwpp) $(libmsem) $(libpcm) $(RUNTIME_LIBS) $(LIBS)

$(rocksdb_client_tgt): $(rocksdb_client_obj) $(lib_shim) $(librt_libs) $(libbw) $(libbwpp) $(RUNTIME_DEPS) $(LIBS)
	mkdir -p $(BUILD_PATH)
	$(LDXX) -o $@ $(LDFLAGS) -ldl $(rocksdb_client_obj) $(lib_shim) $(librt_libs) $(libbw) $(libbwpp) $(RUNTIME_LIBS) $(LIBS)

$(rocksdb_createdb_tgt): $(rocksdb_createdb_obj) $(lib_rocksdb) $(lib_shim) $(librt_libs) $(libbw) $(libbwpp) $(RUNTIME_DEPS) $(LIBS)
	mkdir -p $(BUILD_PATH)
	$(LDXX) -o $@ $(LDFLAGS) -ldl $(rocksdb_createdb_obj) $(lib_rocksdb) $(lib_shim) $(librt_libs) $(libbw) $(libbwpp) $(RUNTIME_LIBS) $(LIBS)

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
