#define _POSIX_C_SOURCE 200809L

#include "attitude_filter.h"
#include "imu_sample.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_IMU_DEVICE       "/dev/imu0"
#define DEFAULT_OLED_DEVICE      "/dev/oled0"
#define DEFAULT_CALIBRATION_SAMPLES 200U
#define DEFAULT_ALPHA            0.98f
#define DISPLAY_PERIOD_NS        100000000ULL
#define SAMPLE_BATCH_SIZE        32U
#define ACCEL_LSB_PER_G           16384.0f
#define GYRO_LSB_PER_DPS          131.0f
#define CALIBRATION_ACCEL_MIN_G   0.8
#define CALIBRATION_ACCEL_MAX_G   1.2
#define CALIBRATION_GYRO_MEAN_MAX_LSB 400.0
#define CALIBRATION_GYRO_STDDEV_MAX_LSB 100.0
#define POLL_TIMEOUT_WARNING_COUNT 5U

static volatile sig_atomic_t stop_requested;

struct app_options {
	const char *imu_path;
	const char *oled_path;
	unsigned int calibration_samples;
	float alpha;
	bool oled_enabled;
};

static void on_signal(int signo)
{
	(void)signo;
	stop_requested = 1;
}

static void usage(const char *program)
{
	fprintf(stderr,
		"Usage: %s [-i /dev/imu0] [-o /dev/oled0] [-n samples] "
		"[-a alpha] [--no-oled]\n",
		program);
}

static int parse_unsigned(const char *text, unsigned int *value)
{
	char *end = NULL;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0' || parsed > 100000UL)
		return -1;

	*value = (unsigned int)parsed;
	return 0;
}

static int parse_float(const char *text, float *value)
{
	char *end = NULL;
	float parsed;

	errno = 0;
	parsed = strtof(text, &end);
	if (errno != 0 || end == text || *end != '\0' || !isfinite(parsed))
		return -1;

	*value = parsed;
	return 0;
}

static int parse_options(int argc, char **argv, struct app_options *options)
{
	int i;

	options->imu_path = DEFAULT_IMU_DEVICE;
	options->oled_path = DEFAULT_OLED_DEVICE;
	options->calibration_samples = DEFAULT_CALIBRATION_SAMPLES;
	options->alpha = DEFAULT_ALPHA;
	options->oled_enabled = true;

	for (i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
			options->imu_path = argv[++i];
		} else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
			options->oled_path = argv[++i];
		} else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
			if (parse_unsigned(argv[++i], &options->calibration_samples) != 0)
				return -1;
		} else if (strcmp(argv[i], "-a") == 0 && i + 1 < argc) {
			if (parse_float(argv[++i], &options->alpha) != 0 ||
			    options->alpha < 0.0f || options->alpha > 1.0f)
				return -1;
		} else if (strcmp(argv[i], "--no-oled") == 0) {
			options->oled_enabled = false;
		} else {
			return -1;
		}
	}

	return 0;
}

