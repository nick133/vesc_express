#ifndef MAIN_DISPLAY_DISP_ST7789_I80_H_
#define MAIN_DISPLAY_DISP_ST7789_I80_H_

#include <stdint.h>
#include <stdbool.h>
#include "lispif_disp_extensions.h"

void disp_st7789_i80_init(
    const int data_pins[8],
    int pin_wr,
    int pin_cs,
    int pin_reset,
    int pin_dc,
    int pin_rd,
    int width,
    int height,
    int clock_mhz
);

void disp_st7789_i80_command(uint8_t command, const uint8_t *args, int argn);
bool disp_st7789_i80_render_image(image_buffer_t *img, uint16_t x, uint16_t y, color_t *colors);
void disp_st7789_i80_clear(uint32_t color);
void disp_st7789_i80_reset(void);

#endif
