#include <stdint.h>
#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_i80.h"
#include "esp_lcd_panel_io.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "lispif.h"
#include "lispbm.h"
#include "disp_st7789_i80.h"

static esp_lcd_i80_bus_handle_t i80_bus;
static esp_lcd_panel_io_handle_t panel_io;

#define DISPLAY_MAX_PIXELS (320U * 170U)

static int display_x_gap = 0;
static int display_y_gap = 35;
static uint8_t display_madctl = 0xA0;

static int display_width;
static int display_height;
static int m_pin_reset = -1;

static uint16_t *draw_buffer;
static SemaphoreHandle_t transfer_done;
static bool transfer_pending = false;

static lbm_value ext_disp_cmd(lbm_value *args, lbm_uint argn);
static lbm_value ext_disp_orientation(lbm_value *args, lbm_uint argn);


/* Wait until the previous DMA transfer has finished. */
static void wait_transfer(void) {
    if (transfer_pending) {
        xSemaphoreTake(transfer_done, portMAX_DELAY);
        transfer_pending = false;
    }
}


/* Called when an I80 color transfer finishes. */
static bool color_trans_done_cb(
    esp_lcd_panel_io_handle_t panel,
    esp_lcd_panel_io_event_data_t *edata,
    void *user_ctx
) {
    (void)panel;
    (void)edata;

    BaseType_t high_task_wakeup = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)user_ctx, &high_task_wakeup);

    return high_task_wakeup == pdTRUE;
}


void disp_st7789_i80_command(
    uint8_t command,
    const uint8_t *args,
    int argn
) {
    wait_transfer();

    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(
        panel_io,
        command,
        args,
        argn > 0 ? (size_t)argn : 0
    ));
}

static void set_address_window(
    uint16_t x_start,
    uint16_t y_start,
    uint16_t x_end,
    uint16_t y_end
) {
    x_start += display_x_gap;
    x_end += display_x_gap;
    y_start += display_y_gap;
    y_end += display_y_gap;

    uint8_t col[4] = {
        x_start >> 8, x_start,
        x_end >> 8, x_end
    };

    uint8_t row[4] = {
        y_start >> 8, y_start,
        y_end >> 8, y_end
    };

    disp_st7789_i80_command(0x2A, col, 4);
    disp_st7789_i80_command(0x2B, row, 4);
}

static uint16_t to_disp_color(uint32_t rgb) {
    uint8_t b = (uint8_t)rgb;
    uint8_t g = (uint8_t)(rgb >> 8);
    uint8_t r = (uint8_t)(rgb >> 16);

    r >>= 3;
    g >>= 2;
    b >>= 3;

    uint8_t color_high = (r << 3) | (g >> 3);
    uint8_t color_low = (g << 5) | b;

    /*
     * Store bytes in display transmission order:
     * RGB565 high byte first, then low byte.
     */
    return ((uint16_t)color_low << 8) | color_high;
}


static uint16_t convert_pixel(
    image_buffer_t *img,
    color_t *colors,
    uint32_t i
) {
    uint8_t *data = img->data;
    uint32_t rgb_value = 0;

    switch (img->fmt) {
    case indexed2: {
        uint32_t byte = i >> 3;
        uint32_t bit = 7 - (i & 0x07);
        uint32_t color_ind = (data[byte] >> bit) & 0x01;

        rgb_value = COLOR_TO_RGB888(
            colors[color_ind],
            i % img->width,
            i / img->width
        );
        break;
    }

    case indexed4: {
        uint32_t byte = i >> 2;
        uint32_t bit = (3 - (i & 0x03)) * 2;
        uint32_t color_ind = (data[byte] >> bit) & 0x03;

        rgb_value = COLOR_TO_RGB888(
            colors[color_ind],
            i % img->width,
            i / img->width
        );
        break;
    }

    case indexed16: {
        uint32_t byte = i >> 1;
        uint32_t bit = (1 - (i & 0x01)) * 4;
        uint32_t color_ind = (data[byte] >> bit) & 0x0F;

        rgb_value = COLOR_TO_RGB888(
            colors[color_ind],
            i % img->width,
            i / img->width
        );
        break;
    }

    case rgb332: {
        uint8_t pix = data[i];

        uint32_t r = (pix >> 5) & 0x07;
        uint32_t g = (pix >> 2) & 0x07;
        uint32_t b = pix & 0x03;

        rgb_value = (r << 21) | (g << 13) | (b << 6);
        break;
    }

    case rgb565: {
        uint16_t pix =
            ((uint16_t)data[2 * i] << 8) |
            ((uint16_t)data[2 * i + 1]);

        uint32_t r = (pix >> 11) & 0x1F;
        uint32_t g = (pix >> 5) & 0x3F;
        uint32_t b = pix & 0x1F;

        rgb_value = (r << 19) | (g << 10) | (b << 3);
        break;
    }

    case rgb888: {
        uint32_t r = data[3 * i];
        uint32_t g = data[3 * i + 1];
        uint32_t b = data[3 * i + 2];

        rgb_value = (r << 16) | (g << 8) | b;
        break;
    }

    default:
        return 0;
    }

    return to_disp_color(rgb_value);
}


