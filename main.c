#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>

#include "wlr-gamma-control-unstable-v1-client-protocol.h"
#include "color_math.h"
#include "str_vec.h"

#if defined(SPEEDRUN)
static time_t start = 0, offset = 0, multiplier = 1000;
static void init_time(void) {
	tzset();
	struct timespec realtime;
	clock_gettime(CLOCK_REALTIME, &realtime);
	offset = realtime.tv_sec;

	char *startstr = getenv("SPEEDRUN_START");
	if (startstr != NULL) {
		start = atol(startstr);
	} else {
		start = offset;
	}

	char *multistr = getenv("SPEEDRUN_MULTIPLIER");
	if (multistr != NULL) {
		multiplier = atol(multistr);
	}
}
static time_t get_time_sec(void) {
	struct timespec realtime;
	clock_gettime(CLOCK_REALTIME, &realtime);
	time_t now = start + ((realtime.tv_sec - offset) * multiplier +
			realtime.tv_nsec / (1000000000 / multiplier));
	struct tm tm;
	localtime_r(&now, &tm);
	fprintf(stderr, "time in termina: %02d:%02d:%02d, %d/%d/%d\n",
			tm.tm_hour, tm.tm_min, tm.tm_sec, tm.tm_mday,
			tm.tm_mon+1, tm.tm_year + 1900);
	return now;
}
static void adjust_timerspec(struct itimerspec *timerspec) {
	int diff = timerspec->it_value.tv_sec - offset;
	timerspec->it_value.tv_sec = offset + diff / multiplier;
	timerspec->it_value.tv_nsec = (diff % multiplier) * (1000000000 / multiplier);
}
#else
static inline void init_time(void) {
	tzset();
}
static inline time_t get_time_sec(void) {
	struct timespec realtime;
	clock_gettime(CLOCK_REALTIME, &realtime);
	return realtime.tv_sec;
}
static inline void adjust_timerspec(struct itimerspec *timerspec) {
	(void)timerspec;
}
#endif

static time_t get_timezone(void) {
	struct tm tm;
	time_t now = time(NULL);
	localtime_r(&now, &tm);
	return tm.tm_gmtoff;
}

static time_t round_day_offset(time_t now, time_t offset) {
	return now - ((now - offset) % 86400);
}

static time_t tomorrow(time_t now, time_t offset) {
	return round_day_offset(now, offset) + 86400;
}

static time_t longitude_time_offset(double longitude) {
	return -longitude * 43200 / M_PI;
}

static int max(int a, int b) {
	return a > b ? a : b;
}

struct config {
	int high_temp;
	int low_temp;
	double gamma;

	double longitude;
	double latitude;

	bool manual_time;
	time_t sunrise;
	time_t sunset;
	time_t duration;

	struct str_vec output_names;
};

enum state {
	STATE_INITIAL,
	STATE_NORMAL,
	STATE_TRANSITION,
	STATE_STATIC,
	STATE_FORCED,
};

enum force_state {
	FORCE_OFF,
	FORCE_HIGH,
	FORCE_LOW,
};

struct context {
	struct config config;
	struct sun sun;

	time_t longitude_time_offset;

	enum state state;
	enum sun_condition condition;

	time_t dawn_step_time;
	time_t dusk_step_time;
	time_t calc_day;

	bool new_output;
	struct wl_list outputs;
	timer_t timer;

	enum force_state forced_state;

	struct zwlr_gamma_control_manager_v1 *gamma_control_manager;
};

struct output {
	struct wl_list link;

	struct context *context;
	struct wl_output *wl_output;
	struct zwlr_gamma_control_v1 *gamma_control;

	int table_fd;
	uint32_t id;
	uint32_t ramp_size;
	uint16_t *table;
	bool enabled;
	char *name;
};

static void print_trajectory(struct context *ctx) {
	fprintf(stderr, "calculated sun trajectory: ");
	struct tm dawn, sunrise, sunset, dusk;
	switch (ctx->condition) {
	case NORMAL:
		localtime_r(&ctx->sun.dawn, &dawn);
		localtime_r(&ctx->sun.sunrise, &sunrise);
		localtime_r(&ctx->sun.sunset, &sunset);
		localtime_r(&ctx->sun.dusk, &dusk);
		fprintf(stderr,
			"dawn %02d:%02d, sunrise %02d:%02d, sunset %02d:%02d, dusk %02d:%02d\n",
			dawn.tm_hour, dawn.tm_min,
			sunrise.tm_hour, sunrise.tm_min,
			sunset.tm_hour, sunset.tm_min,
			dusk.tm_hour, dusk.tm_min);
		break;
	case MIDNIGHT_SUN:
		fprintf(stderr, "midnight sun\n");
		return;
	case POLAR_NIGHT:
		fprintf(stderr, "polar night\n");
		return;
	default:
		abort();
	}
}

