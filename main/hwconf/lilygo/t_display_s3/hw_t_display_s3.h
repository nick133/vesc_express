/*
	Copyright 2024 Benjamin Vedder	benjamin@vedder.se

	This file is part of the VESC firmware.

	The VESC firmware is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    The VESC firmware is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
    */

#ifndef MAIN_HWCONF_VESC_DASH16_H_
#define MAIN_HWCONF_VESC_DASH16_H_

#include "driver/gpio.h"
#include "adc.h"

#define HW_NAME						"T-Display-S3"
#define HW_TARGET					"esp32s3_n16r8"
#define HW_NO_UART

#define HW_INIT_HOOK()				hw_init()

// CAN
#define CAN_TX_GPIO_NUM				21
#define CAN_RX_GPIO_NUM				20

// Display: ST7789, 8-bit I80 parallel
#define DISP_D0                     39
#define DISP_D1                     40
#define DISP_D2                     41
#define DISP_D3                     42
#define DISP_D4                     45
#define DISP_D5                     46
#define DISP_D6                     47
#define DISP_D7                     48

#define DISP_WR                     8
#define DISP_RD                     9
#define DISP_CS                     6
#define DISP_DC                     7
#define DISP_RESET                  5

#define DISP_BACKLIGHT              38
#define DISP_POWER                  15

// Buttons
#define PIN_BT1                     0
#define PIN_BT2                     14

// Functions
void hw_init(void);

#endif /* MAIN_HWCONF_VESC_DASH16_H_ */
