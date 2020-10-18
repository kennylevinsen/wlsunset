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
	time_t now = start + ((realtime.tv_sec - offset) * multiplier + realtime.tv_nsec / (1000000000 / multiplier));
	struct tm tm;
	localtime_r(&now, &tm);
	fprintf(stderr, "time in termina: %02d:%02d:%02d, %d/%d/%d\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
			tm.tm_mday, tm.tm_mon+1, tm.tm_year + 1900);
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
	return time(NULL);
}
static inline void adjust_timerspec(struct itimerspec *timerspec) {
	(void)timerspec;
}
#endif

struct config {
	int high_temp;
	int low_temp;
	double gamma;

	double longitude;
	double latitude;

	int duration;
};

enum state {
	STATE_INITIAL,
	STATE_NORMAL,
	STATE_TRANSITION,
	STATE_STATIC,
};

struct context {
	struct config config;
	struct sun sun;

	enum state state;
	enum sun_condition condition;

	time_t dawn_step_time;
	time_t dusk_step_time;
	time_t calc_day;

	bool new_output;
	struct wl_list outputs;
	timer_t timer;
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
};

static time_t round_day(time_t now) {
	return now - (now % 86400);
}

static time_t tomorrow(time_t now) {
	return round_day(now) + 86400;
}

static struct zwlr_gamma_control_manager_v1 *gamma_control_manager = NULL;

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
		fprintf(stderr, "could not create gamma table for output %d\n",
				output->id);
		exit(EXIT_FAILURE);
	}
}

