# Cross-platform Makefile
# GCC and Clang. Linux, macOS, Windows (MSYS2/MinGW).
# x86_64, i386, aarch64, riscv64.
#
# make / make pgo / make clean / make info

SRC    := main.c
TARGET := main

# Detect OS
UNAME := $(shell uname -s)
RM    := rm -f

ifeq ($(UNAME),Darwin)
    PLATFORM := macos
else ifneq ($(findstring MINGW,$(UNAME)),)
    PLATFORM := windows
else ifneq ($(findstring MSYS,$(UNAME)),)
    PLATFORM := windows
else
    PLATFORM := linux
endif

# Detect arch (normalize arm64 -> aarch64)
ARCH := $(shell uname -m)
ifeq ($(ARCH),arm64)
    ARCH := aarch64
endif

# Clang default, override with make CC=gcc
CC ?= clang

CC_IS_CLANG := $(shell $(CC) --version 2>&1 | grep -c clang)

# PicoPerf only on x86/x64 Linux
USE_PICOPERF := 0
ifeq ($(PLATFORM),linux)
    ifneq ($(filter $(ARCH),x86_64 i686 i386),)
        USE_PICOPERF := 1
    endif
endif

PICOPERF_SRC := ../../PicoPerf/picoperf.c
PICOPERF_INC := ../../PicoPerf

# Build flags
OPT_FLAGS  := -Ofast
WARN_FLAGS := -Wall -Wextra
LTO_FLAGS  := -flto
EXTRA_FLAGS := -funroll-loops -fomit-frame-pointer -fno-semantic-interposition

# -march=native can fail on some RISC-V toolchains
ifeq ($(ARCH),riscv64)
    ARCH_FLAGS :=
else
    ARCH_FLAGS := -march=native
endif

CFLAGS := $(OPT_FLAGS) $(ARCH_FLAGS) $(LTO_FLAGS) $(EXTRA_FLAGS) $(WARN_FLAGS) \
          -DUSE_PICOPERF=$(USE_PICOPERF)

# Source files depend on whether PicoPerf is used.
ifeq ($(USE_PICOPERF),1)
    ALL_SRC   := $(SRC) $(PICOPERF_SRC)
    INC_FLAGS := -I$(PICOPERF_INC)
else
    ALL_SRC   := $(SRC)
    INC_FLAGS :=
endif

# PGO flags differ between Clang and GCC
ifeq ($(CC_IS_CLANG),1)
    PGO_DIR := pgo_profiles
    PGO_GEN := -fprofile-instr-generate=$(PGO_DIR)/profile.clangraw
    PGO_USE := -fprofile-instr-use=$(PGO_DIR)/profile.clangprofdata
else
    PGO_GEN := -fprofile-generate
    PGO_USE := -fprofile-use -fprofile-correction -Wno-missing-profile
endif

# ── Targets ─────────────────────────────────────────────────────────────────
.PHONY: all clean info pgo profile

all:
	$(CC) $(CFLAGS) $(INC_FLAGS) -o $(TARGET) $(ALL_SRC)

# Profile build: symbols on, PicoPerf off
profile:
	$(CC) -g -Ofast $(ARCH_FLAGS) $(LTO_FLAGS) $(WARN_FLAGS) -DUSE_PICOPERF=0 \
	      $(INC_FLAGS) -o $(TARGET)_profile $(ALL_SRC)

# PGO: instrument -> train -> merge -> rebuild
ifeq ($(CC_IS_CLANG),1)
pgo:
	@mkdir -p $(PGO_DIR)
	$(CC) $(CFLAGS) $(PGO_GEN) $(INC_FLAGS) -o $(TARGET)_gen $(ALL_SRC)
	@echo "Running training workload (encrypt)..."
	./$(TARGET)_gen encrypt /home/strange/tc_test/odd.data /home/strange/tc_test/test.key >/dev/null 2>&1 || true
	@echo "Running training workload (decrypt)..."
	./$(TARGET)_gen decrypt /home/strange/tc_test/odd.data.locked /home/strange/tc_test/test.key >/dev/null 2>&1 || true
	@rm -f /home/strange/tc_test/odd.data.locked /home/strange/tc_test/odd.data.locked.unlocked
	llvm-profdata merge -output=$(PGO_DIR)/profile.clangprofdata \
	                  $(PGO_DIR)/profile.clangraw
	$(CC) $(CFLAGS) $(PGO_USE) $(INC_FLAGS) -o $(TARGET) $(ALL_SRC)
	$(RM) $(TARGET)_gen
else
pgo:
	$(CC) $(CFLAGS) $(PGO_GEN) $(INC_FLAGS) -o $(TARGET)_gen $(ALL_SRC)
	@echo "Running training workload (encrypt)..."
	./$(TARGET)_gen encrypt /home/strange/tc_test/odd.data /home/strange/tc_test/test.key >/dev/null 2>&1 || true
	@echo "Running training workload (decrypt)..."
	./$(TARGET)_gen decrypt /home/strange/tc_test/odd.data.locked /home/strange/tc_test/test.key >/dev/null 2>&1 || true
	@rm -f /home/strange/tc_test/odd.data.locked /home/strange/tc_test/odd.data.locked.unlocked
	$(CC) $(CFLAGS) $(PGO_USE) $(INC_FLAGS) -o $(TARGET) $(ALL_SRC)
	$(RM) $(TARGET)_gen
endif

# Remove build artifacts
clean:
	$(RM) $(TARGET) $(TARGET)_gen $(TARGET)_profile
	$(RM) *.gcda *.gcno
	$(RM) -r pgo_profiles
	$(RM) test.locked unlocked_test.data
	$(RM) slow_profile

# Show detected platform
info:
	@echo "PLATFORM    : $(PLATFORM)"
	@echo "ARCH        : $(ARCH)"
	@echo "CC          : $(CC)"
	@echo "CC_IS_CLANG : $(CC_IS_CLANG)"
	@echo "USE_PICOPERF: $(USE_PICOPERF)"
	@echo "CFLAGS      : $(CFLAGS)"
	@echo "ALL_SRC     : $(ALL_SRC)"