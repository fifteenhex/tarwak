/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * tarball conbiner
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <archive.h>
#include <archive_entry.h>
#include <cjson/cJSON.h>

#include "config.h"

#define READ_BLOCK (1024 * 1024)

#define PATH_MAXLEN 4096

/* Avoid issues with different strings for the same effective path */
static const char *normalise(const char *path, char *buf, size_t len)
{
	size_t end;

	while (path[0] == '.' && path[1] == '/')
		path += 2;

	while (path[0] == '/')
		path++;

	if ((size_t)snprintf(buf, len, "/%s", path) >= len) {
		error("path too long: '%s'\n", path);
		return NULL;
	}

	end = strlen(buf);
	while (end > 1 && buf[end - 1] == '/')
		buf[--end] = '\0';

	return buf;
}

struct unknown {
	const char *what;
	char *name;
};

struct munge {
	struct user_map *usermap;
	int numusers;
	struct group_map *groupmap;
	int numgroups;

	const char *default_user;
	const char *default_group;
	uid_t default_uid;
	gid_t default_gid;

	struct unknown *unknown;
	unsigned int numunknown;
	unsigned int maxunknown;

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

static void warn_once(struct munge *munge, const char *what, const char *name,
		      const char *instead)
{
	struct unknown *grown;
	unsigned int i;

	for (i = 0; i < munge->numunknown; i++)
		if (strcmp(munge->unknown[i].what, what) == 0 &&
		    strcmp(munge->unknown[i].name, name) == 0)
			return;

	if (munge->numunknown == munge->maxunknown) {
		unsigned int want = munge->maxunknown ? munge->maxunknown * 2 : 16;

		grown = realloc(munge->unknown, want * sizeof(*grown));
		if (!grown)
			return;

		munge->unknown = grown;
		munge->maxunknown = want;
	}

	grown = &munge->unknown[munge->numunknown];

	grown->name = strdup(name);
	if (!grown->name)
		return;

	grown->what = what;
	munge->numunknown++;

	error("tarmunge: no %s '%s' in the config, using '%s'\n", what, name,
	      instead);
}

/* Apply the config's ownership settings */
static void set_owner(struct munge *munge, struct archive_entry *entry)
{
	const char *uname = archive_entry_uname(entry);
	const char *gname = archive_entry_gname(entry);
	uid_t uid;
	gid_t gid;

	if (!uname || !*uname ||
	    lookup_uid(munge->usermap, munge->numusers, uname, &uid)) {
		if (uname && *uname)
			warn_once(munge, "user", uname, munge->default_user);

		uname = munge->default_user;
		uid = munge->default_uid;
	}

	if (!gname || !*gname ||
	    lookup_gid(munge->groupmap, munge->numgroups, gname, &gid)) {
		if (gname && *gname)
			warn_once(munge, "group", gname, munge->default_group);

		gname = munge->default_group;
		gid = munge->default_gid;
	}

	archive_entry_set_uid(entry, uid);
	archive_entry_set_uname(entry, uname);
	archive_entry_set_gid(entry, gid);
	archive_entry_set_gname(entry, gname);
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
		char buf[PATH_MAXLEN];
		const char *name;

		name = normalise(archive_entry_pathname(entry), buf, sizeof(buf));
		if (!name) {
			ret = -1;
			break;
		}

		archive_entry_set_pathname(entry, name);
		set_owner(munge, entry);

		if (archive_write_header(out, entry) != ARCHIVE_OK) {
			error("failed to write header for '%s': %s\n", name,
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
	cJSON *config = NULL;
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

	ret = parse_config(input, &config);
	if (ret)
		return 1;

	ret = parse_users(config, &munge.usermap, &munge.numusers);
	if (ret)
		return 1;

	ret = parse_groups(config, &munge.groupmap, &munge.numgroups);
	if (ret)
		return 1;

	munge.default_user = "root";
	munge.default_group = "root";

	ret = parse_defaults(config, &munge.default_user, &munge.default_group);
	if (ret)
		return 1;

	if (lookup_uid(munge.usermap, munge.numusers, munge.default_user,
		       &munge.default_uid)) {
		error("no user '%s' in the config to default to\n",
		      munge.default_user);
		return 1;
	}

	if (lookup_gid(munge.groupmap, munge.numgroups, munge.default_group,
		       &munge.default_gid)) {
		error("no group '%s' in the config to default to\n",
		      munge.default_group);
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