bool disp_st7789_i80_render_image(
    image_buffer_t *img,
    uint16_t x,
    uint16_t y,
    color_t *colors
) {
    if (!img || !img->data || !img->width || !img->height) {
        return false;
    }

    uint32_t x_end = (uint32_t)x + img->width - 1;
    uint32_t y_end = (uint32_t)y + img->height - 1;

    if (x_end >= (uint32_t)display_width ||
        y_end >= (uint32_t)display_height) {
        return false;
    }

    if ((img->fmt == indexed2 ||
         img->fmt == indexed4 ||
         img->fmt == indexed16) && !colors) {
        return false;
    }

    uint32_t num_pix = (uint32_t)img->width * img->height;

    if (num_pix > DISPLAY_MAX_PIXELS) {
        return false;
    }

    wait_transfer();


    for (uint32_t i = 0; i < num_pix; i++) {
        draw_buffer[i] = convert_pixel(img, colors, i);
    }

    set_address_window(x, y, (uint16_t)x_end, (uint16_t)y_end);

    transfer_pending = true;

    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(
        panel_io,
        0x2C,
        draw_buffer,
        num_pix * sizeof(uint16_t)
    ));

    return true;
}


void disp_st7789_i80_clear(uint32_t color) {
    uint32_t num_pix = (uint32_t)display_width * display_height;
    uint16_t clear_color = to_disp_color(color);

    wait_transfer();

    for (uint32_t i = 0; i < num_pix; i++) {
        draw_buffer[i] = clear_color;
    }

    set_address_window(
        0,
        0,
        display_width - 1,
        display_height - 1
    );

    transfer_pending = true;

    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(
        panel_io,
        0x2C,
        draw_buffer,
        num_pix * sizeof(uint16_t)
    ));
}

void disp_st7789_i80_reset(void) {
    static const uint8_t b2[] = {
        0x0B, 0x0B, 0x00, 0x33, 0x33
    };

    static const uint8_t e0[] = {
        0xF0, 0x05, 0x0A, 0x06, 0x06, 0x03, 0x2B,
        0x32, 0x43, 0x36, 0x11, 0x10, 0x2B, 0x32
    };

    static const uint8_t e1[] = {
        0xF0, 0x08, 0x0C, 0x0B, 0x09, 0x24, 0x2B,
        0x22, 0x43, 0x38, 0x15, 0x16, 0x2F, 0x37
    };

    uint8_t arg;

    if (m_pin_reset >= 0) {
        gpio_set_level(m_pin_reset, 0);
        vTaskDelay(pdMS_TO_TICKS(5));
        gpio_set_level(m_pin_reset, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    /* Sleep out */
    disp_st7789_i80_command(0x11, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));

    /* Display inversion */
    disp_st7789_i80_command(0x21, NULL, 0);

    /* Landscape orientation */
    arg = display_madctl;
    disp_st7789_i80_command(0x36, &arg, 1);

    /* Pixel format: 16-bit RGB565 for the I80 interface */
    arg = 0x05;
    disp_st7789_i80_command(0x3A, &arg, 1);

    disp_st7789_i80_command(0xB2, b2, sizeof(b2));

    arg = 0x75;
    disp_st7789_i80_command(0xB7, &arg, 1);

    arg = 0x28;
    disp_st7789_i80_command(0xBB, &arg, 1);

    arg = 0x2C;
    disp_st7789_i80_command(0xC0, &arg, 1);

    arg = 0x01;
    disp_st7789_i80_command(0xC2, &arg, 1);

    arg = 0x1F;
    disp_st7789_i80_command(0xC3, &arg, 1);

    arg = 0x13;
    disp_st7789_i80_command(0xC6, &arg, 1);

    arg = 0xA7;
    disp_st7789_i80_command(0xD0, &arg, 1);

    {
        const uint8_t d0[] = {0xA4, 0xA1};
        disp_st7789_i80_command(0xD0, d0, sizeof(d0));
    }

    arg = 0xA1;
    disp_st7789_i80_command(0xD6, &arg, 1);

    disp_st7789_i80_command(0xE0, e0, sizeof(e0));
    disp_st7789_i80_command(0xE1, e1, sizeof(e1));

    /* Display on */
    disp_st7789_i80_command(0x29, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));

    disp_st7789_i80_clear(0);
}

