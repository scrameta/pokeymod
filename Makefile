# PokeyMAX MOD Player - Unified Makefile
# Requires: cc65 toolchain (cl65, ca65)
# Targets:
#   make             - build main player (modplay.xex)
#   make test2       - MOD header dump
#   make test3       - sine tone test (no MOD file needed)
#   make test4       - first row trigger test
#   make test5       - deferred VBI hook counter test
#   make test6       - PokeyMAX sample IRQ counter test
#   make all         - build everything
#   make clean       - remove all build artefacts

TARGET  = atari
CFLAGS  = -t $(TARGET) -Osir -Cl --include-dir include
#CFLAGS  = -t $(TARGET) -C cfg/atari.cfg -Osir -Cl --include-dir include
CFLAGSLOAD  = -t $(TARGET) -C cfg/load.cfg -Osir -Cl --include-dir include
CFLAGSPLAY  = -t $(TARGET) -C cfg/play.cfg -Osir -Cl --include-dir include
ASFLAGS = -t $(TARGET)

# --- Shared sources (used by player and tests) ---
SHARED_C = src/pokeymax_hw.c src/adpcm.c

# --- Main loader and player ---
LOADER_C = src/loader_main.c src/mod_loader.c src/app_loader.c src/mod.c src/mod_default_progress_plugin.c \
           $(SHARED_C)
LOADER_S = src/memcpy_banked.s

PLAYER_C = src/player_main.c src/app_player.c src/app_player_core.c src/modplayer.c src/chan_base.c \
           src/pokeymax_hw.c src/tables.c src/mod_pattern_bank.c src/mod.c
PLAYER_S = src/vbi_handler.s src/loop_handler_irq.s src/memcpy_banked.s

# --- Tests (each is a single .c + shared) ---
TEST2_C  = tests/test2_header.c
TEST3_C  = tests/test3_tone.c    $(SHARED_C)
TEST3_ADPCM_C = tests/test3_adpcm_tone.c $(SHARED_C)
TEST4_C  = tests/test4_firstrow.c $(SHARED_C)
TEST5_C  = tests/test5_vbi.c
TEST6_C  = tests/test6_irq.c src/pokeymax_hw.c
TEST7_C  = tests/test7_irq_during_vbi.c src/pokeymax_hw.c
TEST8_C  = tests/test8_irq_during_foreground.c src/pokeymax_hw.c
TEST9_C  = tests/test9_startup_path.c src/pokeymax_hw.c src/mod_loader.c src/modplayer.c src/tables.c src/adpcm.c src/loop_handler.c src/chan_base.c src/bank.c
TEST9B_C = tests/test9b_startup_noirq.c src/pokeymax_hw.c src/mod_loader.c src/modplayer.c src/tables.c src/adpcm.c src/loop_handler.c src/chan_base.c src/bank.c
TEST10_C = tests/test10_main_startup_markers.c src/pokeymax_hw.c src/mod_loader.c src/modplayer.c src/tables.c src/adpcm.c src/loop_handler.c src/chan_base.c src/bank.c
TEST11_C = tests/test11_row0_decode.c src/pokeymax_hw.c src/mod_loader.c src/modplayer.c src/tables.c src/adpcm.c src/loop_handler.c src/chan_base.c src/bank.c
TEST56_S = src/vbi_handler.s
TEST56_COMPAT_S = src/loop_handler_irq_compat.s

all: modload.xex modply.xex test2.xex test3.xex test3ad.xex test4.xex test5.xex test6.xex test7.xex test8.xex test9.xex test9b.xex test10.xex test11.xex
.PHONY: all player test2 test3 test3-adpcm test4 test5 test6 test7 test8 test9 test9b test10 test11 test-adpcm-linux clean

#player: modplay.xex

modload.xex: $(LOADER_C) $(LOADER_S)
	cl65 $(CFLAGSLOAD) -t $(TARGET) -m modload.map \
	     -o modload.xex $(LOADER_C) $(LOADER_S)
	@echo "Built: modload.xex ($$(wc -c < modload.xex) bytes)"

modply.xex: $(PLAYER_C) $(PLAYER_S)
	cl65 $(CFLAGSPLAY) -t $(TARGET) -m modply.map \
	     -o modply.xex $(PLAYER_C) $(PLAYER_S)
	@echo "Built: modply.xex ($$(wc -c < modply.xex) bytes)"

