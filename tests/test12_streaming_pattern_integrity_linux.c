/*
 * test12_streaming_pattern_integrity_linux.c
 *
 * Host-side regression test for streamed pattern loading.
 *
 * Validates that if prefetch target changes mid-stream, the loaded pattern
 * still matches the source MOD bytes exactly (no mixed/corrupt pattern data).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modplayer.h"
#include "mod_format.h"
#include "pokeymax_hw.h"

uint8_t pokeymax_irq_pending = 0;

static uint8_t regs[0x10000];
static uint8_t sample_ram[POKEYMAX_RAM_SIZE];
static uint16_t ram_addr_ptr = 0;

uint8_t pokeymax_mock_peek(uint16_t addr)
{
    return regs[addr];
}

void pokeymax_mock_poke(uint16_t addr, uint8_t val)
{
    switch (addr) {
        case REG_RAMADDRL:
            ram_addr_ptr = (uint16_t)((ram_addr_ptr & 0xFF00u) | val);
            regs[addr] = val;
            return;
        case REG_RAMADDRH:
            ram_addr_ptr = (uint16_t)((ram_addr_ptr & 0x00FFu) | ((uint16_t)val << 8));
            regs[addr] = val;
            return;
        case REG_RAMDATAINC:
            if (ram_addr_ptr < POKEYMAX_RAM_SIZE) {
                sample_ram[ram_addr_ptr] = val;
            }
            ram_addr_ptr++;
            regs[addr] = val;
            return;
        default:
            regs[addr] = val;
            return;
    }
}

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t compute_pattern_data_offset(FILE *fp)
{
    uint8_t hdr[30];
    uint8_t two[2];
    uint8_t throwaway[128 + 4];
    uint8_t i;

    if (fseek(fp, 20L, SEEK_SET) != 0) return 0;

    for (i = 0; i < MOD_MAX_SAMPLES; i++) {
        if (fread(hdr, 1, 30, fp) != 30) return 0;
        (void)read_be16(hdr + 22);
    }

    if (fread(two, 1, 2, fp) != 2) return 0;
    if (fread(throwaway, 1, sizeof(throwaway), fp) != sizeof(throwaway)) return 0;

    return (uint32_t)ftell(fp);
}

static int read_pattern_from_file(FILE *fp, uint32_t pattern_base, uint8_t pattern, uint8_t *out)
{
    uint32_t offset = pattern_base + (uint32_t)pattern * 1024u;
    if (fseek(fp, (long)offset, SEEK_SET) != 0) return 1;
    if (fread(out, 1, 1024u, fp) != 1024u) return 1;
    return 0;
}

static int verify_buffered_pattern_matches_file(FILE *fp, uint32_t pattern_base, uint8_t pattern)
{
    uint8_t expected[1024];
    uint8_t row;

    if (read_pattern_from_file(fp, pattern_base, pattern, expected) != 0) {
        fprintf(stderr, "Failed to read pattern %u from source file\n", (unsigned)pattern);
        return 1;
    }

    for (row = 0; row < MOD_ROWS_PER_PAT; row++) {
        const uint8_t *row_ptr = NULL;
        uint16_t off = (uint16_t)row * (MOD_CHANNELS * 4u);

        if (mod_get_row_ptr(pattern, row, &row_ptr) != 0u || row_ptr == NULL) {
            fprintf(stderr, "Pattern %u row %u not available in cache\n",
                    (unsigned)pattern, (unsigned)row);
            return 1;
        }

        if (memcmp(row_ptr, expected + off, MOD_CHANNELS * 4u) != 0) {
            fprintf(stderr, "Pattern mismatch at pattern=%u row=%u\n",
                    (unsigned)pattern, (unsigned)row);
            return 1;
        }
    }

    return 0;
}

int main(int argc, char **argv)
{
    const char *mod_path = (argc > 1) ? argv[1] : "mods/agend.mod";
    FILE *fp = NULL;
    uint32_t pattern_base;
    uint8_t pat_a = 0xFF;
    uint8_t pat_b = 0xFF;
    uint8_t i;

    if (mod_load(mod_path) != 0u) {
        fprintf(stderr, "mod_load failed for %s\n", mod_path);
        return 1;
    }

    if (((uint32_t)mod.num_patterns * 1024u) <= (16u * 1024u)) {
        fprintf(stderr, "Test requires streaming patterns; got only %u patterns\n",
                (unsigned)mod.num_patterns);
        mod_file_close();
        return 2;
    }

    for (i = 0; i < mod.song_length; i++) {
        uint8_t p = mod.order_table[i];
        if (pat_a == 0xFF) {
            pat_a = p;
        } else if (p != pat_a) {
            pat_b = p;
            break;
        }
    }

    if (pat_a == 0xFF || pat_b == 0xFF) {
        fprintf(stderr, "Could not find two distinct patterns in order table\n");
        mod_file_close();
        return 3;
    }

    fp = fopen(mod_path, "rb");
    if (!fp) {
        fprintf(stderr, "Failed to open %s for verification\n", mod_path);
        mod_file_close();
        return 4;
    }

    pattern_base = compute_pattern_data_offset(fp);
    if (pattern_base == 0u) {
        fprintf(stderr, "Failed to parse MOD header for %s\n", mod_path);
        fclose(fp);
        mod_file_close();
        return 5;
    }

    /*
     * Force a partial fetch of pat_a, then switch to pat_b before completion.
     * Regression target: stale prefetch state must not mix bytes from pat_a into pat_b.
     */
    mod_pattern_advance(mod.order_table[0], pat_a);
    if (mod_prefetch_next_pattern() != 0u) {
        fprintf(stderr, "prefetch step failed for first partial fetch\n");
        fclose(fp);
        mod_file_close();
        return 6;
    }

    mod_pattern_advance(mod.order_table[0], pat_b);
    while (mod_need_prefetch) {
        if (mod_prefetch_next_pattern() != 0u) {
            fprintf(stderr, "prefetch failed while loading final target pattern\n");
            fclose(fp);
            mod_file_close();
            return 7;
        }
    }

    if (verify_buffered_pattern_matches_file(fp, pattern_base, pat_b) != 0) {
        fclose(fp);
        mod_file_close();
        return 8;
    }

    printf("Streaming pattern integrity PASS (%s, pat %u -> pat %u)\n",
           mod_path, (unsigned)pat_a, (unsigned)pat_b);

    fclose(fp);
    mod_file_close();
    return 0;
}
