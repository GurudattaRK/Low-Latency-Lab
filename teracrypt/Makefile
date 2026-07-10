# TeraCrypt is Linux + clang only - it leans on io_uring and clang's PGO, so
# there's no point pretending it's portable like the reference project.
# You'll need: clang, lld, liburing, and llvm-profdata if you want `make pgo`.

CC := clang
LD := clang

TARGET := teracrypt

# one translation unit - the crypto lives in crypto_core.h so it all inlines
SRCS := main.c
OBJS := $(SRCS:.c=.o)

WARN       := -Wall -Wextra
ARCH       := -march=native -mtune=native
# _GNU_SOURCE is what unlocks CPU_ZERO/CPU_SET, O_DIRECT, MADV_HUGEPAGE and friends
BASE_CFLAGS := -std=gnu11 -D_GNU_SOURCE -pthread $(WARN) $(ARCH) -mavx2
OPT_CFLAGS  := -Ofast -flto=full -fno-omit-frame-pointer -fno-plt -fno-math-errno

# ?= so the pgo target can swap these out without editing the file
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

# The four-step PGO dance. Builds a counter-instrumented binary, runs it on a
# small + a large random file (both directions) to collect a realistic profile,
# merges the raw counters, then rebuilds for real using them. Worth it - the
# branch layout in the hot loop gets noticeably better.
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