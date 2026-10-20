/* PS1 GPU: GP0/GP1 command processor with an exact software rasterizer over 1MB VRAM.
 * All commands complete instantly (the GPU is always "ready"). */
#pragma once
#include <stdint.h>
#include <stddef.h>

void     gpu_init(void);
void     gpu_write_gp0(uint32_t v);       /* write to 1F801810h */
void     gpu_write_gp1(uint32_t v);       /* write to 1F801814h */
uint32_t gpu_read_data(void);             /* read  from 1F801810h (GPUREAD) */
uint32_t gpu_read_status(void);           /* read  from 1F801814h (GPUSTAT) */

/* DMA channel 2 helpers (called by the DMA controller) */
void     gpu_dma_write(const uint32_t *src, int words);   /* block mode CPU->GPU (GP0) */
void     gpu_dma_read(uint32_t *dst, int words);          /* block mode GPU->CPU (GPUREAD) */

/* Timing hooks from the scheduler */
void     gpu_vblank(void);                /* called at start of vblank (toggles interlace field) */
void     gpu_set_line_parity(int odd);    /* GPUSTAT.31 for non-interlaced modes */

/* Display output */
typedef struct {
    int enabled;          /* display enabled (GP1(03h)) */
    int vram_x, vram_y;   /* display area start in VRAM (GP1(05h)) */
    int width, height;    /* visible resolution in pixels (from horizontal/vertical range + mode) */
    int is_24bit;
    int interlaced;
    int pal;
    int hres_mode;        /* raw GPUSTAT bits 16-18 */
} GpuDisplay;
void     gpu_get_display(GpuDisplay *d);
/* Convert the visible display area to 0xAARRGGBB pixels; out must hold 1024*512 entries.
 * Returns width/height actually written (row stride = *w). */
void     gpu_render_display(uint32_t *out, int *w, int *h);

const uint16_t *gpu_vram(void);           /* 1024x512 16-bit VRAM (debug / screenshots) */

size_t   gpu_state_size(void);
void     gpu_save_state(void *dst);
void     gpu_load_state(const void *src);