static int anim_kelvin_step = 10;

static void recalc_stops(struct context *ctx, time_t now) {
	time_t day = round_day_offset(now, ctx->longitude_time_offset);
	if (day == ctx->calc_day) {
		return;
	}

	if (ctx->forced_state != FORCE_OFF) {
		ctx->state = STATE_FORCED;
		return;
	}

	time_t last_day = ctx->calc_day;
	ctx->calc_day = day;

	enum sun_condition cond = NORMAL;

	if (ctx->config.manual_time) {
		ctx->state = STATE_NORMAL;
		ctx->sun.dawn = ctx->config.sunrise - ctx->config.duration + day;
		ctx->sun.sunrise = ctx->config.sunrise + day;
		ctx->sun.sunset = ctx->config.sunset + day;
		ctx->sun.dusk = ctx->config.sunset + ctx->config.duration + day;

		goto done;
	}

	struct sun sun;
	struct tm tm = { 0 };
	gmtime_r(&day, &tm);
	cond = calc_sun(&tm, ctx->config.latitude, &sun);

	switch (cond) {
	case NORMAL:
		ctx->state = STATE_NORMAL;
		ctx->sun.dawn = sun.dawn + day;
		ctx->sun.sunrise = sun.sunrise + day;
		ctx->sun.sunset = sun.sunset + day;
		ctx->sun.dusk = sun.dusk + day;

		if (ctx->condition == MIDNIGHT_SUN) {
			// Yesterday had no sunset, so remove our sunrise.
			ctx->sun.dawn = day;
			ctx->sun.sunrise = day;
		}

		break;
	case MIDNIGHT_SUN:
		if (ctx->condition == POLAR_NIGHT) {
			fprintf(stderr, "warning: direct polar night to midnight sun transition\n");
		}

		if (ctx->state != STATE_NORMAL) {
			ctx->state = STATE_STATIC;
			break;
		}

		// Borrow yesterday's sunrise to animate into the midnight sun
		sun.dawn = ctx->sun.dawn - last_day + day;
		sun.sunrise = ctx->sun.sunrise - last_day + day;
		ctx->state = STATE_TRANSITION;
		break;
	case POLAR_NIGHT:
		if (ctx->condition == MIDNIGHT_SUN) {
			fprintf(stderr, "warning: direct midnight sun to polar night transition\n");
		}
		ctx->state = STATE_STATIC;
		break;
	default:
		abort();
	}

done:
	ctx->condition = cond;

	int temp_diff = ctx->config.high_temp - ctx->config.low_temp;
	ctx->dawn_step_time = max(1, (ctx->sun.sunrise - ctx->sun.dawn) *
		anim_kelvin_step / temp_diff);
	ctx->dusk_step_time = max(1, (ctx->sun.dusk - ctx->sun.sunset) *
		anim_kelvin_step / temp_diff);

	print_trajectory(ctx);
}

static int interpolate_temperature(time_t now, time_t start, time_t stop,
		int temp_start, int temp_stop) {
	if (start == stop) {
		return stop;
	}
	double time_pos = (double)(now - start) / (double)(stop - start);
	if (time_pos > 1.0) {
		time_pos = 1.0;
	} else if (time_pos < 0.0) {
		time_pos = 0.0;
	}
	int temp_pos = (double)(temp_stop - temp_start) * time_pos;
	return temp_start + temp_pos;
}

static int get_temperature_normal(const struct context *ctx, time_t now) {
	if (now < ctx->sun.dawn) {
		return ctx->config.low_temp;
	} else if (now < ctx->sun.sunrise) {
		return interpolate_temperature(now, ctx->sun.dawn,
				ctx->sun.sunrise, ctx->config.low_temp,
				ctx->config.high_temp);
	} else if (now < ctx->sun.sunset) {
		return ctx->config.high_temp;
	} else if (now < ctx->sun.dusk) {
		return interpolate_temperature(now, ctx->sun.sunset,
				ctx->sun.dusk, ctx->config.high_temp,
				ctx->config.low_temp);
	} else {
		return ctx->config.low_temp;
	}
}

