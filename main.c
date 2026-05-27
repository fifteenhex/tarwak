#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* libarchive */
#include <archive.h>
#include <archive_entry.h>

static void usage(const char *prog)
{
	fprintf(stderr, "usage: %s -i <input> -o <output> -b <basedir>\n", prog);
}

static void free_archive(struct archive **a)
{
	if (*a)
		archive_write_free(*a);
}

int main(int argc, char **argv)
{
	struct archive __attribute__((cleanup(free_archive))) *tarball = NULL;
	const char *basedir = NULL;
	const char *output = NULL;
	const char *input = NULL;
	int opt;

	while ((opt = getopt(argc, argv, "i:o:b:")) != -1) {
		switch (opt) {
		case 'i':
			input = optarg;
			break;
		case 'o':
			output = optarg;
			break;
		case 'b':
			basedir = optarg;
			break;
		default:
			usage(argv[0]);
			break;
		}
	}

	if (!input || !output || !basedir) {
		usage(argv[0]);
		return 1;
	}

	tarball = archive_write_new();
	archive_write_set_format_pax_restricted(tarball);
	archive_write_add_filter_none(tarball);

	if (archive_write_open_filename(tarball, output) != ARCHIVE_OK) {
		fprintf(stderr, "failed to open output: %s\n", archive_error_string(tarball));
		return 1;
	}

	archive_write_close(tarball);

	return 0;
}
