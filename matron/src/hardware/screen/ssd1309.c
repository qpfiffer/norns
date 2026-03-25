#include "ssd1309.h"

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "event_types.h"
#include "events.h"

static int spidev_fd = -1;
static bool display_dirty = false;
static bool should_translate_color = false;
static bool should_turn_on = true;
static bool thread_running = false;
static uint8_t *spidev_buffer = NULL;
static uint32_t *surface_buffer = NULL;
static struct gpiod_line_request *gpio_dc = NULL;
static struct gpiod_line_request *gpio_reset = NULL;
static struct gpiod_line_request *gpio_cs = NULL;
static pthread_mutex_t lock;
static bool lock_initialized = false;
static pthread_t ssd1309_pthread_t;

#define SPIDEV_BUFFER_LEN ((SSD1309_PIXEL_WIDTH * SSD1309_PIXEL_HEIGHT) / 8)
#define SURFACE_BUFFER_LEN (SSD1309_PIXEL_WIDTH * SSD1309_PIXEL_HEIGHT * sizeof(uint32_t))
#define SSD1309_COLUMN_OFFSET 2

static int open_spi(void) {
    uint8_t mode = SPI_MODE_0;
    uint8_t bits_per_word = SPI0_BUS_WIDTH;
    uint8_t little_endian = 0;
    uint32_t speed_hz = 8000000; // faster refresh to reduce visible tearing

    int fd = open(SPIDEV_0_0_PATH, O_RDWR | O_SYNC);

    if (fd < 0) {
        fprintf(stderr, "(screen) couldn't open %s\n", SPIDEV_0_0_PATH);
        return -1;
    }

    int outcome =
        0 ||
        (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0) ||
        (ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits_per_word) < 0) ||
        (ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed_hz) < 0) ||
        (ioctl(fd, SPI_IOC_WR_LSB_FIRST, &little_endian) < 0);

    if (outcome != 0) {
        fprintf(stderr, "could not set SPI WR settings via IOC\n");
        close(fd);
        return -1;
    }

    return fd;
}

static struct gpiod_line_request *
request_output_line(const char *chip_path,
                    unsigned int offset,
                    enum gpiod_line_value initial_value,
                    const char *consumer) {
    struct gpiod_request_config *req_cfg = NULL;
    struct gpiod_line_request *request = NULL;
    struct gpiod_line_settings *settings = NULL;
    struct gpiod_line_config *line_cfg = NULL;
    struct gpiod_chip *chip = NULL;
    int ret = 0;

    chip = gpiod_chip_open(chip_path);
    if (!chip) {
        goto cleanup;
    }

    settings = gpiod_line_settings_new();
    if (!settings) {
        goto cleanup;
    }

    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);
    gpiod_line_settings_set_output_value(settings, initial_value);

    line_cfg = gpiod_line_config_new();
    if (!line_cfg) {
        goto cleanup;
    }

    ret = gpiod_line_config_add_line_settings(line_cfg, &offset, 1, settings);
    if (ret) {
        goto cleanup;
    }

    req_cfg = gpiod_request_config_new();
    if (!req_cfg) {
        goto cleanup;
    }

    gpiod_request_config_set_consumer(req_cfg, consumer);

    request = gpiod_chip_request_lines(chip, req_cfg, line_cfg);

cleanup:
    gpiod_request_config_free(req_cfg);
    gpiod_line_config_free(line_cfg);
    gpiod_line_settings_free(settings);

    if (chip) {
        gpiod_chip_close(chip);
    }

    return request;
}

static int set_output_line(struct gpiod_line_request *request, enum gpiod_line_value value) {
    if (!request) {
        return -1;
    }

    enum gpiod_line_value values[] = {value};
    return gpiod_line_request_set_values(request, values);
}

static void cs_assert(void) {
    if (gpio_cs && set_output_line(gpio_cs, GPIOD_LINE_VALUE_INACTIVE) != 0) {
        fprintf(stderr, "%s: failed to assert CS\n", __func__);
    }
}

static void cs_deassert(void) {
    if (gpio_cs && set_output_line(gpio_cs, GPIOD_LINE_VALUE_ACTIVE) != 0) {
        fprintf(stderr, "%s: failed to deassert CS\n", __func__);
    }
}