static int get_temperature_transition(const struct context *ctx, time_t now) {
	switch (ctx->condition) {
	case MIDNIGHT_SUN:
		if (now < ctx->sun.sunrise) {
			return get_temperature_normal(ctx, now);
		}
		return ctx->config.high_temp;
	default:
		abort();
	}
}

static int get_temperature(const struct context *ctx, time_t now) {
	switch (ctx->state) {
	case STATE_NORMAL:
		return get_temperature_normal(ctx, now);
	case STATE_TRANSITION:
		return get_temperature_transition(ctx, now);
	case STATE_STATIC:
		return ctx->condition == MIDNIGHT_SUN ? ctx->config.high_temp :
			ctx->config.low_temp;
	case STATE_FORCED:
		switch (ctx->forced_state) {
		case FORCE_HIGH:
			return ctx->config.high_temp;
		case FORCE_LOW:
			return ctx->config.low_temp;
		default:
			abort();
		}
	default:
		abort();
	}
}

static time_t get_deadline_normal(const struct context *ctx, time_t now) {
	if (now < ctx->sun.dawn) {
		return ctx->sun.dawn;
	} else if (now < ctx->sun.sunrise) {
		return now + ctx->dawn_step_time;
	} else if (now < ctx->sun.sunset) {
		return ctx->sun.sunset;
	} else if (now < ctx->sun.dusk) {
		return now + ctx->dusk_step_time;
	} else {
		return tomorrow(now, ctx->longitude_time_offset);
	}
}

static time_t get_deadline_transition(const struct context *ctx, time_t now) {
	switch (ctx->condition) {
	case MIDNIGHT_SUN:
		if (now < ctx->sun.sunrise) {
			return get_deadline_normal(ctx, now);
		}
		// fallthrough
	case POLAR_NIGHT:
		return tomorrow(now, ctx->longitude_time_offset);
	default:
		abort();
	}
}

static void update_timer(const struct context *ctx, timer_t timer, time_t now) {
	time_t deadline;
	switch (ctx->state) {
	case STATE_NORMAL:
		deadline = get_deadline_normal(ctx, now);
		break;
	case STATE_TRANSITION:
		deadline = get_deadline_transition(ctx, now);
		break;
	case STATE_STATIC:
	case STATE_FORCED:
		deadline = tomorrow(now, ctx->longitude_time_offset);
		break;
	default:
		abort();
	}

	assert(deadline > now);
	struct itimerspec timerspec = {
		.it_interval = {0},
		.it_value = {
			.tv_sec = deadline,
			.tv_nsec = 0,
		}
	};
	adjust_timerspec(&timerspec);
	timer_settime(timer, TIMER_ABSTIME, &timerspec, NULL);
}

static int create_anonymous_file(off_t size) {
	char template[] = "/tmp/wlsunset-shared-XXXXXX";
	int fd = mkstemp(template);
	if (fd < 0) {
		return -1;
	}

	int ret;
	do {
		errno = 0;
		ret = ftruncate(fd, size);
	} while (errno == EINTR);
	if (ret < 0) {
		close(fd);
		return -1;
	}

	unlink(template);
	return fd;
}

static int create_gamma_table(uint32_t ramp_size, uint16_t **table) {
	size_t table_size = ramp_size * 3 * sizeof(uint16_t);
	int fd = create_anonymous_file(table_size);
	if (fd < 0) {
		fprintf(stderr, "failed to create anonymous file\n");
		return -1;
	}

	void *data =
		mmap(NULL, table_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		fprintf(stderr, "failed to mmap()\n");
		close(fd);
		return -1;
	}

	*table = data;
	return fd;
}

static void gamma_control_handle_gamma_size(void *data,
		struct zwlr_gamma_control_v1 *gamma_control, uint32_t ramp_size) {
	(void)gamma_control;
	struct output *output = data;
	output->ramp_size = ramp_size;
	if (output->table_fd != -1) {
		close(output->table_fd);
	}
	output->table_fd = create_gamma_table(ramp_size, &output->table);
	output->context->new_output = true;
	if (output->table_fd < 0) {
		fprintf(stderr, "could not create gamma table for output %s (%d)\n",
				output->name, output->id);
		exit(EXIT_FAILURE);
	}
}

