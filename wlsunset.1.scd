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

*-S* <sunrise>
	Manual time for sunrise as HH:MM (e.g. 06:30).

*-s* <sunset>
	Manual time for sunset as HH:MM (e.g. 18:30).

*-d* <duration>
	Manual animation time in seconds (e.g. 1800).

	Only applicable when using manual sunset/sunrise times.

*-g* <gamma>
	Set gamma (default: 1.0).

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
