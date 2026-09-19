/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * tarball conbiner
 */

#include <stdio.h>
#include <unistd.h>

#include <archive.h>
#include <archive_entry.h>

#include "config.h"

static void usage(const char *prog)
{
	error("usage: %s -i <config> -o <output> <tarball> [<tarball>...]\n", prog);
}

int main(int argc, char **argv)
{
	const char *output = NULL;
	const char *input = NULL;
	struct archive *out;
	int opt;

	while ((opt = getopt(argc, argv, "i:o:")) != -1) {
		switch (opt) {
		case 'i':
			input = optarg;
			break;
		case 'o':
			output = optarg;
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (!input || !output || optind == argc) {
		usage(argv[0]);
		return 1;
	}

	out = archive_write_new();
	archive_write_set_format_pax_restricted(out);
	archive_write_add_filter_none(out);

	archive_write_set_options(out, "xattrheader=SCHILY");

	if (archive_write_open_filename(out, output) != ARCHIVE_OK) {
		error("failed to open output: %s\n", archive_error_string(out));
		return 1;
	}

	if (archive_write_close(out) != ARCHIVE_OK) {
		error("failed to close output: %s\n", archive_error_string(out));
		return 1;
	}

	archive_write_free(out);

	return 0;
}