static int ssd1309_write_command(uint8_t command, uint8_t data_len, ...) {
    va_list args;
    uint8_t cmd_buf[1];
    uint8_t data_buf[256];
    struct spi_ioc_transfer transfer = {0};

    pthread_mutex_lock(&lock);

    if (spidev_fd < 0 || !gpio_dc) {
        fprintf(stderr, "%s: driver not initialized\n", __func__);
        goto fail;
    }

    cs_assert();

    if (set_output_line(gpio_dc, GPIOD_LINE_VALUE_INACTIVE) != 0) {
        fprintf(stderr, "%s: failed to set D/C for command\n", __func__);
        goto fail;
    }

    cmd_buf[0] = command;
    transfer.tx_buf = (unsigned long)cmd_buf;
    transfer.len = (uint32_t)sizeof(cmd_buf);

    if (ioctl(spidev_fd, SPI_IOC_MESSAGE(1), &transfer) < 0) {
        fprintf(stderr, "%s: could not send command\n", __func__);
        goto fail;
    }

    if (data_len > 0) {
        if (set_output_line(gpio_dc, GPIOD_LINE_VALUE_ACTIVE) != 0) {
            fprintf(stderr, "%s: failed to set D/C for data\n", __func__);
            goto fail;
        }

        va_start(args, data_len);
        for (uint8_t i = 0; i < data_len; i++) {
            data_buf[i] = (uint8_t)va_arg(args, int);
        }
        va_end(args);

        transfer.tx_buf = (unsigned long)data_buf;
        transfer.len = (uint32_t)data_len;

        if (ioctl(spidev_fd, SPI_IOC_MESSAGE(1), &transfer) < 0) {
            fprintf(stderr, "%s: could not send command data\n", __func__);
            goto fail;
        }
    }

    cs_deassert();
    pthread_mutex_unlock(&lock);
    return 0;

fail:
    cs_deassert();
    pthread_mutex_unlock(&lock);
    return -1;
}

#ifndef NUMARGS
#define NUMARGS(...) (sizeof((int[]){__VA_ARGS__}) / sizeof(int))
#endif
#define write_command_with_data(cmd, ...) (ssd1309_write_command(cmd, NUMARGS(__VA_ARGS__), __VA_ARGS__))
#define write_command(cmd) (ssd1309_write_command(cmd, 0, 0))

static inline uint8_t grayscale_to_binary(uint8_t b, uint8_t g, uint8_t r) {
    // Preserve luminance if source has color channels, otherwise any channel works.
    uint16_t luminance = ((uint16_t)r * 54u) + ((uint16_t)g * 183u) + ((uint16_t)b * 19u);
    return (uint8_t)(luminance >> 8);
}

static void surface_to_spidev_buffer(void) {
    memset(spidev_buffer, 0, SPIDEV_BUFFER_LEN);

    for (uint32_t y = 0; y < SSD1309_PIXEL_HEIGHT; y++) {
        for (uint32_t x = 0; x < SSD1309_PIXEL_WIDTH; x++) {
            uint8_t *pixel = (uint8_t *)&surface_buffer[(y * SSD1309_PIXEL_WIDTH) + x];
            uint8_t gray = should_translate_color
                ? grayscale_to_binary(pixel[0], pixel[1], pixel[2])
                : pixel[1];

            if (gray >= 8) {
                uint32_t page = y >> 3;
                uint32_t idx = (page * SSD1309_PIXEL_WIDTH) + x;
                spidev_buffer[idx] |= (uint8_t)(1u << (y & 0x7u));
            }
        }
    }
}

static void *ssd1309_thread_run(void *p) {
    (void)p;

    static struct timespec ts = {
        .tv_sec = 0,
        .tv_nsec = 16666666, // 60Hz
    };

    while (thread_running) {
        if (display_dirty) {
            ssd1309_refresh();
            display_dirty = false;
        }

        event_post(event_data_new(EVENT_SCREEN_REFRESH));
        clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
    }

    return NULL;
}

