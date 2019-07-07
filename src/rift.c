/*
 * Oculus Rift HMDs
 * Copyright 2015-2016 Philipp Zabel
 * SPDX-License-Identifier:	LGPL-2.0+ or BSL-1.0
 */
#include <asm/byteorder.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <math.h>

#include "rift.h"
#include "rift-hid-reports.h"
#include "rift-radio.h"
#include "debug.h"
#include "device.h"
#include "hidraw.h"
#include "imu.h"
#include "maths.h"
#include "leds.h"
#include "telemetry.h"
#include "tracker.h"

/* 44 LEDs + 1 IMU on CV1 */
#define MAX_POSITIONS	45

enum rift_type {
	RIFT_DK2,
	RIFT_CV1,
};

struct _OuvrtRift {
	OuvrtDevice dev;
	OuvrtTracker *tracker;

	enum rift_type type;
	struct leds leds;
	vec3 imu_position;

	unsigned char uuid[20];
	int report_rate;
	int report_interval;
	gboolean flicker;
	bool reboot;
	uint8_t boot_mode;
	uint64_t last_message_time;
	uint64_t last_sample_timestamp;
	uint32_t last_exposure_timestamp;
	int32_t last_exposure_count;
	struct rift_radio radio;
	struct imu_state imu;
};

G_DEFINE_TYPE(OuvrtRift, ouvrt_rift, OUVRT_TYPE_DEVICE)

/*
 * Unpacks three signed 21-bit values packed into a big-endian 64-bit value
 * and stores them in a floating point vector after multiplying by scale.
 */
static inline void unpack_3x21bit(float scale, __be64 buf, vec3 *v)
{
	uint64_t xyz = __be64_to_cpu(buf);

	v->x = scale * ((int64_t)xyz >> 43);
	v->y = scale * ((int64_t)(xyz << 21) >> 43);
	v->z = scale * ((int64_t)(xyz << 42) >> 43);
}

/*
 * Returns the current sensor configuration.
 */
static int rift_get_config(OuvrtRift *rift)
{
	struct rift_config_report report = {
		.id = RIFT_CONFIG_REPORT_ID,
	};
	uint16_t sample_rate;
	uint16_t report_rate;
	int ret;

	ret = hid_get_feature_report(rift->dev.fd, &report, sizeof(report));
	if (ret < 0)
		return ret;

	sample_rate = __le16_to_cpu(report.sample_rate);
	report_rate = sample_rate / (report.packet_interval + 1);

	g_print("Rift: Got sample rate %d Hz, report rate %d Hz, flags: 0x%x\n",
		sample_rate, report_rate, report.flags);

	rift->report_rate = report_rate;
	rift->report_interval = 1000000 / report_rate;

	return 0;
}

/*
 * Reads the IMU factory calibration
 */
static int rift_get_imu_calibration(OuvrtRift *rift)
{
	struct rift_imu_calibration_report report = {
		.id = RIFT_IMU_CALIBRATION_REPORT_ID,
	};
	int ret;
	vec3 accel_offset;
	vec3 gyro_offset;
	float accel_matrix[3][3];
	float gyro_matrix[3][3];
	float temperature;
	float scale = 1.0f / ((1 << 20) - 1);
	int i;

	ret = hid_get_feature_report(rift->dev.fd, &report, sizeof(report));
	if (ret < 0)
		return ret;

	/* 10⁻⁴ m/s² */
	unpack_3x21bit(1e-4f, report.accel_offset, &accel_offset);
	/* 10⁻⁴ rad/s */
	unpack_3x21bit(1e-4f, report.gyro_offset, &gyro_offset);

	for (i = 0; i < 3; i++) {
		unpack_3x21bit(scale, report.accel_matrix[i],
			       (vec3 *)&accel_matrix[i]);
		accel_matrix[i][i] += 1.0f;
		unpack_3x21bit(scale, report.gyro_matrix[i],
			       (vec3 *)&gyro_matrix[i]);
		gyro_matrix[i][i] += 1.0f;
	}

	temperature = 0.01f * __le16_to_cpu(report.temperature);

	(void)temperature;

	return 0;
}

