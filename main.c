#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include "wlr-gamma-control-unstable-v1-client-protocol.h"

struct output {
	struct wl_output *wl_output;
	struct zwlr_gamma_control_v1 *gamma_control;
	uint32_t ramp_size;
	int table_fd;
	uint16_t *table;
	struct wl_list link;
};

static struct wl_list outputs;
static struct zwlr_gamma_control_manager_v1 *gamma_control_manager = NULL;

static int create_anonymous_file(off_t size) {
	char template[] = "/tmp/wlroots-shared-XXXXXX";
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
	struct output *output = data;
	output->ramp_size = ramp_size;
	output->table_fd = create_gamma_table(ramp_size, &output->table);
	if (output->table_fd < 0) {
		exit(EXIT_FAILURE);
	}
}

static void gamma_control_handle_failed(void *data,
		struct zwlr_gamma_control_v1 *gamma_control) {
	fprintf(stderr, "failed to set gamma table\n");
	exit(EXIT_FAILURE);
}

static const struct zwlr_gamma_control_v1_listener gamma_control_listener = {
	.gamma_size = gamma_control_handle_gamma_size,
	.failed = gamma_control_handle_failed,
};

static void registry_handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	if (strcmp(interface, wl_output_interface.name) == 0) {
		struct output *output = calloc(1, sizeof(struct output));
		output->wl_output = wl_registry_bind(registry, name,
				&wl_output_interface, 1);
		wl_list_insert(&outputs, &output->link);
	} else if (strcmp(interface,
				zwlr_gamma_control_manager_v1_interface.name) == 0) {
		gamma_control_manager = wl_registry_bind(registry, name,
				&zwlr_gamma_control_manager_v1_interface, 1);
	}
}

static void registry_handle_global_remove(void *data,
		struct wl_registry *registry, uint32_t name) {
	// Who cares?
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

static double clamp(double value) {
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

static void calc_whitepoint(int temp, double *rw, double *gw, double *bw) {
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

	double maxw = fmaxl(*rw, fmaxl(*gw, *bw));
	*rw /= maxw;
	*gw /= maxw;
	*bw /= maxw;
}

static const char usage[] = "usage: %s [options]\n"
"  -h          show this help message\n"
"  -t <value>  set temperature (default: 6500)\n"
"  -g <value>  set gamma (default: 1)\n";

int main(int argc, char *argv[]) {
	wl_list_init(&outputs);

	double gamma = 1;
	long temperature = 6500;
	int opt;
	while ((opt = getopt(argc, argv, "ht:g:")) != -1) {
		switch (opt) {
			case 't':
				temperature = strtol(optarg, NULL, 10);
				break;
			case 'g':
				gamma = strtod(optarg, NULL);
				break;
			case 'h':
			default:
				fprintf(stderr, usage, argv[0]);
				return opt == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
		}
	}

	struct wl_display *display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "failed to create display\n");
		return -1;
	}

	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);

	if (gamma_control_manager == NULL) {
		fprintf(stderr,
				"compositor doesn't support wlr-gamma-control-unstable-v1\n");
		return EXIT_FAILURE;
	}

	struct output *output;
	wl_list_for_each(output, &outputs, link) {
		output->gamma_control = zwlr_gamma_control_manager_v1_get_gamma_control(
				gamma_control_manager, output->wl_output);
		zwlr_gamma_control_v1_add_listener(output->gamma_control,
				&gamma_control_listener, output);
	}
	wl_display_roundtrip(display);

	/* Approximate white point */
	double rw, gw, bw;
	calc_whitepoint(temperature, &rw, &gw, &bw);

	wl_list_for_each(output, &outputs, link) {
		fill_gamma_table(output->table, output->ramp_size,
				rw, gw, bw, gamma);
		zwlr_gamma_control_v1_set_gamma(output->gamma_control,
				output->table_fd);
	}

	while (wl_display_dispatch(display) != -1) {
		// This space is intentionnally left blank
	}

	return EXIT_SUCCESS;
}
