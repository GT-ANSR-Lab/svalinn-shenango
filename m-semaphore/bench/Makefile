# Compiler
CXX := g++

# Source file
SRC := msem_bench.cc

# Output binary
OUT := msem_bench

# Include directories
INCLUDES := -I../deps/benchmark/include \
            -I../deps/pcm/src \
            -I../inc

# Library files
LIBS := ../deps/benchmark/build/src/libbenchmark.a \
        ../deps/pcm/build/src/libpcm.a \
        -lnuma -lm -pthread

# Default target
all: $(OUT)

# Build rule
$(OUT): $(SRC)
	# $(CXX) -O3 -mssse3 $(CXXFLAGS) $(SRC) $(LIBS) $(INCLUDES) -o $@
	$(CXX) $(CXXFLAGS) $(SRC) $(LIBS) $(INCLUDES) -o $@
# Clean
.PHONY: clean
clean:
	rm -f $(OUT)
