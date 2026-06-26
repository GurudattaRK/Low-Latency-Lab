# Compiler and flags
CC := gcc
PICOPERF := /home/strange/Documents/GitHub/PicoPerf/picoperf.c
PICOPERF_INC := /home/strange/Documents/GitHub/PicoPerf
CFLAGS := -Ofast -march=native -flto
PGOFLAGS := -fprofile-generate
USEFLAGS := -fprofile-use
TARGET := main

# Default target: full PGO cycle (aggressive compiler + link + PGO optimizations)
.PHONY: all baseline pgo clean

all: pgo

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $(TARGET) main.c $(PICOPERF) -I$(PICOPERF_INC)

# Aggressive compiler + link optimizations without PGO
baseline: $(TARGET)

# Full PGO cycle: generate profile, train with enc+dec, then build final binary
pgo:
	$(CC) $(CFLAGS) $(PGOFLAGS) -o $(TARGET)_pgo_gen main.c $(PICOPERF) -I$(PICOPERF_INC)
	printf '1\n' | ./$(TARGET)_pgo_gen >/dev/null 2>&1
	printf '2\n' | ./$(TARGET)_pgo_gen >/dev/null 2>&1
	mv $(TARGET)_pgo_gen-main.gcda $(TARGET)-main.gcda
	mv $(TARGET)_pgo_gen-picoperf.gcda $(TARGET)-picoperf.gcda
	$(CC) $(CFLAGS) $(USEFLAGS) -o $(TARGET) main.c $(PICOPERF) -I$(PICOPERF_INC)
	rm -f $(TARGET)_pgo_gen $(TARGET)-main.gcda $(TARGET)-picoperf.gcda

clean:
	rm -f $(TARGET) $(TARGET)_pgo_gen $(TARGET)-main.gcda $(TARGET)-picoperf.gcda $(TARGET)_pgo_gen-main.gcda $(TARGET)_pgo_gen-picoperf.gcda main.gcda picoperf.gcda *.gcno test.locked unlocked_test.data