static void gamma_control_handle_failed(void *data,
		struct zwlr_gamma_control_v1 *gamma_control) {
	(void)gamma_control;
	struct output *output = data;
	fprintf(stderr, "gamma control of output %d failed\n",
			output->id);
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

static void setup_output(struct output *output) {
	if (output->gamma_control != NULL) {
		return;
	}
	if (gamma_control_manager == NULL) {
		fprintf(stderr, "skipping setup of output %d: gamma_control_manager missing\n",
				output->id);
		return;
	}
	output->gamma_control = zwlr_gamma_control_manager_v1_get_gamma_control(
		gamma_control_manager, output->wl_output);
	zwlr_gamma_control_v1_add_listener(output->gamma_control,
		&gamma_control_listener, output);
}

static void registry_handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	(void)version;
	struct context *ctx = (struct context *)data;
	if (strcmp(interface, wl_output_interface.name) == 0) {
		fprintf(stderr, "registry: adding output %d\n", name);
		struct output *output = calloc(1, sizeof(struct output));
		output->id = name;
		output->wl_output = wl_registry_bind(registry, name,
				&wl_output_interface, 1);
		output->table_fd = -1;
		output->context = ctx;
		wl_list_insert(&ctx->outputs, &output->link);
		setup_output(output);
	} else if (strcmp(interface,
				zwlr_gamma_control_manager_v1_interface.name) == 0) {
		gamma_control_manager = wl_registry_bind(registry, name,
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
			fprintf(stderr, "registry: removing output %d\n", name);
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

static void fill_gamma_table(uint16_t *table, uint32_t ramp_size, double rw, double gw, double bw, double gamma) {
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

static void set_temperature(struct wl_list *outputs, int temp, int gamma) {
	double rw, gw, bw;
	calc_whitepoint(temp, &rw, &gw, &bw);
	fprintf(stderr, "setting temperature to %d K\n", temp);

	struct output *output;
	wl_list_for_each(output, outputs, link) {
		if (output->gamma_control == NULL || output->table_fd == -1) {
			continue;
		}
		fill_gamma_table(output->table, output->ramp_size,
				rw, gw, bw, gamma);
		lseek(output->table_fd, 0, SEEK_SET);
		zwlr_gamma_control_v1_set_gamma(output->gamma_control,
				output->table_fd);
	}
}

static const char *sun_condition_str[] = {
	"normal",
	"midnight sun",
	"polar night",
	"invalid",
	NULL,
};

static void print_trajectory(struct context *ctx, time_t day) {
	fprintf(stderr, "calculated sun trajectory:");

	struct tm tm;
	if (ctx->sun.dawn >= day) {
		localtime_r(&ctx->sun.dawn, &tm);
		fprintf(stderr, " dawn %02d:%02d,", tm.tm_hour, tm.tm_min);
	} else {
		fprintf(stderr, " dawn N/A,");
	}

	if (ctx->sun.sunrise >= day) {
		localtime_r(&ctx->sun.sunrise, &tm);
		fprintf(stderr, " sunrise %02d:%02d,", tm.tm_hour, tm.tm_min);
	} else {
		fprintf(stderr, " sunrise N/A,");
	}

	if (ctx->sun.sunset >= day) {
		localtime_r(&ctx->sun.sunset, &tm);
		fprintf(stderr, " sunset %02d:%02d,", tm.tm_hour, tm.tm_min);
	} else {
		fprintf(stderr, " sunset N/A,");
	}

	if (ctx->sun.dusk >= day) {
		localtime_r(&ctx->sun.dusk, &tm);
		fprintf(stderr, " dusk %02d:%02d,", tm.tm_hour, tm.tm_min);
	} else {
		fprintf(stderr, " dusk N/A,");
	}

	fprintf(stderr, " condition: %s\n", sun_condition_str[ctx->condition]);
}

static int anim_kelvin_step = 25;

static void recalc_stops(struct context *ctx, time_t now) {
	time_t day = round_day(now);
	time_t last_day = ctx->calc_day;
	if (day == last_day) {
		return;
	}
	ctx->calc_day = day;

	struct sun sun;
	struct tm tm = { 0 };
	gmtime_r(&day, &tm);
	enum sun_condition cond = calc_sun(&tm, ctx->config.longitude, ctx->config.latitude, &sun);

	switch (cond) {
	case NORMAL:
		if (ctx->condition == MIDNIGHT_SUN) {
			// Yesterday had no sunset, so remove our sunrise.
			sun.dawn = -1;
			sun.sunrise = -1;
		}
		ctx->state = STATE_NORMAL;
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
		sun.dawn = sun.dawn != -1 ? sun.dawn :
			ctx->sun.dawn - last_day;
		sun.sunrise = sun.sunrise != -1 ? sun.sunrise :
			ctx->sun.sunrise - last_day;
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
	ctx->condition = cond;

	ctx->sun.dawn = sun.dawn + day;
	ctx->sun.sunrise = sun.sunrise + day;
	ctx->sun.sunset = sun.sunset + day;
	ctx->sun.dusk = sun.dusk + day;

	int temp_diff = ctx->config.high_temp - ctx->config.low_temp;
	ctx->dawn_step_time = (ctx->sun.sunrise - ctx->sun.dawn) * anim_kelvin_step / temp_diff;
	ctx->dusk_step_time = (ctx->sun.dusk - ctx->sun.sunset) * anim_kelvin_step / temp_diff;

	print_trajectory(ctx, day);
}

static int interpolate_temperature(time_t now, time_t start, time_t stop, int temp_start, int temp_stop) {
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
		return interpolate_temperature(now, ctx->sun.dawn, ctx->sun.sunrise, ctx->config.low_temp, ctx->config.high_temp);
	} else if (now < ctx->sun.sunset) {
		return ctx->config.high_temp;
	} else if (now < ctx->sun.dusk) {
		return interpolate_temperature(now, ctx->sun.sunset, ctx->sun.dusk, ctx->config.high_temp, ctx->config.low_temp);
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
		return ctx->condition == MIDNIGHT_SUN ? ctx->config.high_temp : ctx->config.low_temp;
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
		return tomorrow(now);
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
		return tomorrow(now);
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
		deadline = tomorrow(now);
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

static int timer_fired = 0;
static int timer_signal_fds[2];

static int display_dispatch(struct wl_display *display, int timeout) {
	if (wl_display_prepare_read(display) == -1) {
		return wl_display_dispatch_pending(display);
	}

	struct pollfd pfd[2];
	pfd[0].fd = wl_display_get_fd(display);
	pfd[1].fd = timer_signal_fds[0];

	pfd[0].events = POLLOUT;
	while (wl_display_flush(display) == -1) {
		if (errno != EAGAIN && errno != EPIPE) {
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
		char garbage[8];
		if (read(timer_signal_fds[0], &garbage, sizeof garbage) == -1 && errno != EAGAIN) {
			return -1;
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

static void timer_signal(int signal) {
	(void)signal;
	timer_fired = true;
	if (write(timer_signal_fds[1], "\0", 1) == -1 && errno != EAGAIN) {
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

static int setup_timer(struct context *ctx) {
	struct sigaction timer_action = {
		.sa_handler = timer_signal,
		.sa_flags = 0,
	};
	if (pipe(timer_signal_fds) == -1) {
		fprintf(stderr, "could not create signal pipe: %s\n", strerror(errno));
		return -1;
	}
	if (set_nonblock(timer_signal_fds[0]) == -1 ||
			set_nonblock(timer_signal_fds[1]) == -1) {
		fprintf(stderr, "could not set nonblock on signal pipe: %s\n", strerror(errno));
		return -1;
	}
	if (sigaction(SIGALRM, &timer_action, NULL) == -1) {
		fprintf(stderr, "could not configure alarm handler: %s\n", strerror(errno));
		return -1;
	}
	if (timer_create(CLOCK_REALTIME, NULL, &ctx->timer) == -1) {
		fprintf(stderr, "could not configure timer: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

static int wlrun(struct config cfg) {
	init_time();

	// Initialize defaults
	struct context ctx = {
		.sun = { 0 },
		.condition = SUN_CONDITION_LAST,
		.state = STATE_INITIAL,
		.config = cfg,
	};
	wl_list_init(&ctx.outputs);

	if (setup_timer(&ctx) == -1) {
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

	if (gamma_control_manager == NULL) {
		fprintf(stderr,
				"compositor doesn't support wlr-gamma-control-unstable-v1\n");
		return EXIT_FAILURE;
	}

	struct output *output;
	wl_list_for_each(output, &ctx.outputs, link) {
		setup_output(output);
	}
	wl_display_roundtrip(display);

	time_t now = get_time_sec();
	recalc_stops(&ctx, now);
	update_timer(&ctx, ctx.timer, now);

	int temp = get_temperature(&ctx, now);
	set_temperature(&ctx.outputs, temp, ctx.config.gamma);

	int old_temp = temp;
	while (display_dispatch(display, -1) != -1) {
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
		} else if (ctx.new_output) {
			ctx.new_output = false;
			set_temperature(&ctx.outputs, temp, ctx.config.gamma);
		}
	}

	return EXIT_SUCCESS;
}

static const char usage[] = "usage: %s [options]\n"
"  -h            show this help message\n"
"  -T <temp>     set high temperature (default: 6500)\n"
"  -t <temp>     set low temperature (default: 4000)\n"
"  -l <lat>      set latitude (e.g. 39.9)\n"
"  -L <long>     set longitude (e.g. 116.3)\n"
"  -s <start>    set manual start time (e.g. 06:30)\n"
"  -S <stop>     set manual stop time (e.g. 19:30)\n"
"  -d <minutes>  set manual ramping duration in minutes (default: 60)\n"
"  -g <gamma>    set gamma (default: 1.0)\n";

int main(int argc, char *argv[]) {
#ifdef SPEEDRUN
	fprintf(stderr, "warning: speedrun mode enabled\n");
#endif

	struct config config = {
		.latitude = 0,
		.longitude = 0,
		.high_temp = 6500,
		.low_temp = 4000,
		.gamma = 1.0,
		.duration = -1,
	};

	int opt;
	while ((opt = getopt(argc, argv, "ht:T:l:L:d:g:")) != -1) {
		switch (opt) {
			case 'T':
				config.high_temp = strtol(optarg, NULL, 10);
				break;
			case 't':
				config.low_temp = strtol(optarg, NULL, 10);
				break;
			case 'l':
				config.latitude = strtod(optarg, NULL);
				break;
			case 'L':
				config.longitude = strtod(optarg, NULL);
				break;
			case 'd':
				fprintf(stderr, "using animation duration override\n");
				config.duration = strtod(optarg, NULL) * 60;
				break;
			case 'g':
				config.gamma = strtod(optarg, NULL);
				break;
			case 'h':
			default:
				fprintf(stderr, usage, argv[0]);
				return opt == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
		}
	}

	if (config.high_temp <= config.low_temp) {
		fprintf(stderr, "high temp (%d) must be higher than low (%d) temp\n",
				config.high_temp, config.low_temp);
		return -1;
	}
	if (config.latitude > 90.0 || config.latitude < -90.0) {
		fprintf(stderr, "latitude (%lf) must be in interval [-90,90]\n", config.latitude);
		return EXIT_FAILURE;
	}
	config.latitude = RADIANS(config.latitude);
	if (config.longitude > 180.0 || config.longitude < -180.0) {
		fprintf(stderr, "longitude (%lf) must be in interval [-180,180]\n", config.longitude);
		return EXIT_FAILURE;
	}
	config.longitude = RADIANS(config.longitude);

	return wlrun(config);
}
