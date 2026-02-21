#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/display/cfb.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/sys/util.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>


static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static const struct device *adc_dev;
static const struct adc_channel_cfg temp_ch_cfg = {
	.gain = ADC_GAIN_1,
	.reference = ADC_REF_INTERNAL,
	.acquisition_time = ADC_ACQ_TIME_DEFAULT,
	.channel_id = 4,
};

static const struct device *display_dev;

static const struct device *cdc_dev;


struct monitor_state {
	float temperature;
	uint32_t uptime_secs;
	uint8_t thread_count;
	bool led_on;
	uint16_t blink_ms;
	char custom_msg[32];
};

static struct monitor_state state = {
	.temperature = 0.0f,
	.uptime_secs = 0,
	.thread_count = 4,
	.led_on = true,
	.blink_ms = 250,
	.custom_msg = "",
};

K_MUTEX_DEFINE(state_mutex);


static int16_t adc_buf;
static struct adc_sequence adc_seq = {
	.buffer = &adc_buf,
	.buffer_size = sizeof(adc_buf),
	.resolution = 12,
	.channels = BIT(4),
};

static float read_internal_temp(void)
{
	if (!adc_dev || !device_is_ready(adc_dev)) {
		return -99.0f;
	}

	int ret = adc_read(adc_dev, &adc_seq);
	if (ret < 0) {
		return -99.0f;
	}

	/* RP2040 datasheet temp conversion:
	 * T = 27 - (V_adc - 0.706) / 0.001721
	 * V_adc = raw * 3.3 / 4096
	 */
	float voltage = (float)adc_buf * 3.3f / 4096.0f;
	float temp = 27.0f - (voltage - 0.706f) / 0.001721f;
	return temp;
}

static void init_adc(void)
{
	adc_dev = DEVICE_DT_GET(DT_NODELABEL(adc));
	if (device_is_ready(adc_dev)) {
		adc_channel_setup(adc_dev, &temp_ch_cfg);
		printk("ADC initialized (ch4 = internal temp)\n");
	} else {
		printk("ADC not ready!\n");
	}
}


static void sensor_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	init_adc();

	while (1) {
		float temp = read_internal_temp();

		k_mutex_lock(&state_mutex, K_FOREVER);
		state.temperature = temp;
		state.uptime_secs = k_uptime_get_32() / 1000;
		k_mutex_unlock(&state_mutex);

		k_msleep(1000);
	}
}

K_THREAD_DEFINE(sensor_tid, 1024, sensor_thread_fn, NULL, NULL, NULL, 5, 0, 0);


static void display_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!device_is_ready(display_dev)) {
		printk("Display not ready\n");
		return;
	}

	if (display_set_pixel_format(display_dev, PIXEL_FORMAT_MONO10) != 0) {
		display_set_pixel_format(display_dev, PIXEL_FORMAT_MONO01);
	}

	if (cfb_framebuffer_init(display_dev)) {
		printk("CFB init failed\n");
		return;
	}

	cfb_framebuffer_clear(display_dev, true);
	display_blanking_off(display_dev);

	uint8_t font_w, font_h;
	int best_font = 0;
	for (int i = 0; i < 42; i++) {
		if (cfb_get_font_size(display_dev, i, &font_w, &font_h)) {
			break;
		}
		best_font = i;
		if (font_h <= 16) {
			break;
		}
	}
	cfb_framebuffer_set_font(display_dev, best_font);
	cfb_set_kerning(display_dev, 1);

	char line_buf[32];

	while (1) {
		k_mutex_lock(&state_mutex, K_FOREVER);
		bool led_st = state.led_on;
		uint16_t blink = state.blink_ms;
		char msg[32];
		strncpy(msg, state.custom_msg, sizeof(msg) - 1);
		msg[sizeof(msg) - 1] = '\0';
		k_mutex_unlock(&state_mutex);

		cfb_framebuffer_clear(display_dev, false);

		cfb_print(display_dev, "     SHRIKE", 0, 0);

		snprintf(line_buf, sizeof(line_buf), "LED: %s",
			 led_st ? "ON" : "OFF");
		cfb_print(display_dev, line_buf, 0, 16);

		if (msg[0] != '\0') {
			cfb_print(display_dev, msg, 0, 32);
		} else {
			cfb_print(display_dev, "> Ready", 0, 32);
		}

		cfb_framebuffer_finalize(display_dev);
		k_msleep(500);
	}
}