static lbm_value ext_disp_cmd(lbm_value *args, lbm_uint argn) {
    LBM_CHECK_NUMBER_ALL();

    lbm_value res = ENC_SYM_TERROR;

    if (argn > 1 && argn <= 13) {
        uint8_t cmd = (uint8_t)lbm_dec_as_u32(args[0]);
        uint8_t paras[12];

        for (int i = 0; i < argn - 1; i++) {
            paras[i] = (uint8_t)lbm_dec_as_u32(args[i + 1]);
        }

        disp_st7789_i80_command(cmd, paras, argn - 1);
        res = ENC_SYM_TRUE;

    } else if (argn == 1) {
        uint8_t cmd = (uint8_t)lbm_dec_as_u32(args[0]);
        disp_st7789_i80_command(cmd, NULL, 0);
        res = ENC_SYM_TRUE;
    }

    return res;
}


static lbm_value ext_disp_orientation(lbm_value *args, lbm_uint argn) {
    LBM_CHECK_ARGN_NUMBER(1);

    uint32_t orientation = lbm_dec_as_u32(args[0]);
    uint8_t arg = 0;
    lbm_value res = ENC_SYM_TRUE;

    switch (orientation) {
    case 0:
        arg = 0x00;
        display_width = 170;
        display_height = 320;
        display_x_gap = 35;
        display_y_gap = 0;
        break;

    case 1:
        arg = 0x60;
        display_width = 320;
        display_height = 170;
        display_x_gap = 0;
        display_y_gap = 35;
        break;

    case 2:
        arg = 0xC0;
        display_width = 170;
        display_height = 320;
        display_x_gap = 35;
        display_y_gap = 0;
        break;

    case 3:
        arg = 0xA0;
        display_width = 320;
        display_height = 170;
        display_x_gap = 0;
        display_y_gap = 35;
        break;

    default:
        res = ENC_SYM_EERROR;
        return res;
    }

    display_madctl = arg;
    disp_st7789_i80_command(0x36, &arg, 1);

    return res;
}


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
) {
    display_width = width;
    display_height = height;

    display_x_gap = 0;
    display_y_gap = 35;
    display_madctl = 0xA0;

    m_pin_reset = pin_reset;

    transfer_done = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(transfer_done ? ESP_OK : ESP_ERR_NO_MEM);

    draw_buffer = heap_caps_malloc(
        DISPLAY_MAX_PIXELS * sizeof(uint16_t),
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL
    );
    ESP_ERROR_CHECK(draw_buffer ? ESP_OK : ESP_ERR_NO_MEM);

    gpio_config_t gpio_conf = {
        .pin_bit_mask = (1ULL << pin_dc),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    if (pin_reset >= 0) {
        gpio_conf.pin_bit_mask |= (1ULL << pin_reset);
    }

    if (pin_rd >= 0) {
        gpio_conf.pin_bit_mask |= (1ULL << pin_rd);
    }

    ESP_ERROR_CHECK(gpio_config(&gpio_conf));

    gpio_set_level(pin_dc, 0);

    if (pin_reset >= 0) {
        gpio_set_level(pin_reset, 1);
    }

    if (pin_rd >= 0) {
        gpio_set_level(pin_rd, 1);
    }

    esp_lcd_i80_bus_config_t bus_config = {
        .dc_gpio_num = pin_dc,
        .wr_gpio_num = pin_wr,
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .data_gpio_nums = {
            data_pins[0], data_pins[1],
            data_pins[2], data_pins[3],
            data_pins[4], data_pins[5],
            data_pins[6], data_pins[7]
        },
        .bus_width = 8,
        .max_transfer_bytes = DISPLAY_MAX_PIXELS * sizeof(uint16_t),
        .dma_burst_size = 64
    };

    ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_config, &i80_bus));

    esp_lcd_panel_io_i80_config_t io_config = {
        .cs_gpio_num = pin_cs,
        .pclk_hz = (uint32_t)clock_mhz * 1000000U,
        .trans_queue_depth = 1,
        .on_color_trans_done = color_trans_done_cb,
        .user_ctx = transfer_done,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_levels = {
            .dc_idle_level = 0,
            .dc_cmd_level = 0,
            .dc_dummy_level = 0,
            .dc_data_level = 1
        }
    };

    ESP_ERROR_CHECK(
        esp_lcd_new_panel_io_i80(i80_bus, &io_config, &panel_io)
    );

    lbm_add_extension("ext-disp-cmd", ext_disp_cmd);
    lbm_add_extension("ext-disp-orientation", ext_disp_orientation);

    disp_st7789_i80_reset();
}
