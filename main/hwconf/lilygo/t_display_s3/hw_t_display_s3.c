#include "hw_t_display_s3.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#include "hw.h"
#include "disp_st7789_i80.h"
#include "lispif.h"
#include "lispbm.h"
#include "lispif_disp_extensions.h"

static const int display_data_pins[8] = {
    DISP_D0, DISP_D1, DISP_D2, DISP_D3,
    DISP_D4, DISP_D5, DISP_D6, DISP_D7
};

static lbm_value ext_disp_init(lbm_value *args, lbm_uint argn) {
    (void)args;
    (void)argn;

    disp_st7789_i80_init(
        display_data_pins,
        DISP_WR,
        DISP_CS,
        DISP_RESET,
        DISP_DC,
        DISP_RD,
        320,
        170,
        10
    );

    lbm_display_extensions_set_callbacks(
        disp_st7789_i80_render_image,
        disp_st7789_i80_clear,
        disp_st7789_i80_reset
    );

    gpio_set_level(DISP_BACKLIGHT, 1);

    return ENC_SYM_TRUE;
}

static lbm_value ext_bt1(lbm_value *args, lbm_uint argn) {
    (void)args;
    (void)argn;
    return gpio_get_level(PIN_BT1) == 0 ? ENC_SYM_TRUE : ENC_SYM_NIL;
}

static lbm_value ext_bt2(lbm_value *args, lbm_uint argn) {
    (void)args;
    (void)argn;
    return gpio_get_level(PIN_BT2) == 0 ? ENC_SYM_TRUE : ENC_SYM_NIL;
}

static void load_extensions(bool main_found) {
    if (main_found) {
        return;
    }

    lbm_add_extension("disp-init", ext_disp_init);
    lbm_add_extension("bt1-pressed", ext_bt1);
    lbm_add_extension("bt2-pressed", ext_bt2);
}

void hw_init(void) {
    gpio_reset_pin(PIN_BT1);
    gpio_set_direction(PIN_BT1, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_BT1, GPIO_PULLUP_ONLY);

    gpio_reset_pin(PIN_BT2);
    gpio_set_direction(PIN_BT2, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_BT2, GPIO_PULLUP_ONLY);

    gpio_reset_pin(DISP_POWER);
    gpio_set_direction(DISP_POWER, GPIO_MODE_OUTPUT);
    gpio_set_level(DISP_POWER, 1);

    gpio_reset_pin(DISP_BACKLIGHT);
    gpio_set_direction(DISP_BACKLIGHT, GPIO_MODE_OUTPUT);
    gpio_set_level(DISP_BACKLIGHT, 0);

    vTaskDelay(pdMS_TO_TICKS(10));

    lispif_add_ext_load_callback(load_extensions);
}
