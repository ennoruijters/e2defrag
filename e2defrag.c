#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include "e2defrag.h"

void usage(void)
{
	printf("Usage: e2defrag <disk>\n");
	exit(EINVAL);
}

int main(int argc, char *argv[])
{
	struct defrag_ctx *disk;
	char *filename;
	int ret;
	if (argc < 1)
		usage();
	filename = argv[1];
	disk = open_drive(filename, 0);
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
	dump_trees(disk);
#endif
	move_extent_interactive(disk);
	move_file_interactive(disk);
	close_drive(disk);
	return 0;
}