/*
 * Configures the sensor report rate
 */
static int rift_set_report_rate(OuvrtRift *rift, int report_rate)
{
	struct rift_config_report report = {
		.id = RIFT_CONFIG_REPORT_ID,
	};
	uint16_t sample_rate;
	int ret;

	ret = hid_get_feature_report(rift->dev.fd, &report, sizeof(report));
	if (ret < 0)
		return ret;

	sample_rate = __le16_to_cpu(report.sample_rate);

	if (report_rate > sample_rate)
		report_rate = sample_rate;
	if (report_rate < 5)
		report_rate = 5;

	report.packet_interval = sample_rate / report_rate - 1;

	g_print("Rift: Set sample rate %d Hz, report rate %d Hz\n",
		sample_rate, report_rate);

	ret = hid_send_feature_report(rift->dev.fd, &report, sizeof(report));
	if (ret < 0)
		return ret;

	rift->report_rate = report_rate;
	rift->report_interval = 1000000 / report_rate;

	return 0;
}

/*
 * Reads the gyro, accelerometer, and magnetometer ranges
 */
static int rift_get_ranges(OuvrtRift *rift)
{
	struct rift_range_report report = {
		.id = RIFT_RANGE_REPORT_ID,
	};
	int ret;

	ret = hid_get_feature_report(rift->dev.fd, &report, sizeof(report));
	if (ret < 0)
		return ret;

	if (report.gyro_range != 4)
		g_print("Rift: unexpected gyro range %u\n", report.gyro_range);
	if (report.accel_range != 2000)
		g_print("Rift: unexpected accel range %u\n", report.accel_range);
	if ((rift->type == RIFT_DK2 && report.mag_range != 4000) ||
	    (rift->type == RIFT_CV1 && report.mag_range != 1300))
		g_print("Rift: unexpected mag range %u\n", report.mag_range);

	return 0;
}

/*
 * Obtains the factory calibrated position data of IR LEDs and IMU
 * from the Rift. Values are stored with µm accuracy in the Rift's
 * local reference frame: the positive x axis points left, the y
 * axis points upward, and z forward:
 *
 *      up
 *       y z forward
 * left  |/
 *    x--+
 */
static int rift_get_positions(OuvrtRift *rift)
{
	struct rift_position_report report = {
		.id = RIFT_POSITION_REPORT_ID,
	};
	int fd = rift->dev.fd;
	uint8_t type;
	vec3 pos, dir;
	uint16_t index;
	uint16_t num;
	int ret;
	int i;

	ret = hid_get_feature_report(fd, &report, sizeof(report));
	if (ret < 0)
		return ret;

	num = __le16_to_cpu(report.num);
	if (num > MAX_POSITIONS)
		return -1;

	leds_init(&rift->leds, num - 1);

	for (i = 0; ; i++) {
		index = __le16_to_cpu(report.index);
		if (index >= num)
			return -1;

		type = __le16_to_cpu(report.type);

		/* Position in µm */
		pos.x = 1e-6f * (int32_t)__le32_to_cpu(report.pos[0]);
		pos.y = 1e-6f * (int32_t)__le32_to_cpu(report.pos[1]);
		pos.z = 1e-6f * (int32_t)__le32_to_cpu(report.pos[2]);

		if (type == RIFT_POSITION_LED) {
			rift->leds.model.points[index] = pos;

			/* Direction, magnitude in unknown units */
			dir.x = (int16_t)__le16_to_cpu(report.dir[0]);
			dir.y = (int16_t)__le16_to_cpu(report.dir[1]);
			dir.z = (int16_t)__le16_to_cpu(report.dir[2]);
			vec3_normalize(&dir);
			rift->leds.model.normals[index] = dir;
		} else if (type == RIFT_POSITION_IMU) {
			rift->imu_position = pos;
			/*
			 * Rotation direction and angle are stored as all zeros
			 * on DK2 and CV1, as IMU orientation can be determined
			 * from the IMU calibration report.
			 */
		}

		/* Break out before reading the first report again */
		if (i + 1 == num)
			break;

		ret = hid_get_feature_report(fd, &report, sizeof(report));
		if (ret < 0)
			return ret;
	}

	return 0;
}

