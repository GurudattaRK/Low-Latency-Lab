# ─────────────────────────────────────────────────────────────────────────────
#  Cross-platform Makefile — GCC and Clang only
#  Targets: Linux (x86/x64, arm64, RISC-V), macOS (x86, Apple Silicon),Windows via MSYS2/MinGW with clang or gcc
# ─────────────────────────────────────────────────────────────────────────────

SRC    := main.c
TARGET := main

# ── Detect OS ─────────────────────────────────────────────────────────────────
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

# ── Detect arch ───────────────────────────────────────────────────────────────
ARCH := $(shell uname -m)
ifeq ($(ARCH),arm64)
    ARCH := aarch64
endif

# ── Compiler selection ────────────────────────────────────────────────────────
# Default: clang on macOS (Xcode), gcc everywhere else.
# Override: make CC=clang
ifeq ($(PLATFORM),macos)
    CC ?= clang
else
    CC ?= gcc
endif

CC_IS_CLANG := $(shell $(CC) --version 2>&1 | grep -c clang)

# ── picoperf: x86/x64 Linux only ─────────────────────────────────────────────
USE_PICOPERF := 0
ifeq ($(PLATFORM),linux)
    ifneq ($(filter $(ARCH),x86_64 i686 i386),)
        USE_PICOPERF := 1
    endif
endif

PICOPERF_SRC := ../../PicoPerf/picoperf.c
PICOPERF_INC := ../../PicoPerf

# ── Compile flags ─────────────────────────────────────────────────────────────
OPT_FLAGS  := -O3
WARN_FLAGS := -Wall -Wextra
LTO_FLAGS  := -flto

# -march=native: supported on all GCC/Clang targets except some RISC-V toolchains
ifeq ($(ARCH),riscv64)
    ARCH_FLAGS :=
else
    ARCH_FLAGS := -march=native
endif

CFLAGS := $(OPT_FLAGS) $(ARCH_FLAGS) $(LTO_FLAGS) $(WARN_FLAGS)

# ── Source list ───────────────────────────────────────────────────────────────
ifeq ($(USE_PICOPERF),1)
    ALL_SRC   := $(SRC) $(PICOPERF_SRC)
    INC_FLAGS := -I$(PICOPERF_INC)
else
    ALL_SRC   := $(SRC)
    INC_FLAGS :=
endif

# ── PGO flags ─────────────────────────────────────────────────────────────────
ifeq ($(CC_IS_CLANG),1)
    PGO_DIR      := pgo_profiles
    PGO_GEN      := -fprofile-instr-generate=$(PGO_DIR)/profile.clangraw
    PGO_USE      := -fprofile-instr-use=$(PGO_DIR)/profile.clangprofdata
else
    PGO_GEN      := -fprofile-generate
    PGO_USE      := -fprofile-use -fprofile-correction
endif

# ── Targets ───────────────────────────────────────────────────────────────────
.PHONY: all pgo clean info

all: baseline

baseline:
	$(CC) $(CFLAGS) $(INC_FLAGS) -o $(TARGET) $(ALL_SRC)

# ── PGO ───────────────────────────────────────────────────────────────────────
ifeq ($(CC_IS_CLANG),1)

pgo:
	@mkdir -p $(PGO_DIR)
	$(CC) $(CFLAGS) $(PGO_GEN) $(INC_FLAGS) -o $(TARGET)_gen $(ALL_SRC)
	printf '1\n' | ./$(TARGET)_gen >/dev/null 2>&1 || true
	printf '2\n' | ./$(TARGET)_gen >/dev/null 2>&1 || true
	llvm-profdata merge -output=$(PGO_DIR)/profile.clangprofdata $(PGO_DIR)/profile.clangraw
	$(CC) $(CFLAGS) $(PGO_USE) $(INC_FLAGS) -o $(TARGET) $(ALL_SRC)
	$(RM) $(TARGET)_gen

else

pgo:
	$(CC) $(CFLAGS) $(PGO_GEN) $(INC_FLAGS) -o $(TARGET)_gen $(ALL_SRC)
	printf '1\n' | ./$(TARGET)_gen >/dev/null 2>&1 || true
	printf '2\n' | ./$(TARGET)_gen >/dev/null 2>&1 || true
	$(CC) $(CFLAGS) $(PGO_USE) $(INC_FLAGS) -o $(TARGET) $(ALL_SRC)
	$(RM) $(TARGET)_gen

endif

# ── Clean ─────────────────────────────────────────────────────────────────────
clean:
	$(RM) $(TARGET) $(TARGET)_gen
	$(RM) *.gcda *.gcno
	$(RM) -r pgo_profiles
	$(RM) test.locked unlocked_test.data

# ── Info ──────────────────────────────────────────────────────────────────────
info:
	@echo "PLATFORM    : $(PLATFORM)"
	@echo "ARCH        : $(ARCH)"
	@echo "CC          : $(CC)"
	@echo "CC_IS_CLANG : $(CC_IS_CLANG)"
	@echo "USE_PICOPERF: $(USE_PICOPERF)"
	@echo "CFLAGS      : $(CFLAGS)"
	@echo "ALL_SRC     : $(ALL_SRC)"