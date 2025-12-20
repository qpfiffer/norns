// vim: noet ts=4 sw=4
#include "ssd1322.h"

#define SPIDEV_BUFFER_LEN SSD1322_PIXEL_WIDTH *SSD1322_PIXEL_HEIGHT * sizeof(uint8_t)
#define SURFACE_BUFFER_LEN SSD1322_PIXEL_WIDTH *SSD1322_PIXEL_HEIGHT * sizeof(uint32_t)

static int spidev_fd = 0;

static uint8_t *spidev_buffer = NULL;
static uint32_t *surface_buffer = NULL;

static struct gpiod_line_request *gpio_dc = NULL;
static struct gpiod_line_request *gpio_reset = NULL;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

int _open_spi() {
	uint8_t mode = SPI_MODE_0;
	uint8_t bits_per_word = SPI0_BUS_WIDTH;
	uint8_t little_endian = 0;
	uint32_t speed_hz = 1200000000 / 64; // 18.75Mhz, 1200Mhz is the CPU speed.

	int fd = open(SPIDEV_0_0_PATH, O_RDWR | O_SYNC);

	if (fd < 0) {
		fprintf(stderr, "(screen) couldn't open %s\n", SPIDEV_0_0_PATH);
		return -1;
	}

	int outcome = 0 || (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0) || (ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits_per_word) < 0) || (ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed_hz) < 0) || (ioctl(fd, SPI_IOC_WR_LSB_FIRST, &little_endian) < 0);
	if (outcome != 0) {
		fprintf(stderr, "could not set SPI WR settings via IOC\n");
		close(fd);
		return -1;
	}

	return fd;
}

static struct gpiod_line_request *
_request_output_line(const char *chip_path, unsigned int offset,
		    enum gpiod_line_value value, const char *consumer)
{
	/* Copied from libgpiod examples directory. */
	struct gpiod_request_config *req_cfg = NULL;
	struct gpiod_line_request *request = NULL;
	struct gpiod_line_settings *settings;
	struct gpiod_line_config *line_cfg;
	struct gpiod_chip *chip;
	int ret;

	chip = gpiod_chip_open(chip_path);
	if (!chip)
		return NULL;

	settings = gpiod_line_settings_new();
	if (!settings)
		goto close_chip;

	gpiod_line_settings_set_direction(settings,
					  GPIOD_LINE_DIRECTION_OUTPUT);
	gpiod_line_settings_set_output_value(settings, value);

	line_cfg = gpiod_line_config_new();
	if (!line_cfg)
		goto free_settings;

	ret = gpiod_line_config_add_line_settings(line_cfg, &offset, 1,
						  settings);
	if (ret)
		goto free_line_config;

	if (consumer) {
		req_cfg = gpiod_request_config_new();
		if (!req_cfg)
			goto free_line_config;

		gpiod_request_config_set_consumer(req_cfg, consumer);
	}

	request = gpiod_chip_request_lines(chip, req_cfg, line_cfg);

	gpiod_request_config_free(req_cfg);

free_line_config:
	gpiod_line_config_free(line_cfg);

free_settings:
	gpiod_line_settings_free(settings);

close_chip:
	gpiod_chip_close(chip);

	return request;
}