static void gamma_control_handle_failed(void *data,
		struct zwlr_gamma_control_v1 *gamma_control) {
	(void)gamma_control;
	struct output *output = data;
	fprintf(stderr, "gamma control of output %s (%d) failed\n",
			output->name, output->id);
	zwlr_gamma_control_v1_destroy(output->gamma_control);
	output->gamma_control = NULL;
	if (output->table_fd != -1) {
		close(output->table_fd);
		output->table_fd = -1;
	}
}

static const struct zwlr_gamma_control_v1_listener gamma_control_listener = {
	.gamma_size = gamma_control_handle_gamma_size,
	.failed = gamma_control_handle_failed,
};

static void setup_gamma_control(struct context *ctx, struct output *output) {
	if (output->gamma_control != NULL) {
		return;
	}
	if (ctx->gamma_control_manager == NULL) {
		fprintf(stderr, "skipping setup of output %s (%d): gamma_control_manager missing\n",
				output->name, output->id);
		return;
	}
	output->gamma_control = zwlr_gamma_control_manager_v1_get_gamma_control(
		ctx->gamma_control_manager, output->wl_output);
	zwlr_gamma_control_v1_add_listener(output->gamma_control,
		&gamma_control_listener, output);
}

static void wl_output_handle_geometry(void *data, struct wl_output *output, int x, int y, int width,
				      int height, int subpixel, const char *make, const char *model,
				      int transform) {
	(void)data, (void)output, (void)x, (void)y, (void)width, (void)height, (void)subpixel,
		(void)make, (void)model, (void)transform;
}

static void wl_output_handle_mode(void *data, struct wl_output *output, uint32_t flags, int width,
				  int height, int refresh) {
	(void)data, (void)output, (void)flags, (void)width, (void)height, (void)refresh;
}

static void wl_output_handle_done(void *data, struct wl_output *wl_output) {
	(void)wl_output;
	struct output *output = data;
	setup_gamma_control(output->context, output);
}

static void wl_output_handle_scale(void *data, struct wl_output *output, int scale) {
	(void)data, (void)output, (void)scale;
}

static void wl_output_handle_name(void *data, struct wl_output *wl_output, const char *name) {
	(void)wl_output;
	struct output *output = data;
	output->name = strdup(name);
	struct config *cfg = &output->context->config;
	for (size_t idx = 0; idx < cfg->output_names.len; ++idx) {
		if (strcmp(output->name, cfg->output_names.data[idx]) == 0) {
			fprintf(stderr, "enabling output %s by name\n", output->name);
			output->enabled = true;
			return;
		}
	}
}

static void wl_output_handle_description(void *data, struct wl_output *wl_output, const char *description) {
	(void)wl_output;
	struct output *output = data;
	struct config *cfg = &output->context->config;
	for (size_t idx = 0; idx < cfg->output_names.len; ++idx) {
		if (strcmp(description, cfg->output_names.data[idx]) == 0) {
			fprintf(stderr, "enabling output %s by description\n", description);
			output->enabled = true;
			return;
		}
	}
}

struct wl_output_listener wl_output_listener = {
	.geometry = wl_output_handle_geometry,
	.mode = wl_output_handle_mode,
	.done = wl_output_handle_done,
	.scale = wl_output_handle_scale,
	.name = wl_output_handle_name,
	.description = wl_output_handle_description,
};

static void registry_handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	(void)version;
	struct context *ctx = (struct context *)data;
	if (strcmp(interface, wl_output_interface.name) == 0) {
		fprintf(stderr, "registry: adding output %d\n", name);

		struct output *output = calloc(1, sizeof(struct output));
		output->id = name;
		output->table_fd = -1;
		output->context = ctx;

		if (version >= WL_OUTPUT_NAME_SINCE_VERSION) {
			output->enabled = ctx->config.output_names.len == 0;
			output->wl_output = wl_registry_bind(registry, name,
					&wl_output_interface, WL_OUTPUT_NAME_SINCE_VERSION);
			wl_output_add_listener(output->wl_output, &wl_output_listener, output);
		} else {
			fprintf(stderr, "wl_output: old version (%d < %d), disabling name support\n",
					version, WL_OUTPUT_NAME_SINCE_VERSION);
			output->enabled = true;
			output->wl_output = wl_registry_bind(registry, name,
					&wl_output_interface, version);
			setup_gamma_control(ctx, output);
		}

		wl_list_insert(&ctx->outputs, &output->link);
	} else if (strcmp(interface,
				zwlr_gamma_control_manager_v1_interface.name) == 0) {
		ctx->gamma_control_manager = wl_registry_bind(registry, name,
				&zwlr_gamma_control_manager_v1_interface, 1);
	}
}

