/*
 * test2_header.c - Load MOD file and dump header info
 *
 * Tests: fopen, fread, fseek, header parsing
 * No VBI, no PokeyMAX writes, no sample upload.
 * Just reads the file and prints what it finds.
 *
 * Build:
 *   cl65 -t atari -O -o test2.xex test2_header.c pokeymax_hw.c -I include
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <conio.h>

/* Read big-endian uint16 */
static uint16_t be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

int main(void)
{
    FILE    *f;
    uint8_t  buf[30];
    uint8_t  title[21];
    uint8_t  order[128];
    uint8_t  two[2];
    uint8_t  magic[5];
    uint8_t  i;
    uint8_t  num_patterns;
    uint32_t total_sample_bytes;
    const char *filename = "D1:MOD.DAT";

    clrscr();
    printf("Test 2: MOD Header\n");
    printf("------------------\n");
    printf("Opening: %s\n", filename);

    f = fopen(filename, "rb");
    if (!f) { printf("FAILED to open\n"); goto done; }
    printf("OK\n");

    /* Title */
    if (fread(title, 1, 20, f) != 20) { printf("read title FAILED\n"); goto done; }
    title[20] = 0;
    printf("Title: [%s]\n", title);

    /* 31 sample headers */
    total_sample_bytes = 0;
    printf("Samples:\n");
    for (i = 1; i <= 31; i++) {
        uint16_t len, loop_start, loop_len;
        uint8_t  vol, ft;
        uint8_t  name[23];

        if (fread(buf, 1, 30, f) != 30) { printf("read sample %d FAILED\n", i); goto done; }
        memcpy(name, buf, 22); name[22] = 0;
        len        = be16(buf + 22) * 2;
        ft         = buf[24] & 0x0F;
        vol        = buf[25];
        loop_start = be16(buf + 26) * 2;
        loop_len   = be16(buf + 28) * 2;

        if (len > 0) {
            printf(" %2d: %5u bytes vol=%2d ft=%d loop=%u+%u [%s]\n",
                   i, len, vol, (ft > 7 ? (int)ft-16 : (int)ft),
                   loop_start, loop_len, name);
            total_sample_bytes += len;
        }
    }
    printf("Total sample bytes: %lu\n", total_sample_bytes);

    /* Song length + restart */
    if (fread(two, 1, 2, f) != 2) { printf("read songlength FAILED\n"); goto done; }
    printf("Song length: %d orders\n", two[0]);

    /* Order table */
    if (fread(order, 1, 128, f) != 128) { printf("read orders FAILED\n"); goto done; }
    num_patterns = 0;
    for (i = 0; i < two[0]; i++)
        if (order[i] > num_patterns) num_patterns = order[i];
    num_patterns++;
    printf("Patterns used: %d\n", num_patterns);
    printf("Order: ");
    for (i = 0; i < two[0] && i < 16; i++) printf("%d ", order[i]);
    if (two[0] > 16) printf("...");
    printf("\n");

    /* Magic */
    if (fread(magic, 1, 4, f) != 4) { printf("read magic FAILED\n"); goto done; }
    magic[4] = 0;
    printf("Magic: [%s]\n", magic);

    /* Pattern data size */
    {
        uint32_t pat_bytes = (uint32_t)num_patterns * 64UL * 4UL * 4UL;
        printf("Pattern data: %lu bytes\n", pat_bytes);
    }

    printf("\nAll OK - press any key\n");
    fclose(f);

done:
    while (!(*(unsigned char*)0x02FC != 255))
        ;
    *(unsigned char*)0x02FC = 255;
    return 0;
}