/*
 * Obtains the blinking patterns of the IR LEDs from the Rift.
 */
static int rift_get_led_patterns(OuvrtRift *rift)
{
	struct rift_led_pattern_report report = {
		.id = RIFT_LED_PATTERN_REPORT_ID,
	};
	int fd = rift->dev.fd;
	uint8_t pattern_length;
	uint32_t pattern;
	uint16_t index;
	uint16_t num;
	int ret;
	int i;

	ret = hid_get_feature_report(fd, &report, sizeof(report));
	if (ret < 0)
		return ret;

	num = __le16_to_cpu(report.num);
	if (num > rift->leds.model.num_points)
		return -1;

	for (i = 0; ; i++) {
		index = __le16_to_cpu(report.index);
		if (index >= num)
			return -1;

		pattern_length = report.pattern_length;
		pattern = __le32_to_cpu(report.pattern);

		/* pattern_length should be 10 */
		if (pattern_length != 10) {
			g_print("Rift: Unexpected pattern length: %d\n",
				pattern_length);
			return -1;
		}

		/*
		 * pattern should consist of 10 2-bit values that are either
		 * 1 (dark) or 3 (bright).
		 */
		if ((pattern & ~0xaaaaa) != 0x55555) {
			g_print("Rift: Unexpected pattern: 0x%x\n",
				pattern);
			return -1;
		}

		/* Convert into 10 single-bit values 1 -> 0, 3 -> 1 */
		pattern &= 0xaaaaa;
		pattern |= pattern >> 1;
		pattern &= 0x66666;
		pattern |= pattern >> 2;
		pattern &= 0xe1e1e;
		pattern |= pattern >> 4;
		pattern &= 0xe01fe;
		pattern |= pattern >> 8;
		pattern = (pattern >> 1) & 0x3ff;

		rift->leds.patterns[index] = pattern;

		/* Break out before reading the first report again */
		if (i + 1 == num)
			break;

		ret = hid_get_feature_report(fd, &report, sizeof(report));
		if (ret < 0)
			return ret;
	}

	return 0;
}

/*
 * Reads the HMD UUID
 */
static int rift_get_uuid(OuvrtRift *rift)
{
	struct rift_uuid_report report = {
		.id = RIFT_UUID_REPORT_ID,
	};
	int fd = rift->dev.fd;
	int ret;

	ret = hid_get_feature_report(fd, &report, sizeof(report));
	if (ret < 0)
		return ret;

	memcpy(rift->uuid, report.uuid, 20);

	return 0;
}

/*
 * Sends a keepalive report to keep the device active for 10 seconds.
 */
static int rift_send_keepalive(OuvrtRift *rift)
{
	const struct rift_keepalive_report report = {
		.id = RIFT_KEEPALIVE_REPORT_ID,
		.type = RIFT_KEEPALIVE_TYPE,
		.timeout_ms = __cpu_to_le16(RIFT_KEEPALIVE_TIMEOUT_MS),
	};

	return hid_send_feature_report(rift->dev.fd, &report, sizeof(report));
}

/*
 * Sends a tracking report to enable the IR tracking LEDs.
 */
static int rift_send_tracking(OuvrtRift *rift, bool blink)
{
	struct rift_tracking_report report = {
		.id = RIFT_TRACKING_REPORT_ID,
		.vsync_offset = __cpu_to_le16(RIFT_TRACKING_VSYNC_OFFSET),
		.duty_cycle = RIFT_TRACKING_DUTY_CYCLE,
	};

	if (rift->type == RIFT_CV1) {
		report.exposure_us = __cpu_to_le16(RIFT_TRACKING_EXPOSURE_US_CV1);
		report.period_us = __cpu_to_le16(RIFT_TRACKING_PERIOD_US_CV1);
	} else {
		report.exposure_us = __cpu_to_le16(RIFT_TRACKING_EXPOSURE_US_DK2);
		report.period_us = __cpu_to_le16(RIFT_TRACKING_PERIOD_US_DK2);
	}

	if (blink) {
		report.pattern = 0;
		report.flags = RIFT_TRACKING_ENABLE |
			       RIFT_TRACKING_USE_CARRIER |
			       RIFT_TRACKING_AUTO_INCREMENT;
	} else {
		report.pattern = 0xff;
		report.flags = RIFT_TRACKING_ENABLE |
			       RIFT_TRACKING_USE_CARRIER;
	}

	return hid_send_feature_report(rift->dev.fd, &report, sizeof(report));
}

