/* PS1 SPU: 24 ADPCM voices, ADSR, sweep, noise, pitch modulation, reverb, CD audio input.
 * Produces 44100 Hz stereo output; one output sample per 768 CPU cycles. */
#pragma once
#include <stdint.h>
#include <stddef.h>

void     spu_init(void);
uint16_t spu_read16(uint32_t off);              /* off = address - 0x1F801C00 (0..0x3FF) */
void     spu_write16(uint32_t off, uint16_t v);

/* DMA channel 4 (transfers use the current transfer address register 1F801DA6h) */
void     spu_dma_write(const uint32_t *src, int words);
void     spu_dma_read(uint32_t *dst, int words);

/* Advance the SPU by `samples` output samples, appending stereo int16 pairs to `out`
 * (out may be NULL to discard). */
void     spu_run(int samples, int16_t *out);

/* Provided by the runtime: fetch one stereo CD audio sample (44100 Hz, already volume-mixed
 * by the CD-ROM controller). Return 0 when no audio is available (outputs silence). */
extern int cdrom_audio_sample(int16_t *l, int16_t *r);
/* Provided by the runtime: called when the SPU raises its interrupt (IRQ9). */
extern void spu_irq_callback(void);

size_t   spu_state_size(void);
void     spu_save_state(void *dst);
void     spu_load_state(const void *src);
