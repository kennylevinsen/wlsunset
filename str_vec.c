#define _XOPEN_SOURCE 700
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "str_vec.h"

void str_vec_init(struct str_vec *vec) {
	vec->data = NULL;
	vec->len = 0;
}

void str_vec_push(struct str_vec *vec, const char *new_str) {
	++vec->len;
	vec->data = realloc(vec->data, vec->len * sizeof(char*));
	if (!vec->data) {
		perror("str_vec_push failed to allocate memory for str_vec");
		exit(EXIT_FAILURE);
	}
	vec->data[vec->len - 1] = strdup(new_str);
}

void str_vec_free(struct str_vec *vec) {
	if (vec == NULL) {
		return;
	}
	for (size_t i = 0; i < vec->len; ++i) {
		if (vec->data[i] != NULL) {
			free(vec->data[i]);
		}
	}
	free(vec->data);
	vec->data = NULL;
	vec->len = 0;
}