/*
 * Sends a display report to set up low persistence and pixel readback
 * for latency measurement.
 */
static int rift_send_display(OuvrtRift *rift, bool low_persistence,
			     bool pixel_readback)
{
	struct rift_display_report report = {
		.id = RIFT_DISPLAY_REPORT_ID,
	};
	uint16_t persistence;
	uint16_t total_rows;
	int ret;

	ret = hid_get_feature_report(rift->dev.fd, &report, sizeof(report));
	if (ret < 0)
		return ret;

	persistence = __le16_to_cpu(report.persistence);
	total_rows = __le16_to_cpu(report.total_rows);

	if (low_persistence) {
		report.brightness = 255;
		persistence = total_rows * 18 / 100;
	} else {
		report.brightness = 0;
		persistence = total_rows;
	}
	if (pixel_readback)
		report.flags2 |= RIFT_DISPLAY_READ_PIXEL;
	else
		report.flags2 &= ~RIFT_DISPLAY_READ_PIXEL;
	report.flags2 &= ~RIFT_DISPLAY_DIRECT_PENTILE;

	report.persistence = __cpu_to_le16(persistence);

	return hid_send_feature_report(rift->dev.fd, &report, sizeof(report));
}

/*
 * Powers up components of the Rift CV1.
 */
static int rift_cv1_power_up(OuvrtRift *rift, uint8_t components)
{
	struct rift_cv1_power_report report = {
		.id = RIFT_CV1_POWER_REPORT_ID,
	};
	int ret;

	ret = hid_get_feature_report(rift->dev.fd, &report, sizeof(report));
	if (ret < 0)
		return ret;

	report.components |= components;

	return hid_send_feature_report(rift->dev.fd, &report, sizeof(report));
}

/*
 * Powers down components of the Rift CV1.
 */
static int rift_cv1_power_down(OuvrtRift *rift, uint8_t components)
{
	struct rift_cv1_power_report report = {
		.id = RIFT_CV1_POWER_REPORT_ID,
	};
	int ret;

	ret = hid_get_feature_report(rift->dev.fd, &report, sizeof(report));
	if (ret < 0)
		return ret;

	report.components &= ~components;

	return hid_send_feature_report(rift->dev.fd, &report, sizeof(report));
}

/*
 * Decodes the periodic sensor message containing IMU sample(s) and
 * frame timing data.
 * Without calibration, the accelerometer reports acceleration in units
 * of 10⁻⁴ m/s² in the accelerometer reference frame: the positive x
 * axis points forward, the y axis points right, and z down.
 * The gyroscope reports angular velocity in units of 10⁻⁴ rad/s around
 * those axes. With onboard calibration enabled, the Rift's local frame
 * of reference is used instead:
 *
 *      x forward       up
 *     /                 y z forward
 *    +--y right   left  |/
 *    |               x--+
 *    z down
 */
