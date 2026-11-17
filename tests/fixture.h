#ifndef FIXTURE_H
#define FIXTURE_H

#include <stdint.h>
#include <stdio.h>

#include "test.h"

/* Opens FIXTURES_DIR/name; a missing file counts as a failure. */
static inline FILE *fixture_open(const char *name)
{
	char path[512];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", FIXTURES_DIR, name);
	f = fopen(path, "r");
	if (f == NULL) {
		printf("cannot open %s\n", path);
		test_failures++;
	}
	return f;
}

/* Reads hex digits from the fixture, skipping whitespace; returns the byte count. */
static inline size_t load_fixture(const char *name, uint8_t *out, size_t max)
{
	unsigned int byte;
	size_t n = 0;
	FILE *f = fixture_open(name);

	if (f == NULL) {
		return 0;
	}
	while (n < max && fscanf(f, "%2x", &byte) == 1) {
		out[n++] = (uint8_t)byte;
	}
	fclose(f);
	return n;
}

#endif
