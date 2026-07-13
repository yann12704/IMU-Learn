#include "attitude_filter.h"

#include <math.h>
#include <stddef.h>

#define RAD_TO_DEG (57.295779513082320876f)
#define DEG_TO_RAD (0.01745329251994329577f)
#define NOMINAL_DT_S (0.01f)
#define ACCEL_NORM_MIN_SQ (0.64f)
#define ACCEL_NORM_MAX_SQ (1.44f)
#define EULER_COS_MIN (0.10f)

static float normalize_degrees(float angle)
{
	while (angle > 180.0f)
		angle -= 360.0f;
	while (angle < -180.0f)
		angle += 360.0f;

	return angle;
}

static float blend_angle(float predicted, float measured, float gyro_weight)
{
	float shortest_error = normalize_degrees(measured - predicted);

	return normalize_degrees(predicted + (1.0f - gyro_weight) *
				 shortest_error);
}

void attitude_filter_init(struct attitude_filter *filter, float alpha)
{
	if (filter == NULL)
		return;

	if (alpha < 0.0f)
		alpha = 0.0f;
	else if (alpha > 1.0f)
		alpha = 1.0f;

	filter->roll_deg = 0.0f;
	filter->pitch_deg = 0.0f;
	filter->yaw_deg = 0.0f;
	filter->alpha = alpha;
	filter->initialized = false;
}

void attitude_filter_update(struct attitude_filter *filter,
			    float ax_g, float ay_g, float az_g,
			    float gx_dps, float gy_dps, float gz_dps,
			    float dt_s)
{
	float accel_norm_sq;
	float roll_acc;
	float pitch_acc;
	float roll_gyro;
	float pitch_gyro;
	float roll_rad;
	float pitch_rad;
	float sin_roll;
	float cos_roll;
	float sin_pitch;
	float cos_pitch;
	float safe_cos_pitch;
	float tan_pitch;
	float roll_rate_dps;
	float pitch_rate_dps;
	float yaw_rate_dps;
	float gyro_weight;
	bool accel_valid;

	if (filter == NULL || !isfinite(dt_s) || dt_s <= 0.0f)
		return;

	/* Do not integrate over long scheduling gaps or a reset timestamp. */
	if (dt_s > 0.2f)
		dt_s = NOMINAL_DT_S;

	accel_norm_sq = ax_g * ax_g + ay_g * ay_g + az_g * az_g;
	accel_valid = isfinite(accel_norm_sq) &&
		      accel_norm_sq >= ACCEL_NORM_MIN_SQ &&
		      accel_norm_sq <= ACCEL_NORM_MAX_SQ;
	roll_acc = atan2f(ay_g, az_g) * RAD_TO_DEG;
	pitch_acc = atan2f(-ax_g, sqrtf(ay_g * ay_g + az_g * az_g)) *
		    RAD_TO_DEG;

	if (!filter->initialized) {
		if (!accel_valid)
			return;
		filter->roll_deg = roll_acc;
		filter->pitch_deg = pitch_acc;
		filter->yaw_deg = 0.0f;
		filter->initialized = true;
		return;
	}

	/*
	 * Convert body gyro rates (p, q, r) to 3-2-1 Euler angle rates.  The
	 * representation is singular at pitch +/-90 degrees, so clamp the
	 * denominator to keep a bad sample from exploding the state.
	 */
	roll_rad = filter->roll_deg * DEG_TO_RAD;
	pitch_rad = filter->pitch_deg * DEG_TO_RAD;
	sin_roll = sinf(roll_rad);
	cos_roll = cosf(roll_rad);
	sin_pitch = sinf(pitch_rad);
	cos_pitch = cosf(pitch_rad);
	safe_cos_pitch = cos_pitch;
	if (fabsf(safe_cos_pitch) < EULER_COS_MIN)
		safe_cos_pitch = safe_cos_pitch < 0.0f ? -EULER_COS_MIN :
							 EULER_COS_MIN;
	tan_pitch = sin_pitch / safe_cos_pitch;

	roll_rate_dps = gx_dps + gy_dps * sin_roll * tan_pitch +
			gz_dps * cos_roll * tan_pitch;
	pitch_rate_dps = gy_dps * cos_roll - gz_dps * sin_roll;
	yaw_rate_dps = gy_dps * sin_roll / safe_cos_pitch +
		       gz_dps * cos_roll / safe_cos_pitch;

	roll_gyro = normalize_degrees(filter->roll_deg + roll_rate_dps * dt_s);
	pitch_gyro = normalize_degrees(filter->pitch_deg + pitch_rate_dps * dt_s);
	/* Preserve alpha's original meaning at 100 Hz while adapting to dt. */
	gyro_weight = powf(filter->alpha, dt_s / NOMINAL_DT_S);

	if (accel_valid) {
		filter->roll_deg = blend_angle(roll_gyro, roll_acc, gyro_weight);
		filter->pitch_deg = blend_angle(pitch_gyro, pitch_acc,
					      gyro_weight);
	} else {
		filter->roll_deg = roll_gyro;
		filter->pitch_deg = pitch_gyro;
	}

	/* ICM20602 has no magnetometer, so yaw is relative and will drift. */
	filter->yaw_deg = normalize_degrees(filter->yaw_deg +
					    yaw_rate_dps * dt_s);
}
