# TeraCrypt v3.0 - Thread Variant Makefile
# Aggressive optimizations with PGO support

TARGET := teracrypt_threads
SRCS := main.c platform.c crypto_core.c aio_wrapper.c
OBJS := $(SRCS:.c=.o)

# Compiler selection - Clang by default
CC ?= clang
CC_IS_CLANG := $(shell $(CC) --version 2>&1 | grep -c clang)

# Aggressive optimization flags
OPT_FLAGS := -Ofast
ARCH_FLAGS := -march=native
LTO_FLAGS := -flto
EXTRA_FLAGS := -funroll-loops -fomit-frame-pointer -fno-semantic-interposition
WARN_FLAGS := -Wall -Wextra
DEFINES := -D_GNU_SOURCE

CFLAGS := $(OPT_FLAGS) $(ARCH_FLAGS) $(LTO_FLAGS) $(EXTRA_FLAGS) $(WARN_FLAGS) $(DEFINES)
LDFLAGS := -lpthread -lrt $(LTO_FLAGS)

# PGO flags
ifeq ($(CC_IS_CLANG),1)
    PGO_DIR := pgo_profiles
    PGO_GEN := -fprofile-instr-generate=$(PGO_DIR)/profile.clangraw
    PGO_USE := -fprofile-instr-use=$(PGO_DIR)/profile.clangprofdata
else
    PGO_GEN := -fprofile-generate
    PGO_USE := -fprofile-use -fprofile-correction -Wno-missing-profile
endif

.PHONY: all clean pgo info

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# PGO build
ifeq ($(CC_IS_CLANG),1)
pgo:
	@mkdir -p $(PGO_DIR)
	$(CC) $(CFLAGS) $(PGO_GEN) $(SRCS) -o $(TARGET)_gen $(LDFLAGS)
	@echo "Running training workload (encrypt)..."
	./$(TARGET)_gen encrypt ../../../big.data /tmp/pgo_train.enc ../../../test.key >/dev/null 2>&1 || true
	@echo "Running training workload (decrypt)..."
	./$(TARGET)_gen decrypt /tmp/pgo_train.enc /tmp/pgo_train.dec ../../../test.key >/dev/null 2>&1 || true
	@rm -f /tmp/pgo_train.enc /tmp/pgo_train.dec
	llvm-profdata merge -output=$(PGO_DIR)/profile.clangprofdata $(PGO_DIR)/profile.clangraw
	$(CC) $(CFLAGS) $(PGO_USE) $(SRCS) -o $(TARGET) $(LDFLAGS)
	@rm -f $(TARGET)_gen
	@echo "PGO build complete!"
else
pgo:
	$(CC) $(CFLAGS) $(PGO_GEN) $(SRCS) -o $(TARGET)_gen $(LDFLAGS)
	@echo "Running training workload (encrypt)..."
	./$(TARGET)_gen encrypt ../../../big.data /tmp/pgo_train.enc ../../../test.key >/dev/null 2>&1 || true
	@echo "Running training workload (decrypt)..."
	./$(TARGET)_gen decrypt /tmp/pgo_train.enc /tmp/pgo_train.dec ../../../test.key >/dev/null 2>&1 || true
	@rm -f /tmp/pgo_train.enc /tmp/pgo_train.dec
	$(CC) $(CFLAGS) $(PGO_USE) $(SRCS) -o $(TARGET) $(LDFLAGS)
	@rm -f $(TARGET)_gen
	@echo "PGO build complete!"
endif

clean:
	rm -f $(OBJS) $(TARGET) $(TARGET)_gen
	rm -f *.gcda *.gcno
	rm -rf $(PGO_DIR)

info:
	@echo "CC          : $(CC)"
	@echo "CC_IS_CLANG : $(CC_IS_CLANG)"
	@echo "CFLAGS      : $(CFLAGS)"
	@echo "LDFLAGS     : $(LDFLAGS)"
