# wlsunset

Day/night gamma adjustments for Wayland compositors supporting `wlr-gamma-control-unstable-v1` & `xdg-output-unstable-v1`.

# How to build and install

```
meson build
ninja -C build
sudo ninja -C build install
```

# How to use

See the helptext (`wlsunset -h`)

## Example

```
# Beijing lat/long.
wlsunset -l 39.9 -L 116.3
```

Greater precision than one decimal place [serves no purpose](https://xkcd.com/2170/) other than padding the command-line.

# Help

Go to #kennylevinsen @ irc.libera.chat to discuss, or use [~kennylevinsen/wlsunset-devel@lists.sr.ht](https://lists.sr.ht/~kennylevinsen/wlsunset-devel)
