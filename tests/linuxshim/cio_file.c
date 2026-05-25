/*
 * cio_file.c — Linux shim for cio_file.h
 *
 * Maps the CIO wrapper API to standard C stdio for desktop testing.
 * NOTE/POINT are emulated using ftell/fseek.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "cio_file.h"

static FILE *cio_fp = NULL;
static uint32_t cio_pos = 0;

uint8_t cio_open(const char *filename)
{
    if (cio_fp) cio_close();
    cio_fp = fopen(filename, "rb");
    if (!cio_fp) return 1;
    cio_pos = 0;
    return 0;
}

void cio_close(void)
{
    if (!cio_fp) return;
    fclose(cio_fp);
    cio_fp = NULL;
}

uint16_t cio_read(void *buf, uint16_t len)
{
    uint16_t got;
    if (!cio_fp || len == 0) return 0;
    got = (uint16_t)fread(buf, 1, len, cio_fp);
    cio_pos += got;
    return got;
}

uint32_t cio_tell(void)
{
    return cio_pos;
}

uint8_t cio_note(CioBookmark *bm)
{
    if (!cio_fp) return 1;
    /* On Linux we just store the byte position; the aux fields
     * aren't meaningful but we fill them for consistency. */
    long p = ftell(cio_fp);
    if (p < 0) return 1;
    bm->byte_pos = (uint32_t)p;
    bm->aux3 = 0;
    bm->aux4 = 0;
    bm->aux5 = 0;
    return 0;
}

uint8_t cio_point(const CioBookmark *bm)
{
    if (!cio_fp) return 1;
    if (fseek(cio_fp, (long)bm->byte_pos, SEEK_SET) != 0) return 1;
    cio_pos = bm->byte_pos;
    return 0;
}

uint8_t cio_seek(const char *filename, uint32_t pos, uint8_t whence,
                 const CioBookmark *bm)
{
    if (!cio_fp) return 1;

    if (whence == 1) {
        /* SEEK_CUR */
        if (fseek(cio_fp, (long)pos, SEEK_CUR) != 0) return 1;
        cio_pos += pos;
    } else {
        /* SEEK_SET */
        if (fseek(cio_fp, (long)pos, SEEK_SET) != 0) return 1;
        cio_pos = pos;
    }

    (void)filename;
    (void)bm;
    return 0;
}