static void rift_decode_sensor_message(OuvrtRift *rift,
				       const unsigned char *buf,
				       size_t len, struct timespec *ts)
{
	struct rift_sensor_message *message = (void *)buf;
	uint8_t num_samples;
	uint16_t sample_count;
	int16_t temperature;
	uint32_t sample_timestamp;
	int16_t mag[3];
	uint16_t frame_count;
	uint32_t frame_timestamp;
	uint8_t frame_id;
	uint8_t led_pattern_phase;
	uint16_t exposure_count;
	uint32_t exposure_timestamp;
	uint64_t message_time;
	struct imu_sample sample;
	int32_t dt;
	int i;

	if (len < sizeof(*message))
		return;

	message_time = ts->tv_sec * 1000000000 + ts->tv_nsec;

	num_samples = message->num_samples;
	sample_count = __le16_to_cpu(message->sample_count);
	/* 10⁻²°C */
	temperature = __le16_to_cpu(message->temperature);
	sample.temperature = 0.01f * temperature;

	sample_timestamp = __le32_to_cpu(message->timestamp);
	/* µs, wraps every ~72 min */
	sample.time = 1e-6 * sample_timestamp;

	dt = sample_timestamp - rift->last_sample_timestamp;
	/* µs, wraps every ~600k years */
	rift->last_sample_timestamp += dt;

	if ((dt < num_samples * rift->report_interval - 75) ||
	    (dt > num_samples * rift->report_interval + 75)) {
		rift->last_message_time = message_time;
		if (rift->last_sample_timestamp - dt == 0)
			return;
		if (dt < 0)
			g_print("Rift: got %u samples after %d µs\n",
				num_samples, dt);
		else if (dt + 1 >= (num_samples + 1) * rift->report_interval)
			g_print("Rift: got %u samples after %d µs, %u samples lost\n",
				num_samples, dt,
				(dt + 1) / rift->report_interval - num_samples);
		else
			g_print("Rift: got %u samples after %d µs, too much jitter\n",
				num_samples, dt);
		return;
	}

	mag[0] = __le16_to_cpu(message->mag[0]);
	mag[1] = __le16_to_cpu(message->mag[1]);
	mag[2] = __le16_to_cpu(message->mag[2]);
	sample.magnetic_field.x = 1e-4f * mag[0];
	sample.magnetic_field.y = 1e-4f * mag[1];
	sample.magnetic_field.z = 1e-4f * mag[2];

	frame_count = __le16_to_cpu(message->frame_count);
	frame_timestamp = __le32_to_cpu(message->frame_timestamp);
	frame_id = message->frame_id;
	led_pattern_phase = message->led_pattern_phase;
	exposure_count = __le16_to_cpu(message->exposure_count);
	exposure_timestamp = __le32_to_cpu(message->exposure_timestamp);

	num_samples = num_samples > 1 ? 2 : 1;
	for (i = 0; i < num_samples; i++) {
		/* 10⁻⁴ m/s² */
		unpack_3x21bit(1e-4f, message->sample[i].accel,
			       &sample.acceleration);
		/* 10⁻⁴ rad/s */
		unpack_3x21bit(1e-4f, message->sample[i].gyro,
			       &sample.angular_velocity);

		telemetry_send_imu_sample(rift->dev.id, &sample);

		pose_update(1e-6 / num_samples * dt, &rift->imu.pose, &sample);

		telemetry_send_pose(rift->dev.id, &rift->imu.pose);

		debug_imu_fifo_in(&rift->imu, 1);
	}

	if (exposure_count != rift->last_exposure_count) {
		int32_t sample_expo_dt = (int32_t)sample_timestamp -
					 exposure_timestamp;
		uint64_t exposure_time = message_time - (message_time -
					 rift->last_message_time) *
					 sample_expo_dt / dt;

		ouvrt_tracker_add_exposure(rift->tracker, exposure_timestamp,
					   exposure_time, led_pattern_phase);

		rift->last_exposure_timestamp = exposure_timestamp;
		rift->last_exposure_count = exposure_count;
	}

	rift->last_message_time = message_time;

	(void)frame_id;
	(void)frame_timestamp;
	(void)frame_count;
	(void)sample_count;
}

static int rift_get_boot_mode(OuvrtRift *rift)
{
	struct rift_bootload_report report = {
		.id = RIFT_BOOTLOAD_REPORT_ID,
	};
	int fd = rift->dev.fd;
	int ret;

	ret = hid_get_feature_report(fd, &report, sizeof(report));
	if (ret < 0)
		return ret;

	if (report.bootload != RIFT_BOOT_NORMAL &&
	    report.bootload != RIFT_BOOT_BOOTLOADER &&
	    report.bootload != RIFT_BOOT_RADIO_PAIRING) {
		g_print("%s: unexpected boot mode: 0x%02x\n", rift->dev.name,
			report.bootload);
		return -EINVAL;
	}

	return report.bootload;
}

