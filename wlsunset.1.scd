wlsunset(1)

# NAME

wlsunet - Day/night gamma adjustments for Wayland compositors supporting
wlr-gamma-control-unstable-v1

# SYNOPSIS

*wlsunset* [options...]

# OPTIONS

*-h*
	show this help message

*-T* <temp>
	set high temperature (default: 6500)

*-t* <temp>
	set low temperature (default: 4000)

*-l* <lat>
	set latitude (e.g. 39.9)

*-L* <long>
	set longitude (e.g. 116.3)

*-g* <gamma>
	set gamma (default: 1.0)

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
