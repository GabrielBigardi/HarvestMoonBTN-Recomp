/* GTE (COP2 Geometry Transformation Engine) - exact fixed-point reimplementation. */
#pragma once
#include <stdint.h>
#include <stddef.h>

void     gte_reset(void);
uint32_t gte_read_data(int reg);              /* MFC2 / SWC2  (reg 0..31) */
void     gte_write_data(int reg, uint32_t v); /* MTC2 / LWC2  (reg 0..31) */
uint32_t gte_read_ctrl(int reg);              /* CFC2         (reg 0..31) */
void     gte_write_ctrl(int reg, uint32_t v); /* CTC2         (reg 0..31) */
void     gte_command(uint32_t op);            /* COP2 imm25 command word (instruction & 0x1FFFFFF) */

size_t   gte_state_size(void);
void     gte_save_state(void *dst);
void     gte_load_state(const void *src);
