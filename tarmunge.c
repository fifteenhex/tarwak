/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * tarball conbiner
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <archive.h>
#include <archive_entry.h>

#include "config.h"

#define READ_BLOCK (1024 * 1024)

struct munge {
	void *buff;
};

static void usage(const char *prog)
{
	error("usage: %s -i <config> -o <output> <tarball> [<tarball>...]\n", prog);
}

static struct archive *open_tarball(const char *path)
{
	struct archive *a;

	a = archive_read_new();
	archive_read_support_format_tar(a);
	archive_read_support_filter_all(a);

	if (archive_read_open_filename(a, path, READ_BLOCK) != ARCHIVE_OK) {
		error("failed to open %s: %s\n", path, archive_error_string(a));
		archive_read_free(a);
		return NULL;
	}

	return a;
}

static int __must_check copy_data(struct munge *munge, struct archive *in,
				  struct archive *out)
{
	la_ssize_t got;

	while ((got = archive_read_data(in, munge->buff, READ_BLOCK)) > 0) {
		if (archive_write_data(out, munge->buff, got) != got) {
			error("failed to write: %s\n", archive_error_string(out));
			return -1;
		}
	}

	if (got < 0) {
		error("failed to read: %s\n", archive_error_string(in));
		return -1;
	}

	return 0;
}

static int __must_check process_one(struct munge *munge, const char *path,
				    struct archive *out)
{
	struct archive *a;
	struct archive_entry *entry;
	int ret = 0;

	a = open_tarball(path);
	if (!a)
		return -1;

	while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
		if (archive_write_header(out, entry) != ARCHIVE_OK) {
			error("failed to write header for '%s': %s\n",
			      archive_entry_pathname(entry),
			      archive_error_string(out));
			ret = -1;
			break;
		}

		if (archive_entry_size(entry) > 0) {
			ret = copy_data(munge, a, out);
			if (ret)
				break;
		}
	}

	archive_read_free(a);

	return ret;
}

int main(int argc, char **argv)
{
	struct munge munge = { 0 };
	const char *output = NULL;
	const char *input = NULL;
	struct archive *out;
	unsigned int i;
	int ret;
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

	munge.buff = malloc(READ_BLOCK);
	if (!munge.buff) {
		error("no room for a copy buffer\n");
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

	/* Copy the contents of each tarball into the output */
	for (i = 0; optind + (int)i < argc; i++) {
		ret = process_one(&munge, argv[optind + i], out);
		if (ret)
			return 1;
	}

	if (archive_write_close(out) != ARCHIVE_OK) {
		error("failed to close output: %s\n", archive_error_string(out));
		return 1;
	}

	archive_write_free(out);

	return 0;
}