static int rift_read_flash(OuvrtRift *rift, uint8_t index, unsigned char *buf)
{
	struct rift_cv1_read_flash_report report = {
		.id = RIFT_CV1_READ_FLASH_REPORT_ID,
		.index = index,
		.unknown = 0x80,
	};
	int ret;

	ret = rift_get_boot_mode(rift);
	if (ret < 0)
		return ret;
	if (ret != RIFT_BOOT_NORMAL)
		return ret;

	ret = hid_send_feature_report(rift->dev.fd, &report, sizeof(report));
	if (ret < 0) {
		g_print("%s: failed to set flash read address\n",
			rift->dev.name);
		return ret;
	}

	usleep(10000);

	ret = hid_get_feature_report(rift->dev.fd, &report, sizeof(report));
	if (ret < 0) {
		g_print("%s: failed to read from flash\n", rift->dev.name);
		return ret;
	}

	memcpy(buf, report.payload, sizeof(report.payload));

	return 0;
}

/*
 * Enables the IR tracking LEDs and registers them with the tracker.
 */
static int rift_start(OuvrtDevice *dev)
{
	OuvrtRift *rift = OUVRT_RIFT(dev);
	int ret;

	if (rift->type == RIFT_CV1) {
		ret = rift_get_boot_mode(rift);
		if (ret == RIFT_BOOT_RADIO_PAIRING)
			rift->radio.pairing = true;

		ret = rift_radio_get_address(dev->fds[0],
					     rift->radio.address);
		if (ret < 0)
			return ret;

		ouvrt_tracker_set_radio_address(rift->tracker,
						rift->radio.address);
	}

	ret = rift_get_uuid(rift);
	if (ret < 0) {
		g_print("Rift: Error reading UUID\n");
		return ret;
	}

	if (rift->type == RIFT_CV1) {
		ret = rift_get_boot_mode(rift);
		if (ret == RIFT_BOOT_NORMAL)
			rift_get_firmware_version(dev->fds[0]);
	}

	ret = rift_get_ranges(rift);
	if (ret < 0)
		return ret;

	ret = rift_get_imu_calibration(rift);
	if (ret < 0)
		return ret;

	ret = rift_get_positions(rift);
	if (ret < 0) {
		g_print("Rift: Error reading factory calibrated positions\n");
		return ret;
	}

	if (rift->type == RIFT_CV1) {
		unsigned char index[6] = { 0, 5, 3, 4, 36, 33 };
		unsigned char buf[64];
		int i;

		for (i = 0; i < 6; i++) {
			ret = rift_read_flash(rift, index[i], buf);
			if (ret < 0)
				return ret;
			/* TODO: figure out what to do with these */
		}
	}

	ret = rift_get_led_patterns(rift);
	if (ret < 0) {
		g_print("Rift: Error reading IR LED blinking patterns\n");
		return ret;
	}
	if ((rift->type == RIFT_DK2 && rift->leds.model.num_points != 40) ||
	    (rift->type == RIFT_CV1 && rift->leds.model.num_points != 44)) {
		g_print("Rift: Reported %d IR LEDs\n",
			rift->leds.model.num_points);
	}

	ret = rift_get_config(rift);
	if (ret < 0)
		return ret;

	ret = rift_set_report_rate(rift, 1000);
	if (ret < 0)
		return ret;

	ret = rift_send_tracking(rift, TRUE);
	if (ret < 0)
		return ret;

	ret = rift_send_display(rift, TRUE, TRUE);
	if (ret < 0)
		return ret;

	if (rift->type == RIFT_CV1) {
		ret = rift_cv1_power_up(rift, RIFT_CV1_POWER_DISPLAY |
					      RIFT_CV1_POWER_AUDIO |
					      RIFT_CV1_POWER_LEDS);
		if (ret < 0)
			return ret;
	}

	ouvrt_tracker_register_leds(rift->tracker, &rift->leds);

	return 0;
}