static void registry_handle_global_remove(void *data,
		struct wl_registry *registry, uint32_t name) {
	(void)registry;
	struct context *ctx = (struct context *)data;
	struct output *output, *tmp;
	wl_list_for_each_safe(output, tmp, &ctx->outputs, link) {
		if (output->id == name) {
			fprintf(stderr, "registry: removing output %s (%d)\n", output->name, name);
			free(output->name);
			wl_list_remove(&output->link);
			if (output->gamma_control != NULL) {
				zwlr_gamma_control_v1_destroy(output->gamma_control);
			}
			if (output->table_fd != -1) {
				close(output->table_fd);
			}
			free(output);
			break;
		}
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_handle_global,
	.global_remove = registry_handle_global_remove,
};

static void fill_gamma_table(uint16_t *table, uint32_t ramp_size, double rw,
		double gw, double bw, double gamma) {
	uint16_t *r = table;
	uint16_t *g = table + ramp_size;
	uint16_t *b = table + 2 * ramp_size;
	for (uint32_t i = 0; i < ramp_size; ++i) {
		double val = (double)i / (ramp_size - 1);
		r[i] = (uint16_t)(UINT16_MAX * pow(val * rw, 1.0 / gamma));
		g[i] = (uint16_t)(UINT16_MAX * pow(val * gw, 1.0 / gamma));
		b[i] = (uint16_t)(UINT16_MAX * pow(val * bw, 1.0 / gamma));
	}
}

static void output_set_whitepoint(struct output *output, struct rgb *wp, double gamma) {
	if (!output->enabled || output->gamma_control == NULL || output->table_fd == -1) {
		return;
	}
	fill_gamma_table(output->table, output->ramp_size, wp->r, wp->g, wp->b, gamma);
	lseek(output->table_fd, 0, SEEK_SET);
	zwlr_gamma_control_v1_set_gamma(output->gamma_control,
			output->table_fd);
}

static void set_temperature(struct wl_list *outputs, int temp, double gamma) {
	struct rgb wp = calc_whitepoint(temp);
	struct output *output;
	wl_list_for_each(output, outputs, link) {
		fprintf(stderr, "setting temperature on output %s (%d) to %d K\n",
				output->name, output->id, temp);
		output_set_whitepoint(output, &wp, gamma);
	}
}

static int timer_fired = 0;
static int usr1_fired = 0;
static int signal_fds[2];

static int display_dispatch(struct wl_display *display, int timeout) {
	if (wl_display_prepare_read(display) == -1) {
		return wl_display_dispatch_pending(display);
	}

	struct pollfd pfd[2];
	pfd[0].fd = wl_display_get_fd(display);
	pfd[1].fd = signal_fds[0];

	pfd[0].events = POLLOUT;
	// If we hit EPIPE we might have hit a protocol error. Continue reading
	// so that we can see what happened.
	while (wl_display_flush(display) == -1 && errno != EPIPE) {
		if (errno != EAGAIN) {
			wl_display_cancel_read(display);
			return -1;
		}

		// We only poll the wayland fd here
		while (poll(pfd, 1, timeout) == -1) {
			if (errno != EINTR) {
				wl_display_cancel_read(display);
				return -1;
			}
		}
	}

	pfd[0].events = POLLIN;
	pfd[1].events = POLLIN;
	while (poll(pfd, 2, timeout) == -1) {
		if (errno != EINTR) {
			wl_display_cancel_read(display);
			return -1;
		}
	}

	if (pfd[1].revents & POLLIN) {
		// Empty signal fd
		int signal;
		int res = read(signal_fds[0], &signal, sizeof signal);
		if (res == -1) {
			if (errno != EAGAIN) {
				return -1;
			}
		} else if (res != 4) {
			fprintf(stderr, "could not read full signal ID\n");
			return -1;
		}

		switch (signal) {
		case SIGALRM:
			timer_fired = true;
			break;
		case SIGUSR1:
			// do something
			usr1_fired = true;
			break;
		}
	}

	if ((pfd[0].revents & POLLIN) == 0) {
		wl_display_cancel_read(display);
		return 0;
	}

	if (wl_display_read_events(display) == -1) {
		return -1;
	}

	return wl_display_dispatch_pending(display);
}

static void signal_handler(int signal) {
	if (write(signal_fds[1], &signal, sizeof signal) == -1 && errno != EAGAIN) {
		// This is unfortunate.
	}
}

static int set_nonblock(int fd) {
	int flags;
	if ((flags = fcntl(fd, F_GETFL)) == -1 ||
			fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
		return -1;
	}
	return 0;
}

static int setup_signals(struct context *ctx) {
	struct sigaction signal_action = {
		.sa_handler = signal_handler,
		.sa_flags = 0,
	};
	if (pipe(signal_fds) == -1) {
		fprintf(stderr, "could not create signal pipe: %s\n",
				strerror(errno));
		return -1;
	}
	if (set_nonblock(signal_fds[0]) == -1 ||
			set_nonblock(signal_fds[1]) == -1) {
		fprintf(stderr, "could not set nonblock on signal pipe: %s\n",
				strerror(errno));
		return -1;
	}
	if (sigaction(SIGALRM, &signal_action, NULL) == -1) {
		fprintf(stderr, "could not configure SIGALRM handler: %s\n",
				strerror(errno));
		return -1;
	}
	if (sigaction(SIGUSR1, &signal_action, NULL) == -1) {
		fprintf(stderr, "could not configure SIGUSR1 handler: %s\n",
				strerror(errno));
		return -1;
	}
	if (timer_create(CLOCK_REALTIME, NULL, &ctx->timer) == -1) {
		fprintf(stderr, "could not configure timer: %s\n",
				strerror(errno));
		return -1;
	}
	return 0;
}

static int wlrun(struct config cfg) {
	// Initialize defaults
	struct context ctx = {
		.sun = { 0 },
		.condition = SUN_CONDITION_LAST,
		.state = STATE_INITIAL,
		.config = cfg,
	};

	if (!cfg.manual_time) {
		ctx.longitude_time_offset = longitude_time_offset(cfg.longitude);
	} else {
		ctx.longitude_time_offset = -get_timezone();
	}

	wl_list_init(&ctx.outputs);

	if (setup_signals(&ctx) == -1) {
		return EXIT_FAILURE;
	}

	struct wl_display *display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "failed to create display\n");
		return EXIT_FAILURE;
	}

	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, &ctx);
	wl_display_roundtrip(display);

	if (ctx.gamma_control_manager == NULL) {
		fprintf(stderr, "compositor doesn't support wlr-gamma-control-unstable-v1\n");
		return EXIT_FAILURE;
	}

	struct output *output;
	wl_list_for_each(output, &ctx.outputs, link) {
		setup_gamma_control(&ctx, output);
	}
	wl_display_roundtrip(display);

	time_t now = get_time_sec();
	recalc_stops(&ctx, now);
	update_timer(&ctx, ctx.timer, now);

	int temp = get_temperature(&ctx, now);
	set_temperature(&ctx.outputs, temp, ctx.config.gamma);

	int old_temp = temp;
	while (display_dispatch(display, -1) != -1) {
		if (ctx.new_output) {
			ctx.new_output = false;

			// Force set_temperature
			old_temp = 0;
			timer_fired = true;
		}

		if (usr1_fired) {
			usr1_fired = false;
			switch (ctx.forced_state) {
			case FORCE_OFF:
				ctx.forced_state = FORCE_HIGH;
				fprintf(stderr, "forcing high temperature\n");
				break;
			case FORCE_HIGH:
				ctx.forced_state = FORCE_LOW;
				fprintf(stderr, "forcing low temperature\n");
				break;
			case FORCE_LOW:
				ctx.forced_state = FORCE_OFF;
				fprintf(stderr, "disabling forced temperature\n");
				break;
			default:
				abort();
			}

			// Force re-calculation
			ctx.calc_day = 0;
			timer_fired = true;
		}

		if (timer_fired) {
			timer_fired = false;
			now = get_time_sec();
			recalc_stops(&ctx, now);
			update_timer(&ctx, ctx.timer, now);

			if ((temp = get_temperature(&ctx, now)) != old_temp) {
				old_temp = temp;
				ctx.new_output = false;
				set_temperature(&ctx.outputs, temp, ctx.config.gamma);
			}
		}
	}

	return EXIT_SUCCESS;
}

