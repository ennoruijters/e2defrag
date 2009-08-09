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
	printf("Usage: e2defrag [-s|--simulate] [--] <disk>\n");
	exit(retval);
}

int parse_long_option(int argc, char **argv, int *idx)
{
	if (strcmp(argv[*idx], "--simulate") == 0)
		global_settings.simulate = 1;
	else if (strcmp(argv[*idx], "--interactive") == 0)
		global_settings.interactive = 1;
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
