/* Host test of holes.c on a tree saved from the phone (mapple-link.py lk-dtb), mapped where LK left it:
 * cc -DSPACED -Wl,-pagezero_size,0x4000 holes_test.c holes.c fdt.c -o out/ht && out/ht lk-live.dtb [initrd-end] */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include "holes.h"

#define LK_DTB		0x4bc80000UL
#define INITRD_START	0x47c80000UL

int main(int argc, char **argv)
{
	int fd = open(argv[1], O_RDONLY);
	void *dtb = mmap((void *)LK_DTB, 0x100000, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0);
	uint64_t h[HOLES_MAX][2], rd_end = argc > 2 ? strtoull(argv[2], NULL, 0) : 0x48d40000;

	if (fd < 0 || dtb != (void *)LK_DTB) {
		perror(argv[1]);
		return 1;
	}
	int n = mem_holes(LK_DTB, INITRD_START, rd_end, 0x40000000, 0x13df00000, h);
	uint64_t total = 0;
	for (int i = 0; i < n; i++) {
		printf("%09llx-%09llx %8.2f MiB\n", h[i][0], h[i][1] - 1, (h[i][1] - h[i][0]) / 1048576.0);
		total += h[i][1] - h[i][0];
	}
	printf("%d holes, %.1f MiB; free %.1f MiB of the window\n", n, total / 1048576.0,
	    (0x13df00000 - 0x40000000 - total) / 1048576.0);
	return n > 0 ? 0 : 1;
}
