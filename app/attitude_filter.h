#ifndef ATTITUDE_FILTER_H
#define ATTITUDE_FILTER_H

#include <stdbool.h>

struct attitude_filter {
	float roll_deg;
	float pitch_deg;
	float yaw_deg;
	float alpha;
	bool initialized;
};

void attitude_filter_init(struct attitude_filter *filter, float alpha);
void attitude_filter_update(struct attitude_filter *filter,
			    float ax_g, float ay_g, float az_g,
			    float gx_dps, float gy_dps, float gz_dps,
			    float dt_s);

#endif /* ATTITUDE_FILTER_H */