static int open_oled(const struct app_options *options)
{
	int fd;

	if (!options->oled_enabled)
		return -1;

	fd = open(options->oled_path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		fprintf(stderr, "warning: cannot open %s: %s; terminal output only\n",
			options->oled_path, strerror(errno));

	return fd;
}

static void display_attitude(int oled_fd, const struct attitude_filter *filter)
{
	char text[96];
	int length;
	ssize_t written;

	length = snprintf(text, sizeof(text), "R:%7.2f\nP:%7.2f\nY:%7.2f\n",
			  filter->roll_deg, filter->pitch_deg, filter->yaw_deg);
	if (length < 0 || (size_t)length >= sizeof(text))
		return;

	printf("\rroll=%8.2f pitch=%8.2f yaw=%8.2f   ",
	       filter->roll_deg, filter->pitch_deg, filter->yaw_deg);
	fflush(stdout);

	if (oled_fd < 0)
		return;

	written = write(oled_fd, text, (size_t)length);
	if (written < 0)
		fprintf(stderr, "\nwarning: OLED write failed: %s\n", strerror(errno));
}

static void reset_calibration(double gyro_sum[3], double gyro_sum_sq[3],
			      double *accel_norm_sum,
			      unsigned int *sample_count)
{
	memset(gyro_sum, 0, 3U * sizeof(gyro_sum[0]));
	memset(gyro_sum_sq, 0, 3U * sizeof(gyro_sum_sq[0]));
	*accel_norm_sum = 0.0;
	*sample_count = 0U;
}

static bool calibration_is_stationary(const double gyro_sum[3],
				      const double gyro_sum_sq[3],
				      double accel_norm_sum,
				      unsigned int sample_count,
				      float gyro_bias[3])
{
	double accel_norm_mean;
	double gyro_mean[3];
	unsigned int axis;

	if (sample_count == 0U)
		return false;

	accel_norm_mean = accel_norm_sum / sample_count;
	if (accel_norm_mean < CALIBRATION_ACCEL_MIN_G ||
	    accel_norm_mean > CALIBRATION_ACCEL_MAX_G)
		return false;

	for (axis = 0U; axis < 3U; ++axis) {
		double variance;

		gyro_mean[axis] = gyro_sum[axis] / sample_count;
		if (fabs(gyro_mean[axis]) > CALIBRATION_GYRO_MEAN_MAX_LSB)
			return false;
		variance = gyro_sum_sq[axis] / sample_count -
			   gyro_mean[axis] * gyro_mean[axis];
		if (variance < 0.0)
			variance = 0.0;
		if (sqrt(variance) > CALIBRATION_GYRO_STDDEV_MAX_LSB)
			return false;
	}

	for (axis = 0U; axis < 3U; ++axis)
		gyro_bias[axis] = (float)gyro_mean[axis];

	return true;
}

int main(int argc, char **argv)
{
	struct app_options options;
	struct attitude_filter filter;
	struct pollfd pollfd;
	struct imu_sample samples[SAMPLE_BATCH_SIZE];
	double gyro_sum[3] = { 0.0, 0.0, 0.0 };
	double gyro_sum_sq[3] = { 0.0, 0.0, 0.0 };
	double accel_norm_sum = 0.0;
	float gyro_bias[3] = { 0.0f, 0.0f, 0.0f };
	unsigned int calibration_count = 0U;
	unsigned int consecutive_timeouts = 0U;
	uint64_t previous_timestamp = 0U;
	uint64_t last_display_timestamp = 0U;
	int imu_fd;
	int oled_fd;

	_Static_assert(sizeof(struct imu_sample) == IMU_SAMPLE_WIRE_SIZE,
		       "imu_sample ABI size mismatch");
	_Static_assert(offsetof(struct imu_sample, reserved0) == 12,
		       "imu_sample reserved0 offset mismatch");
	_Static_assert(offsetof(struct imu_sample, timestamp_ns) == 16,
		       "imu_sample timestamp offset mismatch");

	if (parse_options(argc, argv, &options) != 0) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	if (signal(SIGINT, on_signal) == SIG_ERR ||
	    signal(SIGTERM, on_signal) == SIG_ERR) {
		perror("signal");
		return EXIT_FAILURE;
	}

	imu_fd = open(options.imu_path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	if (imu_fd < 0) {
		fprintf(stderr, "cannot open %s: %s\n", options.imu_path,
			strerror(errno));
		return EXIT_FAILURE;
	}

	oled_fd = open_oled(&options);
	attitude_filter_init(&filter, options.alpha);
	pollfd.fd = imu_fd;
	pollfd.events = POLLIN;
	pollfd.revents = 0;

	if (options.calibration_samples > 0U)
		printf("Keep the board still: calibrating %u samples...\n",
		       options.calibration_samples);

	while (!stop_requested) {
		ssize_t bytes_read;
		size_t sample_count;
		size_t i;
		int poll_result;

		poll_result = poll(&pollfd, 1, 1000);
		if (poll_result < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			break;
		}
		if (poll_result == 0) {
			++consecutive_timeouts;
			if (consecutive_timeouts == POLL_TIMEOUT_WARNING_COUNT)
				fprintf(stderr,
					"warning: no IMU samples for %u seconds; check DRDY IRQ and device tree\n",
					POLL_TIMEOUT_WARNING_COUNT);
			continue;
		}
		if (consecutive_timeouts >= POLL_TIMEOUT_WARNING_COUNT)
			fprintf(stderr, "IMU sample stream recovered\n");
		consecutive_timeouts = 0U;
		if ((pollfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
			fprintf(stderr, "IMU device reported poll error: 0x%x\n",
				pollfd.revents);
			break;
		}
		if ((pollfd.revents & POLLIN) == 0)
			continue;

		bytes_read = read(imu_fd, samples, sizeof(samples));
		if (bytes_read < 0) {
			if (errno == EAGAIN || errno == EINTR)
				continue;
			fprintf(stderr, "IMU read failed: %s\n", strerror(errno));
			break;
		}
		if (bytes_read == 0)
			continue;
		if ((size_t)bytes_read % sizeof(samples[0]) != 0U) {
			fprintf(stderr, "IMU driver returned a partial sample\n");
			break;
		}

		sample_count = (size_t)bytes_read / sizeof(samples[0]);
		for (i = 0U; i < sample_count; ++i) {
			const struct imu_sample *sample = &samples[i];
			float dt_s;

			if (calibration_count < options.calibration_samples) {
				double accel_x_g = (double)sample->ax / ACCEL_LSB_PER_G;
				double accel_y_g = (double)sample->ay / ACCEL_LSB_PER_G;
				double accel_z_g = (double)sample->az / ACCEL_LSB_PER_G;
				double gyro_raw[3] = {
					sample->gx, sample->gy, sample->gz
				};
				unsigned int axis;

				for (axis = 0U; axis < 3U; ++axis) {
					gyro_sum[axis] += gyro_raw[axis];
					gyro_sum_sq[axis] += gyro_raw[axis] * gyro_raw[axis];
				}
				accel_norm_sum += sqrt(accel_x_g * accel_x_g +
							accel_y_g * accel_y_g +
							accel_z_g * accel_z_g);
				++calibration_count;
				if (calibration_count == options.calibration_samples) {
					if (calibration_is_stationary(gyro_sum, gyro_sum_sq,
								      accel_norm_sum,
								      calibration_count,
								      gyro_bias)) {
						printf("calibration complete: bias=(%.2f, %.2f, %.2f) LSB\n",
						       gyro_bias[0], gyro_bias[1], gyro_bias[2]);
					} else {
						fprintf(stderr,
							"calibration rejected: board moved or acceleration was not near 1 g; retrying\n");
						reset_calibration(gyro_sum, gyro_sum_sq,
								  &accel_norm_sum,
								  &calibration_count);
					}
				}
				previous_timestamp = sample->timestamp_ns;
				continue;
			}

			if (previous_timestamp == 0U ||
			    sample->timestamp_ns <= previous_timestamp)
				dt_s = 0.01f;
			else
				dt_s = (float)(sample->timestamp_ns - previous_timestamp) /
				       1000000000.0f;
			previous_timestamp = sample->timestamp_ns;

			attitude_filter_update(&filter,
				(float)sample->ax / ACCEL_LSB_PER_G,
				(float)sample->ay / ACCEL_LSB_PER_G,
				(float)sample->az / ACCEL_LSB_PER_G,
				((float)sample->gx - gyro_bias[0]) / GYRO_LSB_PER_DPS,
				((float)sample->gy - gyro_bias[1]) / GYRO_LSB_PER_DPS,
				((float)sample->gz - gyro_bias[2]) / GYRO_LSB_PER_DPS,
				dt_s);

			if (filter.initialized &&
			    (last_display_timestamp == 0U ||
			     sample->timestamp_ns - last_display_timestamp >=
			     DISPLAY_PERIOD_NS)) {
				display_attitude(oled_fd, &filter);
				last_display_timestamp = sample->timestamp_ns;
			}
		}
	}

	printf("\n");
	if (oled_fd >= 0)
		close(oled_fd);
	close(imu_fd);

	return stop_requested ? EXIT_SUCCESS : EXIT_FAILURE;
}