/*
 * Keeps the Rift active.
 */
static void rift_thread(OuvrtDevice *dev)
{
	OuvrtRift *rift = OUVRT_RIFT(dev);
	unsigned char buf[64];
	struct pollfd fds[2];
	struct timespec ts;
	int count;
	int ret;

	g_print("Rift: Sending keepalive\n");
	rift_send_keepalive(rift);
	count = 0;

	while (dev->active) {
		fds[0].fd = dev->fds[0];
		fds[0].events = POLLIN;
		fds[0].revents = 0;
		fds[1].fd = dev->fds[1];
		fds[1].events = POLLIN;
		fds[1].revents = 0;

		ret = poll(fds, 2, 1000);
		clock_gettime(CLOCK_MONOTONIC, &ts);
		if (ret == -1 || ret == 0 ||
		    count > 9 * rift->report_rate) {
			if (ret == -1 || ret == 0)
				g_print("Rift: Resending keepalive\n");
			rift_send_keepalive(rift);
			count = 0;
			continue;
		}

		if ((fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) ||
		    (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)))
			break;

		if (fds[0].revents & POLLIN) {
			ret = read(dev->fds[0], buf, sizeof(buf));
			if (ret == -1) {
				g_print("%s: Read error: %d\n", dev->name,
					errno);
				continue;
			}
			if (ret < 64) {
				g_print("%s: Error, invalid %d-byte report 0x%02x\n",
					dev->name, ret, buf[0]);
				continue;
			}

			rift_decode_sensor_message(rift, buf, sizeof(buf), &ts);
			count++;
		}
		if (fds[1].revents & POLLIN) {
			ret = read(dev->fds[1], buf, sizeof(buf));
			if (ret == -1) {
				g_print("%s: Read error: %d\n", dev->name,
					errno);
				continue;
			}

			if (ret != 64 ||
			    (buf[0] != RIFT_RADIO_REPORT_ID &&
			     buf[0] != RIFT_RADIO_UNKNOWN_MESSAGE_ID)) {
				g_print("%s: Error, invalid %d-byte report 0x%02x\n",
					dev->name, ret, buf[0]);
				continue;
			}

			rift_decode_radio_report(&rift->radio, dev->fds[1],
						 buf, sizeof(buf));

			struct rift_wireless_device *c;

			c = &rift->radio.remote.base;
			if (c->active && !c->dev_id)
				c->dev_id = ouvrt_device_claim_id(dev, c->serial);
			c = &rift->radio.touch[0].base;
			if (c->active && !c->dev_id)
				c->dev_id = ouvrt_device_claim_id(dev, c->serial);
			c = &rift->radio.touch[1].base;
			if (c->active && !c->dev_id)
				c->dev_id = ouvrt_device_claim_id(dev, c->serial);
		}
	}
}

/*
 * Disables the IR tracking LEDs and unregisters model from the
 * tracker.
 */
static void rift_stop(OuvrtDevice *dev)
{
	OuvrtRift *rift = OUVRT_RIFT(dev);
	struct rift_tracking_report report = {
		.id = RIFT_TRACKING_REPORT_ID,
	};
	int fd = rift->dev.fd;

	ouvrt_tracker_unregister_leds(rift->tracker, &rift->leds);
	g_clear_object(&rift->tracker);

	if (rift->type == RIFT_CV1) {
		rift_cv1_power_down(rift, RIFT_CV1_POWER_DISPLAY |
					  RIFT_CV1_POWER_AUDIO |
					  RIFT_CV1_POWER_LEDS);
	}

	hid_get_feature_report(fd, &report, sizeof(report));
	report.flags &= ~RIFT_TRACKING_ENABLE;
	hid_send_feature_report(fd, &report, sizeof(report));

	rift_set_report_rate(rift, 50);

	if (rift->type == RIFT_CV1 && rift->reboot) {
		struct rift_bootload_report report = {
			.id = RIFT_BOOTLOAD_REPORT_ID,
			.bootload = rift->boot_mode,
		};

		g_print("%s: Rebooting ...\n", dev->name);

		close(rift->dev.fds[1]);
		rift->dev.fds[1] = -1;
		hid_send_feature_report(fd, &report, sizeof(report));
		close(fd);
		rift->dev.fds[0] = -1;
	}
}

