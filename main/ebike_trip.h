#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Loads the two persistent local trip counters from NVS. */
esp_err_t ebike_trip_init(void);

/* Adds the delta of VESC absolute distance to both local counters. */
void ebike_trip_update_distance(float distance_abs_m, bool valid);

/* Thread-safe values in kilometres. */
void ebike_trip_get(float *local_1_km, float *local_2_km);

/* index is 0 for TRIP 1 and 1 for TRIP 2. */
void ebike_trip_reset(unsigned index);

#ifdef __cplusplus
}
#endif
