/*
 * cio_file.c — Raw CIO file I/O using a fixed IOCB.
 *
 * CC65's fopen/fread use findfreeiocb which can grab the IOCB that
 * the OS XEX loader is using to read the program file.  This kills
 * the load stream when the file I/O is done via INITAD (mid-load).
 *
 * This module always uses IOCB #6 ($03A0), leaving the XEX loader's
 * IOCB untouched.
 *
 * Seek uses SpartaDOS NOTE/POINT for efficient backward seeks.
 * Call cio_note() to capture a bookmark, cio_point() to restore it.
 * Forward seeks read-and-discard.
 */

#include <stdint.h>
#include <atari.h>
#include <string.h>
#include "cio_file.h"

/* IOCB we exclusively use */
#define CIO_IOCB     6
#define CIO_IOCB_OFS (CIO_IOCB * 16)

/* IOCB register offsets from base ($0340 + N*16) */
#define ICHID  0   /* handler ID */
#define ICDNO  1   /* device number */
#define ICCOM  2   /* command */
#define ICSTA  3   /* status */
#define ICBAL  4   /* buffer address low */
#define ICBAH  5   /* buffer address high */
#define ICBLL  8   /* buffer length low */
#define ICBLH  9   /* buffer length high */
#define ICAX1 10   /* aux 1 (open mode) */
#define ICAX2 11   /* aux 2 */
#define ICAX3 12   /* aux 3 — NOTE/POINT */
#define ICAX4 13   /* aux 4 */
#define ICAX5 14   /* aux 5 */

/* CIO commands */
#define CIO_CMD_OPEN   0x03
#define CIO_CMD_GETBIN 0x07
#define CIO_CMD_CLOSE  0x0C
#define CIO_CMD_NOTE   0x26
#define CIO_CMD_POINT  0x25

#define IOCB (((uint8_t *)0x0340) + CIO_IOCB_OFS)

/* Implemented in cio_call.s — calls CIO with IOCB #6, returns status */
extern uint8_t cio_call6(void);

static uint8_t cio_is_open = 0;
static uint32_t cio_pos = 0;

/* Last CIO status, accessible for debugging */
uint8_t cio_last_status = 0;

uint8_t cio_open(const char *filename)
{
    uint8_t status;
    if (cio_is_open) cio_close();

    /* Ensure IOCB is seen as free — ICHID must be $FF */
    IOCB[ICHID] = 0xFF;

    IOCB[ICCOM] = CIO_CMD_OPEN;
    IOCB[ICBAL] = (uint8_t)((uint16_t)filename & 0xFF);
    IOCB[ICBAH] = (uint8_t)((uint16_t)filename >> 8);
    IOCB[ICAX1] = 4;  /* open for reading */
    IOCB[ICAX2] = 0;
    IOCB[ICBLL] = 0;  /* zero length for open */
    IOCB[ICBLH] = 0;

    status = cio_call6();
    cio_last_status = status;
    if (status & 0x80) return 1;

    cio_is_open = 1;
    cio_pos = 0;
    return 0;
}

void cio_close(void)
{
    uint8_t status;
    if (!cio_is_open) return;
    IOCB[ICCOM] = CIO_CMD_CLOSE;
    status = cio_call6();
    cio_last_status = status;
    cio_is_open = 0;
}

uint16_t cio_read(void *buf, uint16_t len)
{
    uint8_t status;
    uint16_t remaining;
    if (!cio_is_open || len == 0) return 0;

    IOCB[ICCOM] = CIO_CMD_GETBIN;
    IOCB[ICBAL] = (uint8_t)((uint16_t)buf & 0xFF);
    IOCB[ICBAH] = (uint8_t)((uint16_t)buf >> 8);
    IOCB[ICBLL] = (uint8_t)(len & 0xFF);
    IOCB[ICBLH] = (uint8_t)(len >> 8);

    status = cio_call6();
    cio_last_status = status;

    if (!(status & 0x80)) {
        /* Success — all bytes transferred */
        cio_pos += len;
        return len;
    }

    /* Error or EOF ($88) — check how many bytes actually transferred.
     * CIO decrements ICBLL/ICBLH during transfer; remaining = untransferred. */
    remaining = (uint16_t)(IOCB[ICBLL] | ((uint16_t)IOCB[ICBLH] << 8));
    {
        uint16_t got = len - remaining;
        cio_pos += got;
        return got;
    }
}

uint32_t cio_tell(void)
{
    return cio_pos;
}

/*
 * cio_note — capture current file position as a bookmark.
 * Uses SpartaDOS NOTE command.  Returns 0 on success.
 */
uint8_t cio_note(CioBookmark *bm)
{
    uint8_t status;
    if (!cio_is_open) return 1;

    IOCB[ICCOM] = CIO_CMD_NOTE;

    status = cio_call6();
    cio_last_status = status;
    if (status & 0x80) return 1;

    bm->aux3 = IOCB[ICAX3];
    bm->aux4 = IOCB[ICAX4];
    bm->aux5 = IOCB[ICAX5];
    bm->byte_pos = cio_pos;
    return 0;
}

/*
 * cio_point — restore a previously captured bookmark.
 * Uses SpartaDOS POINT command.  Returns 0 on success.
 */
uint8_t cio_point(const CioBookmark *bm)
{
    uint8_t status;
    if (!cio_is_open) return 1;

    IOCB[ICAX3] = bm->aux3;
    IOCB[ICAX4] = bm->aux4;
    IOCB[ICAX5] = bm->aux5;
    IOCB[ICCOM] = CIO_CMD_POINT;

    status = cio_call6();
    cio_last_status = status;
    if (status & 0x80) return 1;

    cio_pos = bm->byte_pos;
    return 0;
}

/*
 * cio_seek — seek to position.
 *
 * whence: 0 = SEEK_SET, 1 = SEEK_CUR.
 *
 * Forward seeks read-and-discard (always works).
 * Backward SEEK_SET uses the bookmark (via POINT) if provided,
 * otherwise falls back to close/reopen/skip.
 */
uint8_t cio_seek(const char *filename, uint32_t pos, uint8_t whence,
                 const CioBookmark *bm)
{
    uint32_t target;
    uint8_t skip_buf[128];

    if (whence == 1) {
        target = cio_pos + pos;
    } else {
        target = pos;
    }

    if (target == cio_pos) return 0;

    if (target < cio_pos) {
        /* Backward seek */
        if (bm != NULL && bm->byte_pos <= target) {
            /* Use bookmark — fast POINT then skip forward */
            if (cio_point(bm) != 0) return 1;
        } else {
            /* Fallback: close, reopen, skip forward */
            cio_close();
            if (cio_open(filename) != 0) return 1;
        }
    }

    /* Skip forward to target */
    while (cio_pos < target) {
        uint32_t remain = target - cio_pos;
        uint16_t chunk = (remain > 128u) ? 128u : (uint16_t)remain;
        if (cio_read(skip_buf, chunk) != chunk) return 1;
    }

    return 0;
}
