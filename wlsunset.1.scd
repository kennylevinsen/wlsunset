wlsunset(1)

# NAME

wlsunset - day/night gamma adjustments for Wayland compositors supporting
wlr-gamma-control-unstable-v1

# SYNOPSIS

*wlsunset* [options...]

# OPTIONS

*-h*
	Show this help message.

*-T* <temp>
	Set high temperature (default: 6500).

*-t* <temp>
	Set low temperature (default: 4000).

*-l* <lat>
	Set latitude (e.g. 39.9).

*-L* <long>
	Set longitude (e.g. 116.3).

*-E* <daylight>
	Set solar elevation above the visible horizon that will be used for
	transitions between daylight and twilight.

*-e* <twilight>
	Set solar elevation above the visible horizon that will be used for
	transitions between twilight and night.

*-S* <sunrise>
	Manual time for sunrise as HH:MM (e.g. 06:30).

*-s* <sunset>
	Manual time for sunset as HH:MM (e.g. 18:30).

*-d* <duration>
	Manual animation time in seconds (e.g. 1800).

	Only applicable when using manual sunset/sunrise times.

*-g* <gamma>
	Set gamma (default: 1.0).

# SOLAR TRACKING

wlsunset uses the current day and specified location to calculate the time of
day:

1. The time where night transitions to twilight, as defined by the twilight
solar elevation, marking the start of the low-to-high color temperature
transition.
2. The time where twilight transitions to daylight, as defined by the daylight
solar elevation, marking the end of the low-to-high color temperature
transition.
3. The time where daylight transitions to twilight, as defined by the daylight
solar elevation, marking the start of the high-to-low color temperature
transition.
4. The time where twilight transitions to night, as defined by the twilight
solar elevation, marking the end of the high-to-low color temperature
transition.

The calculation uses a number of mathematical approximations and rely on a
correctly set latitude, longitude and system clock.

The point of transition can be controlled by changing the configured solar
elevation thresholds with *-E* and *-e*. It is also possible to use a manually
set time of transition using `*S*, *-s* and *-d*.

# RUNTIME CONTROL

Sending SIGUSR1 to wlsunset causes it to cycle through the following modes:

1. Forced use of the high temperature.

2. Forced use of the low temperature.

3. Automatic temperature calculation, the default behavior.

# EXAMPLE

```
# Beijing lat/long.
wlsunset -l 39.9 -L 116.3
```

Greater precision than one decimal place serves no purpose
(https://xkcd.com/2170/) other than padding the command-line.

# AUTHORS

Maintained by Kenny Levinsen <contact@kl.wtf>. For more information about
wlsunset development, see https://sr.ht/~kennylevinsen/wlsunset.
