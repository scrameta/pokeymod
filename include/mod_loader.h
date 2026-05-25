#ifndef MOD_LOADER_H
#define MOD_LOADER_H

#include "mod_format.h"

/* -------------------------------------------------------
 * API
 * ------------------------------------------------------- */

/*
 * mod_load()
 * Open a MOD file from disk, stream samples to PokeyMAX block RAM.
 * The file is kept open during playback for row-by-row pattern reads.
 * Returns 0 on success, non-zero on error.
 */
uint8_t mod_load(const char *filename);

/*
 * mod_file_close()
 * Close the MOD file (call after playback ends).
 */
void mod_file_close(void);

/*
 * Load progress/status plugin for mod_load().
 * Pass NULL to disable plugin output entirely.
 */

typedef struct {
    void (*begin)(void *ctx, uint32_t source_total);
    void (*update)(void *ctx,
                   const SampleInfo *si,
                   uint16_t sample_index,
                   uint16_t total_samples,
                   uint32_t source_loaded,
                   uint32_t source_total,
                   uint32_t stored_loaded,
                   uint8_t skipped);
    void (*end)(void *ctx);
    void *ctx;
} ModLoadProgressPlugin;

void mod_set_load_progress_plugin(const ModLoadProgressPlugin *plugin);

#endif // MOD_LOADER_H
