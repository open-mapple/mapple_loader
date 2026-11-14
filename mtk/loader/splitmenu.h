// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Pritish Joshi (DrtSinX98)
/* The dual boot menu's pure parts (SPLIT_MENU): its boot-args and key presses. No hardware, no libc: splitmenu_test.c
 * runs them on the Mac. The root's check is mtkroot.h's root_check. */
#ifndef SPLITMENU_H
#define SPLITMENU_H
#include <stdint.h>

#define MENU_SECS_DEFAULT   5u
#define MENU_SECS_MIN       3u          /* a record cannot take the menu away: Android stays reachable */
#define MENU_SECS_MAX       30u

/* NAME= at a word's start in a NUL-terminated line of at most max bytes: its value (up to a space) into v; 1 if found */
static int menu_arg(const volatile char *cl, uint32_t max, const char *name, char *v, uint32_t vmax)
{
	for (uint32_t at = 0; at < max && cl[at]; at++) {
		uint32_t k = 0, n = 0;

		if (at && cl[at - 1] != ' ') {
			continue;
		}
		while (name[k] && at + k < max && cl[at + k] == name[k]) {
			k++;
		}
		if (name[k]) {
			continue;
		}
		while (at + k + n < max && cl[at + k + n] && cl[at + k + n] != ' ' && n + 1 < vmax) {
			v[n] = cl[at + k + n];
			n++;
		}
		v[n] = 0;
		return 1;
	}
	return 0;
}

/* mp_menu_default=android|ios, mp_menu_secs=N and mp_menu_pmic=0 (volume up and Power not read) from line (0 when
 * there is none): into *xnu, *secs and *pmic if given */
static void menu_args(const volatile char *cl, uint32_t max, int *xnu, uint32_t *secs, int *pmic)
{
	char v[8];

	if (!cl) {
		return;
	}
	if (menu_arg(cl, max, "mp_menu_default=", v, sizeof(v))) {
		if (v[0] == 'i' && v[1] == 'o' && v[2] == 's' && !v[3]) {
			*xnu = 1;
		} else if (v[0] == 'a' && v[1] == 'n' && v[2] == 'd') {
			*xnu = 0;
		}
	}
	if (menu_arg(cl, max, "mp_menu_secs=", v, sizeof(v)) && v[0] >= '0' && v[0] <= '9') {
		uint32_t s = 0;

		for (int i = 0; v[i] >= '0' && v[i] <= '9' && s < 1000; i++) {
			s = s * 10 + (uint32_t)(v[i] - '0');
		}
		*secs = s < MENU_SECS_MIN ? MENU_SECS_MIN : s > MENU_SECS_MAX ? MENU_SECS_MAX : s;
	}
	if (menu_arg(cl, max, "mp_menu_pmic=", v, sizeof(v)) && (v[0] == '0' || v[0] == '1') && !v[1]) {
		*pmic = v[0] == '1';
	}
}

/* One key's presses: a press counts after 3 samples up, then 3 down (a key held since before the menu does not). */
struct menu_key {
	uint8_t up, down, armed;
};

static int menu_key_pressed(struct menu_key *k, int is_down)
{
	if (!is_down) {
		k->down = 0;
		if (k->up < 3 && ++k->up == 3) {
			k->armed = 1;
		}
		return 0;
	}
	k->up = 0;
	if (k->armed && ++k->down == 3) {
		k->armed = 0;
		return 1;
	}
	return 0;
}
#endif
