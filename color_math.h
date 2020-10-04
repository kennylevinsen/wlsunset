#ifndef _COLOR_MATH_H
#define _COLOR_MATH_H

void sun(struct tm *tm, double longitude, double latitude, time_t *dawn, time_t *sunrise, time_t *sunset, time_t *dusk);
double clamp(double value);
void calc_whitepoint(int temp, double *rw, double *gw, double *bw);

#endif
