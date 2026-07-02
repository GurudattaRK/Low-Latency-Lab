# TeraCrypt build file.
# x86_64 Linux, clang only (uses clang-specific attributes/pragmas and LLVM PGO).
# Requires: clang, lld, liburing (dev headers + lib), llvm-profdata (for `make pgo` only).

CC := clang
LD := clang

TARGET := teracrypt

# All code is inlined via crypto_core.h -- only main.c needs compiling.
SRCS := main.c
OBJS := $(SRCS:.c=.o)

WARN       := -Wall -Wextra
ARCH       := -march=native -mtune=native
# _GNU_SOURCE is required for CPU_ZERO/CPU_SET/pthread_setaffinity_np
# (platform.c) -- -std=gnu11 only enables GNU *language* extensions, it does
# NOT define _GNU_SOURCE for glibc's feature-test macros. Without this,
# clang fails with "call to undeclared function 'CPU_ZERO'" etc.
BASE_CFLAGS := -std=gnu11 -D_GNU_SOURCE -pthread $(WARN) $(ARCH) -mavx2
OPT_CFLAGS  := -Ofast -ffast-math -flto=full -fno-omit-frame-pointer -funroll-loops -fno-plt -fno-math-errno

# Overridable from the command line / recursive `make` calls (used by the
# `pgo` target below to build instrumented, then re-optimized, binaries).
CFLAGS  ?= $(BASE_CFLAGS) $(OPT_CFLAGS)
LDFLAGS ?= -flto -fuse-ld=lld -pthread
LIBS    := -luring

PGO_DIR         := .pgo
PGO_PROFILE     := $(PGO_DIR)/merged.profdata
PGO_TRAIN_KEY   := $(PGO_DIR)/train_key.bin
PGO_TRAIN_SMALL := $(PGO_DIR)/train_small.bin   # below PARALLEL_THRESHOLD -> single-core path
PGO_TRAIN_LARGE := $(PGO_DIR)/train_large.bin   # above PARALLEL_THRESHOLD -> forked/parallel path

.PHONY: all clean pgo

all: $(TARGET)

$(TARGET): $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f *.o $(TARGET) $(TARGET)_instrumented
	rm -rf $(PGO_DIR)

# ---------------------------------------------------------------------------
# PGO: instrument -> train (small + large, encrypt + decrypt) -> merge -> optimize
# Not needed for day-to-day benchmarking (`bench.sh --skip-pgo` skips this
# entirely) but kept working here in case you want to come back to it later.
# ---------------------------------------------------------------------------
pgo:
	@echo "==> PGO 1/4: instrumented build"
	$(MAKE) clean
	$(MAKE) all \
		CFLAGS="$(BASE_CFLAGS) -Ofast -ffast-math -fprofile-generate -DPGO_INSTRUMENTED" \
		LDFLAGS="-fprofile-generate -fuse-ld=lld -pthread"
	mv $(TARGET) $(TARGET)_instrumented
	@echo "==> PGO 2/4: training runs"
	mkdir -p $(PGO_DIR)
	head -c 128  /dev/urandom > $(PGO_TRAIN_KEY)
	head -c 16M  /dev/urandom > $(PGO_TRAIN_SMALL)
	head -c 192M /dev/urandom > $(PGO_TRAIN_LARGE)
	LLVM_PROFILE_FILE="$(PGO_DIR)/raw-%p.profraw" LLVM_PROFILE_RUNTIME_COMPRESSION=none ./$(TARGET)_instrumented encrypt $(PGO_TRAIN_SMALL) $(PGO_DIR)/small.enc $(PGO_TRAIN_KEY)
	LLVM_PROFILE_FILE="$(PGO_DIR)/raw-%p.profraw" LLVM_PROFILE_RUNTIME_COMPRESSION=none ./$(TARGET)_instrumented decrypt $(PGO_DIR)/small.enc  $(PGO_DIR)/small.dec $(PGO_TRAIN_KEY)
	LLVM_PROFILE_FILE="$(PGO_DIR)/raw-%p.profraw" LLVM_PROFILE_RUNTIME_COMPRESSION=none ./$(TARGET)_instrumented encrypt $(PGO_TRAIN_LARGE) $(PGO_DIR)/large.enc $(PGO_TRAIN_KEY)
	LLVM_PROFILE_FILE="$(PGO_DIR)/raw-%p.profraw" LLVM_PROFILE_RUNTIME_COMPRESSION=none ./$(TARGET)_instrumented decrypt $(PGO_DIR)/large.enc  $(PGO_DIR)/large.dec $(PGO_TRAIN_KEY)
	@echo "==> PGO 3/4: merging profiles"
	llvm-profdata merge -output=$(PGO_PROFILE) $(PGO_DIR)/raw-*.profraw
	@echo "==> PGO 4/4: optimized rebuild using merged profile"
	rm -f *.o $(TARGET)
	$(MAKE) all \
		CFLAGS="$(BASE_CFLAGS) $(OPT_CFLAGS) -fprofile-use=$(PGO_PROFILE)"
	rm -f $(PGO_DIR)/*.profraw $(PGO_DIR)/*.bin $(PGO_DIR)/*.enc $(PGO_DIR)/*.dec
	@echo "==> PGO build complete: ./$(TARGET) (profile kept at $(PGO_PROFILE))"