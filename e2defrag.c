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
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include "e2defrag.h"

struct settings global_settings = {
	.simulate = 0,
	.interactive = 0,
};

void usage(int retval)
{
	printf("Usage: e2defrag [-s|--simulate] [-i|--interactive] [-d|--no-data-move] [--] <disk>\n");
	exit(retval);
}

int parse_long_option(int argc, char **argv, int *idx)
{
	if (strcmp(argv[*idx], "--simulate") == 0)
		global_settings.simulate = 1;
	else if (strcmp(argv[*idx], "--interactive") == 0)
		global_settings.interactive = 1;
	else if (strcmp(argv[*idx], "--no-data-move") == 0)
		global_settings.no_data_move = 1;
	else
		return EXIT_FAILURE;
	return 0;
}

int parse_options(int argc, char *argv[], char **filename)
{
	int i;
	if (argc < 2)
		return EXIT_FAILURE;
	*filename = NULL;
	for (i = 1; i < argc; i++) {
		switch (argv[i][0]) {
		case '-':
			switch (argv[i][1]) {
			case 's':
				global_settings.simulate = 1;
				break;
			case 'i':
				global_settings.interactive = 1;
				break;
			case 'd':
				global_settings.no_data_move = 1;
				break;
			case '-':
				if (argv[i][2] != '0') {
					int ret;
					ret = parse_long_option(argc, argv, &i);
					if (ret)
						return ret;
				} else {
					if (argc != i + 2) {
						return EXIT_FAILURE;
					} else {
						if (*filename == NULL)
							*filename = argv[++i];
						else
							return EXIT_FAILURE;
					}
				}
				break;
			default:
				return EXIT_FAILURE;
			}
			break;
		default:
			if (*filename == NULL)
				*filename = argv[i];
			else
				return EXIT_FAILURE;
		}
	}
	if (*filename == NULL)
		return EXIT_FAILURE;
	return 0;
}

int main(int argc, char *argv[])
{
	struct defrag_ctx *disk;
	char *filename;
	int ret;

	ret = parse_options(argc, argv, &filename);
	if (ret)
		usage(ret);
	disk = open_drive(filename);
	if (!disk) {
		printf("Error opening drive: %s\n", strerror(errno));
		return errno;
	}
	ret = set_e2_filesystem_data(disk);
	if (ret < 0) {
		printf("%d\n", ret);
		printf("Error reading filesystem: %s\n", strerror(errno));
		return errno;
	}
#ifndef NDEBUG
	dump_trees(disk, 3);
#endif
	if (global_settings.interactive) {
		ret = 0;
		while (!ret)
			ret = defrag_file_interactive(disk);
	} else {
		ret = do_whole_disk(disk);
	}
	close_drive(disk);
	return 0;
}
