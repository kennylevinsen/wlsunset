#ifndef _COLOR_MATH_H
#define _COLOR_MATH_H

#include "math.h"
#include "time.h"

// These are macros so they can be applied to constants
#define DEGREES(rad) ((rad) * 180.0 / M_PI)
#define RADIANS(deg) ((deg) * M_PI / 180.0)

enum sun_condition {
	NORMAL,
	MIDNIGHT_SUN,
	POLAR_NIGHT,
	SUN_CONDITION_LAST
};

struct sun {
	time_t dawn;
	time_t sunrise;
	time_t sunset;
	time_t dusk;
};

struct rgb {
  double r, g, b;
};

struct xyz {
  double x, y, z;
};

enum sun_condition calc_sun(struct tm *tm, double latitude, struct sun *sun);
struct rgb calc_whitepoint(int temp);

#endif
