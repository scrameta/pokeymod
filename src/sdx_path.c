/*
 * sdx_path.c — Resolve a filename relative to the current drive/directory.
 *
 * If the filename already has a drive prefix (contains ':'), use as-is.
 * Otherwise, prepend the current drive and path from SpartaDOS:
 *   device ($0761): low nibble = unit number (1=D1:, 2=D2:, etc.)
 *   PATH ($07A0): current directory path, NUL-terminated, uses '>' separator
 *
 * Result is written to the supplied buffer.
 */

#include <stdint.h>
#include <string.h>
#include "sdx_path.h"

#define SDX_DEVICE  (*(volatile uint8_t *)0x0761)
#define SDX_PATH    ((const char *)0x07A0)

void sdx_resolve_path(const char *filename, char *buf, uint8_t bufsize)
{
    const char *p;
    uint8_t pos = 0;

    /* Check if filename already has a drive prefix */
    for (p = filename; *p; p++) {
        if (*p == ':') {
            /* Already has drive — copy as-is */
            strncpy(buf, filename, bufsize);
            buf[bufsize - 1] = 0;
            return;
        }
    }

    /* Build "Dn:" from current device */
    {
        uint8_t unit = SDX_DEVICE & 0x0F;
        if (unit == 0) unit = 1;  /* default to D1: */
        if (pos < bufsize - 1) buf[pos++] = 'D';
        if (pos < bufsize - 1) buf[pos++] = '0' + unit;
        if (pos < bufsize - 1) buf[pos++] = ':';
    }

    /* Append PATH if non-empty */
    {
        const char *path = SDX_PATH;
        while (*path && pos < bufsize - 1) {
            buf[pos++] = *path++;
        }
        /* Ensure path ends with '>' separator if it had content */
        if (pos > 3 && buf[pos - 1] != '>') {
            if (pos < bufsize - 1) buf[pos++] = '>';
        }
    }

    /* Append filename */
    while (*filename && pos < bufsize - 1) {
        buf[pos++] = *filename++;
    }

    buf[pos] = 0;
}
