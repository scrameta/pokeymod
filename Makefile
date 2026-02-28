# PokeyMAX MOD Player - Unified Makefile
# Requires: cc65 toolchain (cl65, ca65)
# Targets:
#   make             - build main player (modplay.xex)
#   make test2       - MOD header dump
#   make test3       - sine tone test (no MOD file needed)
#   make test4       - first row trigger test
#   make all         - build everything
#   make clean       - remove all build artefacts

TARGET  = atari
CFLAGS  = -t $(TARGET) -O --include-dir include
ASFLAGS = -t $(TARGET)

# --- Shared sources (used by player and tests) ---
SHARED_C = src/pokeymax_hw.c src/adpcm.c src/tables.c

# --- Main player ---
PLAYER_C = src/main.c src/modplayer.c src/mod_loader.c \
           src/loop_handler.c $(SHARED_C)
PLAYER_S = src/vbi_handler.s
PLAYER_O = $(PLAYER_C:.c=.o) $(PLAYER_S:.s=.o)

# --- Tests (each is a single .c + shared) ---
TEST2_C  = tests/test2_header.c
TEST3_C  = tests/test3_tone.c    $(SHARED_C)
TEST3_ADPCM_C = tests/test3_adpcm_tone.c $(SHARED_C)
TEST4_C  = tests/test4_firstrow.c $(SHARED_C)

.PHONY: all player test2 test3 test3-adpcm test4 test-adpcm-linux clean

all: modplay.xex test2.xex test3.xex test3_adpcm.xex test4.xex

player: modplay.xex

modplay.xex: $(PLAYER_C) $(PLAYER_S)
	cl65 $(CFLAGS) -t $(TARGET) -m modplay.map \
	     -o modplay.xex $(PLAYER_C) $(PLAYER_S)
	@echo "Built: modplay.xex ($$(wc -c < modplay.xex) bytes)"

test2.xex: $(TEST2_C)
	cl65 $(CFLAGS) -o test2.xex $(TEST2_C)
	@echo "Built: test2.xex"

test3.xex: $(TEST3_C)
	cl65 $(CFLAGS) -o test3.xex $(TEST3_C)
	@echo "Built: test3.xex"

test3_adpcm.xex: $(TEST3_ADPCM_C)
	cl65 $(CFLAGS) -o test3_adpcm.xex $(TEST3_ADPCM_C)
	@echo "Built: test3_adpcm.xex"

test4.xex: $(TEST4_C)
	cl65 $(CFLAGS) -o test4.xex $(TEST4_C)
	@echo "Built: test4.xex"

test-adpcm-linux: tests/adpcm_roundtrip_linux.c src/adpcm.c include/adpcm.h
	gcc -std=c99 -O2 -Wall -Wextra -Iinclude -o tests/adpcm_roundtrip_linux tests/adpcm_roundtrip_linux.c src/adpcm.c -lm
	./tests/adpcm_roundtrip_linux

clean:
	rm -f modplay.xex modplay.map test2.xex test3.xex test3_adpcm.xex test4.xex
	rm -f src/*.o tests/*.o 

