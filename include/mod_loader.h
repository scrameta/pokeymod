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

/* Select the default speed-only tempo before mod_load().
 * PAL uses BPM 125 (50 ticks/sec); NTSC uses BPM 150 (60 ticks/sec).
 * Files that contain F20+ BPM commands still override this during playback. */
void mod_set_legacy_tempo_pal(uint8_t pal);

/* Select player tick source before mod_load(). 0=deferred VBI, non-zero=POKEY timer. */
void mod_set_timer_timing(uint8_t enabled);

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
