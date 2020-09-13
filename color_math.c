#include <math.h>
#include <errno.h>
#include "color_math.h"

static int illuminant_d(int temp, double *x, double *y) {
	// https://en.wikipedia.org/wiki/Standard_illuminant#Illuminant_series_D
	if (temp >= 4000 && temp <= 7000) {
		*x = 0.244063 + (0.09911e3/temp) + (2.9678e6/pow(temp, 2)) - (4.6070e9/pow(temp, 3));
	} else if (temp > 7000 && temp <= 25000) {
		*x = 0.237040 + (0.24748e3/temp) + (1.9018e6/pow(temp, 2)) - (2.0064e9/pow(temp, 3));
	} else {
		errno = EINVAL;
		return -1;
	}
	*y = (-3 * pow(*x, 2)) + (2.870 * (*x)) - 0.275;
	return 0;
}

static int planckian_locus(int temp, double *x, double *y) {
	if (temp >= 1667 && temp <= 4000) {
		*x = (-0.2661239e9/pow(temp, 3)) - (0.2343589e6/pow(temp, 2)) + (0.8776956e3/temp) + 0.179910;
		if (temp <= 2222) {
			*y = (-1.1064814 * pow(*x, 3)) - (1.34811020 * pow(*x, 2)) + (2.18555832 * (*x)) - 0.20219683;
		} else {
			*y = (-0.9549476 * pow(*x, 3)) - (1.37418593 * pow(*x, 2)) + (2.09137015 * (*x)) - 0.16748867;
		}
	} else if (temp > 4000 && temp < 25000) {
		*x = (-3.0258469e9/pow(temp, 3)) + (2.1070379e6/pow(temp, 2)) + (0.2226347e3/temp) + 0.240390;
		*y = (3.0817580 * pow(*x, 3)) - (5.87338670 * pow(*x, 2)) + (3.75112997 * (*x)) - 0.37001483;
	} else {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

static double srgb_gamma(double value, double gamma) {
	// https://en.wikipedia.org/wiki/SRGB
	if (value <= 0.0031308) {
		return 12.92 * value;
	} else {
		return pow(1.055 * value, 1.0/gamma) - 0.055;
	}
}

double clamp(double value) {
	if (value > 1.0) {
		return 1.0;
	} else if (value < 0.0) {
		return 0.0;
	} else {
		return value;
	}
}

static void xyz_to_srgb(double x, double y, double z, double *r, double *g, double *b) {
	// http://www.brucelindbloom.com/index.html?Eqn_RGB_XYZ_Matrix.html
	*r = srgb_gamma(clamp(3.2404542 * x - 1.5371385 * y - 0.4985314 * z), 2.2);
	*g = srgb_gamma(clamp(-0.9692660 * x + 1.8760108 * y + 0.0415560 * z), 2.2);
	*b = srgb_gamma(clamp(0.0556434 * x - 0.2040259 * y + 1.0572252 * z), 2.2);
}

static void srgb_normalize(double *r, double *g, double *b) {
	double maxw = fmaxl(*r, fmaxl(*g, *b));
	*r /= maxw;
	*g /= maxw;
	*b /= maxw;
}

void calc_whitepoint(int temp, double *rw, double *gw, double *bw) {
	if (temp == 6500) {
		*rw = *gw = *bw = 1.0;
		return;
	}

	double x = 1.0, y = 1.0;
	if (temp > 1667 && temp <= 6500) {
		planckian_locus(temp, &x, &y);
	} else if (temp >= 6500 && temp <= 25000) {
		illuminant_d(temp, &x, &y);
	}
	double z = 1.0 - x - y;

	xyz_to_srgb(x, y, z, rw, gw, bw);
	srgb_normalize(rw, gw, bw);
}