void ssd1309_init(void) {
    should_turn_on = true;
    display_dirty = false;
    should_translate_color = false;

    if (pthread_mutex_init(&lock, NULL) != 0) {
        fprintf(stderr, "%s: pthread_mutex_init failed\n", __func__);
        return;
    }
    lock_initialized = true;

    surface_buffer = calloc(SURFACE_BUFFER_LEN, 1);
    if (!surface_buffer) {
        fprintf(stderr, "%s: couldn't allocate surface_buffer\n", __func__);
        goto fail;
    }

    spidev_buffer = calloc(SPIDEV_BUFFER_LEN, 1);
    if (!spidev_buffer) {
        fprintf(stderr, "%s: couldn't allocate spidev_buffer\n", __func__);
        goto fail;
    }

    spidev_fd = open_spi();
    if (spidev_fd < 0) {
        fprintf(stderr, "%s: couldn't open %s\n", __func__, SPIDEV_0_0_PATH);
        goto fail;
    }

    gpio_dc = request_output_line(
        SSD1309_DC_AND_RESET_GPIO_CHIP,
        SSD1309_DC_GPIO_LINE,
        GPIOD_LINE_VALUE_INACTIVE,
        "D/C"
    );
    gpio_reset = request_output_line(
        SSD1309_DC_AND_RESET_GPIO_CHIP,
        SSD1309_RESET_GPIO_LINE,
        GPIOD_LINE_VALUE_INACTIVE,
        "RST"
    );
    gpio_cs = request_output_line(
        SSD1309_DC_AND_RESET_GPIO_CHIP,
        SSD1309_MANUAL_CS_GPIO_LINE,
        GPIOD_LINE_VALUE_ACTIVE,
        "CS"
    );

    if (!gpio_dc || !gpio_reset || !gpio_cs) {
        fprintf(stderr, "%s: couldn't request output lines for dc/rst/cs\n", __func__);
        goto fail;
    }
    // Hardware reset sequence (match known-good SSD1309 drivers): high -> low -> high.
    set_output_line(gpio_reset, GPIOD_LINE_VALUE_ACTIVE);
    usleep(1000);
    set_output_line(gpio_reset, GPIOD_LINE_VALUE_INACTIVE);
    usleep(10000);
    set_output_line(gpio_reset, GPIOD_LINE_VALUE_ACTIVE);
    usleep(10000);

    // Keep command order aligned with known-good SSD1309 init sequences.
    write_command(SSD1309_SET_DISPLAY_OFF);
    write_command_with_data(SSD1309_SET_DISPLAY_CLOCK_DIV, 0x80);
    write_command_with_data(SSD1309_SET_MULTIPLEX_RATIO, 0x3F);
    write_command_with_data(SSD1309_SET_DISPLAY_OFFSET, 0x00);
    write_command((uint8_t)(SSD1309_SET_DISPLAY_START_LINE | 0x00));
    write_command_with_data(SSD1309_CHARGE_PUMP, 0x14);
    write_command_with_data(SSD1309_SET_MEMORY_MODE, 0x02);

    write_command(SSD1309_SET_SEGMENT_REMAP_0);
    write_command(SSD1309_SET_COM_SCAN_INC);

    write_command_with_data(SSD1309_SET_COM_PINS_CONFIG, 0x12);
    write_command_with_data(SSD1309_SET_CONTRAST_CURRENT, 0xFF);
    write_command_with_data(SSD1309_SET_PRECHARGE_PERIOD, 0xF1);
    write_command_with_data(SSD1309_SET_VCOM_DESELECT_LEVEL, 0x40);
    write_command(SSD1309_DEACTIVATE_SCROLL);
    write_command(SSD1309_SET_DISPLAY_MODE_ALL_OFF);
    write_command(SSD1309_SET_DISPLAY_MODE_NORMAL);
    write_command(SSD1309_SET_DISPLAY_ON);
    should_turn_on = false;

    thread_running = true;

    // Set high thread priority to avoid flashing.
    static struct sched_param param;
    param.sched_priority = sched_get_priority_max(SCHED_OTHER);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedparam(&attr, &param);
    if (pthread_create(&ssd1309_pthread_t, &attr, &ssd1309_thread_run, NULL) != 0) {
        fprintf(stderr, "%s: failed to create refresh thread\n", __func__);
        pthread_attr_destroy(&attr);
        thread_running = false;
        goto fail;
    }
    pthread_attr_destroy(&attr);

    return;

fail:
    ssd1309_deinit();
}

void ssd1309_deinit(void) {
    thread_running = false;
    if (ssd1309_pthread_t) {
        pthread_join(ssd1309_pthread_t, NULL);
        ssd1309_pthread_t = (pthread_t){0};
    }

    if (spidev_fd >= 0 && gpio_dc) {
        write_command(SSD1309_SET_DISPLAY_OFF);
    }

    if (gpio_reset) {
        set_output_line(gpio_reset, GPIOD_LINE_VALUE_INACTIVE);
    }
    cs_deassert();

    if (lock_initialized) {
        pthread_mutex_destroy(&lock);
        lock_initialized = false;
    }

    if (gpio_reset) {
        gpiod_line_request_release(gpio_reset);
        gpio_reset = NULL;
    }

    if (gpio_dc) {
        gpiod_line_request_release(gpio_dc);
        gpio_dc = NULL;
    }

    if (gpio_cs) {
        gpiod_line_request_release(gpio_cs);
        gpio_cs = NULL;
    }

    if (spidev_fd >= 0) {
        close(spidev_fd);
        spidev_fd = -1;
    }

    free(spidev_buffer);
    spidev_buffer = NULL;

    free(surface_buffer);
    surface_buffer = NULL;

    display_dirty = false;
    should_turn_on = true;
}