static void rift_radio_start_discovery(OuvrtDevice *dev)
{
	OuvrtRift *rift = OUVRT_RIFT(dev);
	int ret;

	ret = rift_get_boot_mode(rift);
	if (ret < 0)
		return;
	if (ret == RIFT_BOOT_RADIO_PAIRING) {
		g_print("Rift: Already in radio pairing mode\n");
		return;
	}

	g_print("Rift: Rebooting in radio pairing mode\n");
	rift->boot_mode = RIFT_BOOT_RADIO_PAIRING;
	rift->reboot = true;
	ouvrt_device_stop(dev);
}

static void rift_radio_stop_discovery(OuvrtDevice *dev)
{
	OuvrtRift *rift = OUVRT_RIFT(dev);
	int ret;

	ret = rift_get_boot_mode(rift);
	if (ret < 0)
		return;
	if (ret == RIFT_BOOT_NORMAL) {
		g_print("Rift: Already in normal mode\n");
		return;
	}

	g_print("Rift: Rebooting in normal mode\n");
	rift->boot_mode = RIFT_BOOT_NORMAL;
	rift->reboot = true;
	ouvrt_device_stop(dev);
}

/*
 * Frees the device structure and its contents.
 */
static void ouvrt_rift_finalize(GObject *object)
{
	OuvrtRift *rift = OUVRT_RIFT(object);

	g_clear_object(&rift->tracker);
	G_OBJECT_CLASS(ouvrt_rift_parent_class)->finalize(object);
}

static void ouvrt_rift_class_init(OuvrtRiftClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = ouvrt_rift_finalize;
	OUVRT_DEVICE_CLASS(klass)->start = rift_start;
	OUVRT_DEVICE_CLASS(klass)->thread = rift_thread;
	OUVRT_DEVICE_CLASS(klass)->stop = rift_stop;
	OUVRT_DEVICE_CLASS(klass)->radio_start_discovery = rift_radio_start_discovery;
	OUVRT_DEVICE_CLASS(klass)->radio_stop_discovery = rift_radio_stop_discovery;
}

static void ouvrt_rift_init(OuvrtRift *self)
{
	self->dev.type = DEVICE_TYPE_HMD;
	self->flicker = false;
	self->last_sample_timestamp = 0;
	rift_radio_init(&self->radio);
	self->imu.pose.rotation.w = 1.0;
}

/*
 * Allocates and initializes the device structure and opens the HID device
 * file descriptor.
 *
 * Returns the newly allocated Rift device.
 */
OuvrtDevice *rift_new(enum rift_type type)
{
	OuvrtRift *rift;

	rift = g_object_new(OUVRT_TYPE_RIFT, NULL);
	if (rift == NULL)
		return NULL;

	rift->dev.has_radio = type == RIFT_CV1;
	rift->tracker = ouvrt_tracker_new();
	rift->type = type;

	return &rift->dev;
}

OuvrtDevice *rift_dk2_new(G_GNUC_UNUSED const char *devnode)
{
	return rift_new(RIFT_DK2);
}

OuvrtDevice *rift_cv1_new(G_GNUC_UNUSED const char *devnode)
{
	return rift_new(RIFT_CV1);
}

void ouvrt_rift_set_flicker(OuvrtRift *rift, gboolean flicker)
{
	if (rift->flicker == flicker)
		return;

	rift->flicker = flicker;
	blobwatch_set_flicker(flicker);

	if (rift->dev.active)
		rift_send_tracking(rift, flicker);
}

OuvrtTracker *ouvrt_rift_get_tracker(OuvrtRift *rift)
{
	return rift->tracker;
}
