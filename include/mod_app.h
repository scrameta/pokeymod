#ifndef MOD_APP_H
#define MOD_APP_H

#include <stdint.h>

/*
 * app_loader_run()
 * Performs hardware detect + MOD load and prints load-time summary.
 *
 * show_progress_ui:
 *   1 = use default load-time progress/status plugin
 *   0 = disable load-time progress/status output
 *
 * Returns 0 on success, non-zero on error.
 */
uint8_t app_loader_run(const char *filename, uint8_t show_progress_ui);

/*
 * app_player_vbi_tick()
 * Call from your deferred VBI handler (or equivalent periodic music tick hook).
 * This advances player timing/effects and can trigger row decode.
 */
void app_player_vbi_tick(void);

/*
 * app_player_irq_handler()
 * Fast IRQ service helper for PokeyMAX sample-end IRQs.
 *
 * Returns 1 if a PokeyMAX sample-end IRQ was pending and serviced,
 * 0 if not ours (caller should chain to next IRQ handler).
 */
uint8_t app_player_irq_handler(void);

/*
 * app_player_main_service()
 * Foreground/main-loop helper. Performs at most one 256-byte pattern
 * prefetch chunk when needed.
 *
 * Call cadence:
 *   call once per frame (N=1) for robust streaming.
 *
 * Cost (rough guide):
 *   0 ms if no prefetch due; otherwise one disk read chunk.
 *   On a 1050-class drive, one 256-byte chunk is typically ~25 ms worst-case.
 *
 * Returns 0 on success/no-op, non-zero on I/O error.
 */
uint8_t app_player_main_service(void);

/* Start/stop helpers for integrators managing their own VBI/IRQ/main loop. */
void app_player_start(void);
void app_player_stop(uint8_t close_file);

/*
 * app_player_run()
 * Starts playback loop (installs VBI hook, handles keys, prefetches patterns).
 */
void app_player_run(void);

#endif
