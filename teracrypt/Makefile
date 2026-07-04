# TeraCrypt build file. x86_64 Linux, clang only.
# Requires: clang, lld, liburing, llvm-profdata (for pgo).

CC := clang
LD := clang

TARGET := teracrypt

# All code is inlined via crypto_core.h
SRCS := main.c
OBJS := $(SRCS:.c=.o)

WARN       := -Wall -Wextra
ARCH       := -march=native -mtune=native
# _GNU_SOURCE for CPU_ZERO/CPU_SET etc.
BASE_CFLAGS := -std=gnu11 -D_GNU_SOURCE -pthread $(WARN) $(ARCH) -mavx2
OPT_CFLAGS  := -Ofast -flto=full -fno-omit-frame-pointer -fno-plt -fno-math-errno

# Overridable (used by pgo target)
CFLAGS  ?= $(BASE_CFLAGS) $(OPT_CFLAGS)
LDFLAGS ?= -fuse-ld=lld -pthread
LIBS    := -luring

PGO_DIR         := .pgo
PGO_PROFILE     := $(PGO_DIR)/merged.profdata
PGO_TRAIN_KEY   := $(PGO_DIR)/train_key.bin
PGO_TRAIN_SMALL := $(PGO_DIR)/train_small.bin
PGO_TRAIN_LARGE := $(PGO_DIR)/train_large.bin

.PHONY: all clean pgo

all: $(TARGET)

$(TARGET): $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f *.o $(TARGET) $(TARGET)_instrumented
	rm -rf $(PGO_DIR)

# PGO: instrument -> train -> merge -> optimize
pgo:
	@echo "==> PGO 1/4: instrumented build"
	$(MAKE) clean
	$(MAKE) all \
		CFLAGS="$(BASE_CFLAGS) -Ofast -fprofile-generate -DPGO_INSTRUMENTED" \
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