static int parse_time_of_day(const char *s, time_t *time) {
	struct tm tm = { 0 };

	if (strptime(s, "%H:%M", &tm) == NULL) {
		return -1;
	}
	*time = tm.tm_hour * 3600 + tm.tm_min * 60;
	return 0;
}

static const char usage[] = "usage: %s [options]\n"
"  -h             show this help message\n"
"  -v             show the version number\n"
"  -o <output>    name of output (display) to use,\n"
"                 by default all outputs are used\n"
"                 can be specified multiple times\n"
"  -t <temp>      set low temperature (default: 4000)\n"
"  -T <temp>      set high temperature (default: 6500)\n"
"  -l <lat>       set latitude (e.g. 39.9)\n"
"  -L <long>      set longitude (e.g. 116.3)\n"
"  -S <sunrise>   set manual sunrise (e.g. 06:30)\n"
"  -s <sunset>    set manual sunset (e.g. 18:30)\n"
"  -d <duration>  set manual duration in seconds (e.g. 1800)\n"
"  -g <gamma>     set gamma (default: 1.0)\n";

int main(int argc, char *argv[]) {
#ifdef SPEEDRUN
	fprintf(stderr, "warning: speedrun mode enabled\n");
#endif
	init_time();

	struct config config = {
		.latitude = NAN,
		.longitude = NAN,
		.high_temp = 6500,
		.low_temp = 4000,
		.gamma = 1.0,
	};
	str_vec_init(&config.output_names);

	int ret = EXIT_FAILURE;
	int opt;
	while ((opt = getopt(argc, argv, "hvo:t:T:l:L:S:s:d:g:")) != -1) {
		switch (opt) {
			case 'o':
				str_vec_push(&config.output_names, optarg);
				break;
			case 't':
				config.low_temp = strtol(optarg, NULL, 10);
				break;
			case 'T':
				config.high_temp = strtol(optarg, NULL, 10);
				break;
			case 'l':
				config.latitude = strtod(optarg, NULL);
				break;
			case 'L':
				config.longitude = strtod(optarg, NULL);
				break;
			case 'S':
				if (parse_time_of_day(optarg, &config.sunrise) != 0) {
					fprintf(stderr, "invalid time, expected HH:MM, got %s\n", optarg);
					goto end;
				}
				config.manual_time = true;
				break;
			case 's':
				if (parse_time_of_day(optarg, &config.sunset) != 0) {
					fprintf(stderr, "invalid time, expected HH:MM, got %s\n", optarg);
					goto end;
				}
				config.manual_time = true;
				break;
			case 'd':
				config.duration = strtol(optarg, NULL, 10);
				break;
			case 'g':
				config.gamma = strtod(optarg, NULL);
				break;
			case 'v':
				printf("wlsunset version %s\n", WLSUNSET_VERSION);
				ret = EXIT_SUCCESS;
				goto end;
			case 'h':
				ret = EXIT_SUCCESS;
			default:
				fprintf(stderr, usage, argv[0]);
				goto end;
		}
	}

	if (config.high_temp <= config.low_temp) {
		fprintf(stderr, "high temp (%d) must be higher than low (%d) temp\n",
				config.high_temp, config.low_temp);
		goto end;
	}
	if (config.manual_time) {
		if (!isnan(config.latitude) || !isnan(config.longitude)) {
			fprintf(stderr, "latitude and longitude are not valid in manual time mode\n");
			goto end;
		}
	} else {
		if (config.latitude > 90.0 || config.latitude < -90.0) {
			fprintf(stderr, "latitude (%lf) must be in interval [-90,90]\n",
					config.latitude);
			goto end;
		}
		config.latitude = RADIANS(config.latitude);
		if (config.longitude > 180.0 || config.longitude < -180.0) {
			fprintf(stderr, "longitude (%lf) must be in interval [-180,180]\n",
					config.longitude);
			goto end;
		}
		config.longitude = RADIANS(config.longitude);
	}
	ret = wlrun(config);
end:
	str_vec_free(&config.output_names);
	return ret;
}