modplay.xex: modply.xex modload.xex
	#./xex-filter.pl -o modplay.xex -i 1,2,3,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,5,6,7,8 -w738=10241,130=0,0,0,0,0,0,0,0,0,0,0,0,0,0 modload.xex modply.xex
	#./xex-filter.pl -o modplay.xex -i 1,2,3,9,7,8 -w738=10241,130=0,0,0,0,0,0,0,0,0,0,0,0,0,0 modload.xex modply.xex
	./xex-filter.pl -o modplay.xex -i 1,2,3,9,5,6,7,8 -w738=10241 modload.xex modply.xex


hello.xex: src/hello.c
	cl65 $(CFLAGS) -o hello.xex src/hello.c
	@echo "Built: hello.xex"

world.xex: src/world.c
	cl65 $(CFLAGS) -o world.xex src/world.c
	@echo "Built: world.xex"

hellow.xex: hello.xex world.xex
	./xex-filter.pl -o hellow.xex -i 1,2,3,9,5,6,7,8 -w738=8193  hello.xex world.xex

test2.xex: $(TEST2_C)
	cl65 $(CFLAGS) -o test2.xex $(TEST2_C)
	@echo "Built: test2.xex"

test3.xex: $(TEST3_C)
	cl65 $(CFLAGS) -o test3.xex $(TEST3_C)
	@echo "Built: test3.xex"

test3ad.xex: $(TEST3_ADPCM_C)
	cl65 $(CFLAGS) -o test3ad.xex $(TEST3_ADPCM_C)
	@echo "Built: test3ad.xex"

test4.xex: $(TEST4_C)
	cl65 $(CFLAGS) -o test4.xex $(TEST4_C)
	@echo "Built: test4.xex"

test5.xex: $(TEST5_C) $(TEST56_S) $(TEST56_COMPAT_S)
	cl65 $(CFLAGS) -o test5.xex $(TEST5_C) $(TEST56_S) $(TEST56_COMPAT_S)
	@echo "Built: test5.xex"

test6.xex: $(TEST6_C) $(TEST56_S) $(TEST56_COMPAT_S)
	cl65 $(CFLAGS) -o test6.xex $(TEST6_C) $(TEST56_S) $(TEST56_COMPAT_S)
	@echo "Built: test6.xex"
test7.xex: $(TEST7_C) $(TEST56_S) $(TEST56_COMPAT_S)
	cl65 $(CFLAGS) -o test7.xex $(TEST7_C) $(TEST56_S) $(TEST56_COMPAT_S)
	@echo "Built: test7.xex"
test8.xex: $(TEST8_C) $(TEST56_S) $(TEST56_COMPAT_S)
	cl65 $(CFLAGS) -o test8.xex $(TEST8_C) $(TEST56_S) $(TEST56_COMPAT_S)
	@echo "Built: test8.xex"
test9.xex: $(TEST9_C) $(TEST56_S) src/loop_handler_irq.s
	cl65 $(CFLAGS) -o test9.xex $(TEST9_C) $(TEST56_S) src/loop_handler_irq.s
	@echo "Built: test9.xex"
test9b.xex: $(TEST9B_C) $(TEST56_S) src/loop_handler_irq.s
	cl65 $(CFLAGS) -o test9b.xex $(TEST9B_C) $(TEST56_S) src/loop_handler_irq.s
	@echo "Built: test9b.xex"
test10.xex: $(TEST10_C) $(TEST56_S) src/loop_handler_irq.s
	cl65 $(CFLAGS) -o test10.xex $(TEST10_C) $(TEST56_S) src/loop_handler_irq.s
	@echo "Built: test10.xex"
test11.xex: $(TEST11_C) $(TEST56_S) src/loop_handler_irq.s
	cl65 $(CFLAGS) -o test11.xex $(TEST11_C) $(TEST56_S) src/loop_handler_irq.s
	@echo "Built: test11.xex"
test-adpcm-linux: tests/adpcm_roundtrip_linux.c src/adpcm.c include/adpcm.h
	gcc -std=c99 -O2 -Wall -Wextra -Iinclude -o tests/adpcm_roundtrip_linux tests/adpcm_roundtrip_linux.c src/adpcm.c -lm
	./tests/adpcm_roundtrip_linux

clean:
	rm -f *.xex modplay.map 
	rm -f *.o src/*.o tests/*.o 
