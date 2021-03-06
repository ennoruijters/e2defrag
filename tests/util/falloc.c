/*
    Copyright 2009 Enno Ruijters

    This program is free software; you can redistribute it and/or
    modify it under the terms of version 2 of the GNU General
    Public License as published by the Free Software Foundation.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#define __USE_GNU
#include <fcntl.h>
#include <unistd.h>
#include <linux/falloc.h>

#define DIV_ROUND_UP(x, y) (((x) + (y) - 1) / (y))

int fallocate(int fd, int mode, loff_t offset, loff_t len)
{
	return syscall(SYS_fallocate, fd, mode, offset, len);
}

int main(int argc, char *argv[])
{
	char *tmp;
	loff_t size;
	int fd, ret;
	if (argc < 3) {
		printf("Usage: falloc <file> <size>\n");
		return EXIT_FAILURE;
	}
	size = strtoll(argv[2], &tmp, 10);
	if (*tmp != 0 || size <= 0) {
		printf("%s is not a valid size.\n", argv[2]);
		return EXIT_FAILURE;
	}

	fd = open(argv[1], O_WRONLY);
	if (fd < 0) {
		printf("Could not open %s: %s\n", argv[1], strerror(errno));
		return EXIT_FAILURE;
	}

	ret = fallocate(fd, FALLOC_FL_KEEP_SIZE, 0, size);
	if (ret < 0) {
		printf("Could not fallocate %lu bytes: %s\n", size, strerror(errno));
		return EXIT_FAILURE;
	}

	ret = close(fd);
	if (ret < 0) {
		printf("Could not close file: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}

	return 0;
}
