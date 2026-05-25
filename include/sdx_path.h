#ifndef SDX_PATH_H
#define SDX_PATH_H

#include <stdint.h>

/*
 * Resolve a filename relative to the current SpartaDOS drive/directory.
 * If filename already contains ':', it's used as-is.
 * Otherwise, prepends "Dn:path>" from SpartaDOS system variables.
 * Result written to buf (max bufsize bytes including NUL).
 */
void sdx_resolve_path(const char *filename, char *buf, uint8_t bufsize);

#endif
