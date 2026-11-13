// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Pritish Joshi (DrtSinX98)
/* splitmenu.h on the Mac: cc -o /tmp/t splitmenu_test.c && /tmp/t */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "splitmenu.h"

int main(void)
{
	char line[64];
	int xnu;
	uint32_t secs;
	struct menu_key k = { 0 };
	int seq[] = { 1, 1, 1, 1, 0, 0, 0, 1, 1, 1, 1, 0, 1, 1 }, got = 0;

	xnu = 0, secs = 5;
	strcpy(line, "a=1 mp_menu_default=ios mp_menu_secs=12 b");
	menu_args(line, sizeof(line), &xnu, &secs);
	assert(xnu == 1 && secs == 12);
	strcpy(line, "mp_menu_default=android mp_menu_secs=0");
	menu_args(line, sizeof(line), &xnu, &secs);
	assert(xnu == 0 && secs == MENU_SECS_MIN);
	strcpy(line, "mp_menu_secs=999 xmp_menu_default=ios");
	menu_args(line, sizeof(line), &xnu, &secs);
	assert(xnu == 0 && secs == MENU_SECS_MAX);
	memcpy(line, "mp_menu_default=ios", 19);    /* no NUL inside max: stops at max */
	xnu = 0;
	menu_args(line, 19, &xnu, &secs);
	assert(xnu == 1);
	menu_args(0, 0, &xnu, &secs);

	/* held from before: no press; released 3, pressed 3: one press; a bounce of one sample: none */
	for (unsigned i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
		got += menu_key_pressed(&k, seq[i]);
	}
	assert(got == 1);

	printf("splitmenu: checks passed\n");
	return 0;
}