K_THREAD_DEFINE(display_tid, 2048, display_thread_fn, NULL, NULL, NULL, 6, 0, 0);


static void heartbeat_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	if (!gpio_is_ready_dt(&led)) {
		printk("LED GPIO not ready\n");
		return;
	}

	gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
	printk("LED GPIO configured on pin %d\n", led.pin);

	while (1) {
		uint16_t blink;
		bool on;

		k_mutex_lock(&state_mutex, K_FOREVER);
		blink = state.blink_ms;
		on = state.led_on;
		k_mutex_unlock(&state_mutex);

		if (on) {
			gpio_pin_toggle_dt(&led);
		} else {
			gpio_pin_set_dt(&led, 0);
		}

		k_msleep(blink);
	}
}

K_THREAD_DEFINE(heartbeat_tid, 512, heartbeat_thread_fn, NULL, NULL, NULL, 7, 0, 0);


static void send_telemetry(const struct device *dev)
{
	char buf[128];

	k_mutex_lock(&state_mutex, K_FOREVER);
	int len = snprintf(buf, sizeof(buf),
		"{\"temp\":%.1f,\"up\":%u,\"thds\":%u,\"led\":%u,\"blink\":%u}\n",
		(double)state.temperature,
		state.uptime_secs,
		state.thread_count,
		state.led_on ? 1 : 0,
		state.blink_ms);
	k_mutex_unlock(&state_mutex);

	for (int i = 0; i < len; i++) {
		uart_poll_out(dev, buf[i]);
	}
}

static void parse_command(const char *json)
{
	const char *cmd_pos = strstr(json, "\"cmd\":\"");
	if (!cmd_pos) return;
	cmd_pos += 7;

	const char *val_pos = strstr(json, "\"val\":");
	int val = 0;
	if (val_pos) {
		val_pos += 6;
		val = atoi(val_pos);
	}

	k_mutex_lock(&state_mutex, K_FOREVER);

	if (strncmp(cmd_pos, "led", 3) == 0) {
		state.led_on = (val != 0);
	} else if (strncmp(cmd_pos, "blink", 5) == 0) {
		if (val >= 50 && val <= 2000) {
			state.blink_ms = (uint16_t)val;
		}
	} else if (strncmp(cmd_pos, "oled_msg", 8) == 0) {
		const char *str_val = strstr(json, "\"val\":\"");
		if (str_val) {
			str_val += 7;
			const char *end = strchr(str_val, '"');
			if (end) {
				size_t slen = MIN((size_t)(end - str_val),
						  sizeof(state.custom_msg) - 1);
				memcpy(state.custom_msg, str_val, slen);
				state.custom_msg[slen] = '\0';
			}
		}
	}

	k_mutex_unlock(&state_mutex);
}

static void serial_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	cdc_dev = DEVICE_DT_GET_ONE(zephyr_cdc_acm_uart);
	if (!device_is_ready(cdc_dev)) {
		printk("CDC ACM not ready\n");
		return;
	}

	int ret = usb_enable(NULL);
	if (ret != 0 && ret != -EALREADY) {
		printk("USB enable failed: %d\n", ret);
		return;
	}

	uint32_t dtr = 0;
	while (!dtr) {
		uart_line_ctrl_get(cdc_dev, UART_LINE_CTRL_DTR, &dtr);
		k_msleep(100);
	}

	k_msleep(500);

	char rx_buf[128];
	int rx_pos = 0;

	while (1) {
		unsigned char c;
		while (uart_poll_in(cdc_dev, &c) == 0) {
			if (c == '\n' || c == '\r') {
				if (rx_pos > 0) {
					rx_buf[rx_pos] = '\0';
					parse_command(rx_buf);
					rx_pos = 0;
				}
			} else if (rx_pos < (int)sizeof(rx_buf) - 1) {
				rx_buf[rx_pos++] = (char)c;
			}
		}

		send_telemetry(cdc_dev);
		k_msleep(500);
	}
}

K_THREAD_DEFINE(serial_tid, 2048, serial_thread_fn, NULL, NULL, NULL, 4, 0, 0);

int main(void)
{
	printk("ShrikeOS Monitor starting...\n");
	printk("Board: Shrike-lite (RP2040 + SLG47910)\n");
	printk("LED: GPIO %d (blink thread)\n", led.pin);
	printk("Threads: sensor, display, heartbeat, serial\n");

	return 0;
}