void ssd1309_update(cairo_surface_t *surface_pointer, bool surface_has_color) {
    pthread_mutex_lock(&lock);

    should_translate_color = surface_has_color;

    if (surface_buffer && surface_pointer) {
        const uint32_t surface_w = cairo_image_surface_get_width(surface_pointer);
        const uint32_t surface_h = cairo_image_surface_get_height(surface_pointer);
        cairo_format_t surface_f = cairo_image_surface_get_format(surface_pointer);

        if (surface_w != SSD1309_PIXEL_WIDTH || surface_h != SSD1309_PIXEL_HEIGHT || surface_f != CAIRO_FORMAT_ARGB32) {
            fprintf(stderr, "%s: invalid surface shape/format\n", __func__);
            goto early_return;
        }

        memcpy(surface_buffer, cairo_image_surface_get_data(surface_pointer), SURFACE_BUFFER_LEN);
    }

    display_dirty = true;

early_return:
    pthread_mutex_unlock(&lock);
}

void ssd1309_refresh(void) {
    struct spi_ioc_transfer transfer = {0};

    if (spidev_fd < 0 || !spidev_buffer || !surface_buffer || !gpio_dc) {
        return;
    }

    if (should_turn_on) {
        write_command(SSD1309_SET_DISPLAY_ON);
        should_turn_on = false;
    }

    write_command((uint8_t)(SSD1309_SET_DISPLAY_START_LINE | 0x00));
    write_command_with_data(SSD1309_SET_DISPLAY_OFFSET, 0x00);

    pthread_mutex_lock(&lock);
    surface_to_spidev_buffer();
    pthread_mutex_unlock(&lock);

    for (uint8_t page = 0; page < SSD1309_PAGE_COUNT; page++) {
        write_command((uint8_t)(0xB0 | page));
        write_command((uint8_t)(0x00 | (SSD1309_COLUMN_OFFSET & 0x0F)));
        write_command((uint8_t)(0x10 | ((SSD1309_COLUMN_OFFSET >> 4) & 0x0F)));

        pthread_mutex_lock(&lock);
        if (set_output_line(gpio_dc, GPIOD_LINE_VALUE_ACTIVE) != 0) {
            fprintf(stderr, "%s: failed to set D/C for data\n", __func__);
            pthread_mutex_unlock(&lock);
            break;
        }

        cs_assert();
        transfer.tx_buf = (unsigned long)(spidev_buffer + (page * SSD1309_PIXEL_WIDTH));
        transfer.len = SSD1309_PIXEL_WIDTH;

        if (ioctl(spidev_fd, SPI_IOC_MESSAGE(1), &transfer) < 0) {
            fprintf(stderr, "%s: SPI page transfer failed (%u)\n", __func__, page);
            cs_deassert();
            pthread_mutex_unlock(&lock);
            break;
        }

        cs_deassert();
        pthread_mutex_unlock(&lock);
    }
}

void ssd1309_set_brightness(uint8_t value) {
    if (value > 15) {
        value = 15;
    }

    // Map norns brightness range [0,15] to contrast range [0,255].
    ssd1309_set_contrast((uint8_t)(value * 17));
}

void ssd1309_set_contrast(uint8_t value) {
    write_command_with_data(SSD1309_SET_CONTRAST_CURRENT, value);
}

void ssd1309_set_display_mode(ssd1309_display_mode_t mode_offset) {
    write_command((uint8_t)(SSD1309_SET_DISPLAY_MODE_ALL_OFF + mode_offset));
}

void ssd1309_set_gamma(double gamma) {
    (void)gamma;
    // SSD1309 does not expose a programmable grayscale table like SSD1322.
}

void ssd1309_set_refresh_rate(uint8_t hz) {
    (void)hz;
    // Refresh pacing is handled by the update thread.
}

uint8_t *ssd1309_resize_buffer(size_t size) {
    spidev_buffer = realloc(spidev_buffer, size);
    return spidev_buffer;
}

#undef NUMARGS
#undef write_command
#undef write_command_with_data
