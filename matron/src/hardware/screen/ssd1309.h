#pragma once

#include <cairo.h>
#include <gpiod.h>
#include <linux/spi/spidev.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "platform.h"

#define SPIDEV_0_0_PATH "/dev/spidev0.0"
#define SPI0_BUS_WIDTH 8

// Pinout
// see: https://github.com/monome/norns-image/blob/main/readme-hardware.md#pinout-1
#define SSD1309_DC_AND_RESET_GPIO_CHIP "/dev/gpiochip0"
#define SSD1309_DC_GPIO_LINE 25
#define SSD1309_RESET_GPIO_LINE 27
#define SSD1309_MANUAL_CS_GPIO_LINE 24

// Commands
#define SSD1309_SET_COLUMN_ADDRESS 0x21
#define SSD1309_SET_PAGE_ADDRESS 0x22
#define SSD1309_SET_MEMORY_MODE 0x20
#define SSD1309_SET_DISPLAY_START_LINE 0x40
#define SSD1309_SET_CONTRAST_CURRENT 0x81
#define SSD1309_SET_SEGMENT_REMAP_0 0xA0
#define SSD1309_SET_SEGMENT_REMAP_127 0xA1
#define SSD1309_SET_DISPLAY_MODE_ALL_OFF 0xA4
#define SSD1309_SET_DISPLAY_MODE_ALL_ON 0xA5
#define SSD1309_SET_DISPLAY_MODE_NORMAL 0xA6
#define SSD1309_SET_DISPLAY_MODE_INVERT 0xA7
#define SSD1309_SET_MULTIPLEX_RATIO 0xA8
#define SSD1309_SET_DISPLAY_OFF 0xAE
#define SSD1309_SET_DISPLAY_ON 0xAF
#define SSD1309_SET_COM_SCAN_INC 0xC0
#define SSD1309_SET_COM_SCAN_DEC 0xC8
#define SSD1309_SET_DISPLAY_OFFSET 0xD3
#define SSD1309_SET_DISPLAY_CLOCK_DIV 0xD5
#define SSD1309_SET_PRECHARGE_PERIOD 0xD9
#define SSD1309_SET_COM_PINS_CONFIG 0xDA
#define SSD1309_SET_VCOM_DESELECT_LEVEL 0xDB
#define SSD1309_CHARGE_PUMP 0x8D
#define SSD1309_DEACTIVATE_SCROLL 0x2E

#define SSD1309_PIXEL_WIDTH 128
#define SSD1309_PIXEL_HEIGHT 64
#define SSD1309_PAGE_COUNT 8

typedef enum {
    SSD1309_DISPLAY_MODE_ALL_OFF = 0,
    SSD1309_DISPLAY_MODE_ALL_ON,
    SSD1309_DISPLAY_MODE_NORMAL,
    SSD1309_DISPLAY_MODE_INVERT,
} ssd1309_display_mode_t;

void ssd1309_init(void);
void ssd1309_deinit(void);
void ssd1309_refresh(void);
void ssd1309_update(cairo_surface_t *surface, bool should_translate_color);
void ssd1309_set_brightness(uint8_t value);
void ssd1309_set_contrast(uint8_t value);
void ssd1309_set_display_mode(ssd1309_display_mode_t mode);
void ssd1309_set_gamma(double gamma);
void ssd1309_set_refresh_rate(uint8_t hz);
uint8_t *ssd1309_resize_buffer(size_t size);
