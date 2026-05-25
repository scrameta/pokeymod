/*
 * cio_file.h — Raw CIO file I/O using a fixed IOCB.
 *
 * Drop-in replacements for fopen/fread/fseek/ftell/fclose that use
 * IOCB #6 exclusively, avoiding conflicts with the OS XEX loader.
 *
 * Only one file can be open at a time (single IOCB).
 */

#ifndef CIO_FILE_H
#define CIO_FILE_H

#include <stdint.h>

/*
 * Bookmark — captured file position for NOTE/POINT.
 * Save with cio_note(), restore with cio_point().
 */
typedef struct {
    uint8_t  aux3;      /* NOTE sector high / position */
    uint8_t  aux4;      /* NOTE sector low / position */
    uint8_t  aux5;      /* NOTE byte offset */
    uint32_t byte_pos;  /* tracked byte position at time of NOTE */
} CioBookmark;

/* Last CIO status byte, for debugging. Bit 7 set = error. */
extern uint8_t cio_last_status;

/* Open a file for binary reading. Returns 0 on success. */
uint8_t cio_open(const char *filename);

/* Close the file. Safe to call if not open. */
void cio_close(void);

/* Read up to len bytes into buf. Returns actual bytes read. */
uint16_t cio_read(void *buf, uint16_t len);

/* Return current byte position (tracked in software). */
uint32_t cio_tell(void);

/* Capture current file position via NOTE. Returns 0 on success. */
uint8_t cio_note(CioBookmark *bm);

/* Restore a previously captured position via POINT. Returns 0 on success. */
uint8_t cio_point(const CioBookmark *bm);

/*
 * Seek to position.
 *   whence: 0 = absolute (SEEK_SET), 1 = relative (SEEK_CUR).
 *   filename: needed for backward seeks without a bookmark (close/reopen).
 *   bm: optional bookmark for fast backward seek via POINT. Pass NULL
 *       to use the slow close/reopen/skip fallback.
 * Returns 0 on success.
 */
uint8_t cio_seek(const char *filename, uint32_t pos, uint8_t whence,
                 const CioBookmark *bm);

#endif /* CIO_FILE_H */