void ssd1322_init() {
	if (pthread_mutex_init(&lock, NULL) != 0) {
		fprintf(stderr, "%s: pthread_mutex_init failed\n", __func__);
		return;
	}

	surface_buffer = calloc(SURFACE_BUFFER_LEN, 1);
	if (surface_buffer == NULL) {
		fprintf(stderr, "%s: couldn't allocate surface_buffer\n", __func__);
		return;
	}

	spidev_buffer = calloc(SPIDEV_BUFFER_LEN, 1);
	if (spidev_buffer == NULL) {
		fprintf(stderr, "%s: couldn't allocate spidev_buffer\n", __func__);
		return;
	}

	spidev_fd = _open_spi(SPIDEV_0_0_PATH);
	if (spidev_fd < 0) {
		fprintf(stderr, "%s: couldn't open %s.\n", __func__, SPIDEV_0_0_PATH);
		return;
	}

	enum gpiod_line_value ACTIVE_VALUE = GPIOD_LINE_VALUE_ACTIVE;

	gpio_dc = _request_output_line(SSD1322_DC_AND_RESET_GPIO_CHIP, SSD1322_DC_GPIO_LINE,
			ACTIVE_VALUE, "D/C");
	gpio_reset = _request_output_line(SSD1322_DC_AND_RESET_GPIO_CHIP, SSD1322_RESET_GPIO_LINE,
			ACTIVE_VALUE, "RST");

	if (!gpio_dc || !gpio_reset) {
		fprintf(stderr, "%s: couldn't request output lines for dc/rst.\n", __func__);
		return;
	}

	// SSD1322 Reference Document (v1.2) P 16/60
	// "Keep this pin pull HIGH during normal operation"
	gpiod_line_request_set_values(gpio_reset, 1);

	// All values copied from fbtft-ssd1322.c from monome/linux repo.
	// write_command(SSD1322_SET_DISPLAY_OFF);
	// write_command(SSD1322_SET_DEFAULT_LINEAR_GRAY_SCALE);
	// write_command_with_data(SSD1322_SET_OSCILLATOR_FREQUENCY, 0x91);
	// write_command_with_data(SSD1322_SET_MULTIPLEX_RATIO, NORNS_MUX_RATIO);
	// write_command_with_data(SSD1322_SET_DISPLAY_OFFSET, 0x00);
	// write_command_with_data(SSD1322_SET_DISPLAY_START_LINE, 0x00);
	// write_command_with_data(SSD1322_SET_VDD_REGULATOR, 0x01);
	// write_command_with_data(SSD1322_SET_DISPLAY_ENHANCEMENT_A, 0xA0, 0xFD);
	// write_command_with_data(SSD1322_SET_CONTRAST_CURRENT, 0x7F);
	// write_command_with_data(SSD1322_MASTER_CURRENT_CONTROL, 0x0F);
	// write_command_with_data(SSD1322_SET_PHASE_LENGTH, NORNS_PHASE_LENGTH);
	// write_command_with_data(SSD1322_SET_PRECHARGE_VOLTAGE, 0x1F);
	// write_command_with_data(SSD1322_SET_VCOMH_VOLTAGE, 0x04);
	// write_command(SSD1322_SET_DISPLAY_MODE_NORMAL);

	// Flips the screen orientation if the device is a norns shield.
	//if (platform_factory()) {
	//	write_command_with_data(SSD1322_SET_DUAL_COMM_LINE_MODE, 0x16, 0x11);
	//} else {
	//	write_command_with_data(SSD1322_SET_DUAL_COMM_LINE_MODE, 0x04, 0x11);
	//}

	// Do not turn display on until the first update has been called,
	// otherwise previous GDDRAM (or noise) will display before the
	// "hello" startup screen.

	// Set high thread priority to avoid flashing.
	static struct sched_param param;
	param.sched_priority = sched_get_priority_max(SCHED_OTHER);

	// Start thread.
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
	pthread_attr_setschedparam(&attr, &param);
	pthread_create(&ssd1322_pthread_t, &attr, &ssd1322_thread_run, NULL);
	pthread_attr_destroy(&attr);
}

void ssd1322_deinit() {
}

void ssd1322_refresh() {
}

void ssd1322_update(cairo_surface_t *surface, bool should_translate_color) {
    (void)surface;
    (void)should_translate_color;
}

void ssd1322_set_brightness(uint8_t b){
    (void)b;
}

void ssd1322_set_contrast(uint8_t c){
    (void)c;
}

void ssd1322_set_display_mode(ssd1322_display_mode_t mode){
    (void)mode;
}

void ssd1322_set_gamma(double g){
    (void) g;
}

void ssd1322_set_refresh_rate(uint8_t hz){
    (void) hz;
}

uint8_t *ssd1322_resize_buffer(size_t siz){
    (void)siz;
    return 0;
}
