#include <limits.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* libarchive */
#include <archive.h>
#include <archive_entry.h>

/* cJSON */
#include <cjson/cJSON.h>

/* libcap/caps */
#include <sys/capability.h>

struct user_map {
	char *name;
	uid_t uid;
};

struct group_map {
	char *name;
	gid_t gid;
};

struct context {
	const char *pattern;
	struct user_map *usermap;
	unsigned int numusers;
	struct group_map *groupmap;
	unsigned int numgroups;
	struct archive *tarball;
	#define CONTEXT_BUFFSZ (1024 * 1024)
	void *buff;
};

/* Clean up helpers */
static void free_archive(struct archive **a)
{
	if (*a)
		archive_write_free(*a);
}

static void free_archive_entry(struct archive_entry **entry)
{
	if (*entry)
		archive_entry_free(*entry);
}

#define __cleanup_archive_entry __attribute__((cleanup(free_archive_entry)))

static void free_file(FILE **f)
{
	if (*f)
		fclose(*f);
}

#define __cleanup_file __attribute__((cleanup(free_file)))

static void free_malloc(void **p)
{
	if (*p)
		free(*p);
}

/* Util functions */
#define ARRAY_SZ(_a) (sizeof(_a) / sizeof(_a[0]))

static inline long file_len(FILE *f)
{
	long len;

	fseek(f, 0, SEEK_END);
	len = ftell(f);
	rewind(f);

	return len;
}

static void usage(const char *prog)
{
	fprintf(stderr, "usage: %s -i <input> -o <output> -b <basedir> -p <pattern>\n", prog);
}

static int parse_users(const cJSON *config, struct user_map **usermap, int *numusers)
{
	struct user_map *map;
	const cJSON *users;
	const cJSON *entry;
	int count = 0;
	int i;

	users = cJSON_GetObjectItemCaseSensitive(config, "users");
	if (cJSON_IsObject(users))
		count = cJSON_GetArraySize(users);

	/* No user mapping is fine, everything is root */
	if (!count) {
		fprintf(stderr, "No users, everything will be owned by root\n");
		*usermap = NULL;
		*numusers = 0;
	}

	map = calloc(count, sizeof(*map));
	if (!map)
		return -ENOMEM;

	i = 0;
	cJSON_ArrayForEach(entry, users) {
		if (!cJSON_IsNumber(entry)) {
			fprintf(stderr, "UID for '%s' is not a number\n", entry->string);
			return -EINVAL;
		}
		map[i].name = entry->string;
		map[i].uid  = (uid_t)entry->valuedouble;
		i++;
	}

	*usermap = map;
	*numusers = count;

	map = NULL;

	return 0;
}

static int parse_groups(const cJSON *config, struct group_map **groupmap, int *numgroups)
{
	struct group_map *map;
	const cJSON *groups;
	const cJSON *entry;
	int count = 0;
	int i;

	groups = cJSON_GetObjectItemCaseSensitive(config, "groups");
	if (cJSON_IsObject(groups))
		count = cJSON_GetArraySize(groups);

	/* no group map is fine */
	if (!count) {
		fprintf(stderr, "No groups, everything will be owned by root\n");
		*groupmap = NULL;
		*numgroups = 0;
	}

	map = calloc(count, sizeof(*map));
	if (!map)
		return -ENOMEM;

	i = 0;
	cJSON_ArrayForEach(entry, groups) {
		if (!cJSON_IsNumber(entry)) {
			fprintf(stderr, "GID for '%s' is not a number\n", entry->string);
			return -EINVAL;
		}
		map[i].name = entry->string;
		map[i].gid  = (gid_t)entry->valuedouble;
		i++;
	}

	*groupmap = map;
	*numgroups = count;

	map = NULL;

	return 0;
}

static int parse_root(const cJSON *config, const cJSON **rootentries,
		      const char **defaultuser, const char **defaultgroup)
{
	const cJSON *root, *entries;

	root = cJSON_GetObjectItemCaseSensitive(config, "root");
	if (!cJSON_IsObject(root)) {
		fprintf(stderr, "Missing or invalid 'root' node\n");
		return -EINVAL;
	}

	entries = cJSON_GetObjectItemCaseSensitive(root, "entries");
	if (!cJSON_IsObject(entries)) {
		fprintf(stderr, "Missing or invalid 'entries' node\n");
		return -EINVAL;
	}

	*rootentries = entries;

	return 0;
}

static int parse_config(const char *config_path, cJSON **result)
{
	FILE __cleanup_file *config_file = NULL;
	void __attribute__((cleanup(free_malloc))) *config_buf = NULL;
	long config_len;
	cJSON *config;
	int ret;

	config_file = fopen(config_path, "r");
	if (!config_file) {
		fprintf(stderr, "Failed to open config file\n");
		return -1;
	}

	config_len = file_len(config_file);

	config_buf = malloc(config_len + 1);
	if (!config_buf)
		return -ENOMEM;

	memset(config_buf, 0, config_len);
	ret = fread(config_buf, 1, config_len, config_file);
	if (ret != config_len) {
		fprintf(stderr, "Failed to read config file\n");
		return -1;
	}

	config = cJSON_Parse(config_buf);

	if (!config) {
		fprintf(stderr, "Failed to parse config: %s\n", cJSON_GetErrorPtr());
		return -EINVAL;
	}

	*result = config;

	return 0;
}

static int encode_capability(const char *str, struct vfs_cap_data *cap)
{
	int has_effective = 0;
	cap_flag_value_t val;
	cap_t c = { 0 };
	int i;

	cap->magic_etc = VFS_CAP_REVISION_2;

	c = cap_from_text(str);
	if (!c) {
		fprintf(stderr, "failed to parse capability string '%s'\n", str);
		return -EINVAL;
	}

	/* This is copy/paste, I hope it works! */
	for (i = 0; i <= CAP_LAST_CAP; i++) {
		int idx = i / 32;
		int bit = i % 32;

		if (cap_get_flag(c, i, CAP_PERMITTED, &val) == 0 && val == CAP_SET)
		    cap->data[idx].permitted   |= (1u << bit);

		if (cap_get_flag(c, i, CAP_INHERITABLE, &val) == 0 && val == CAP_SET)
		    cap->data[idx].inheritable |= (1u << bit);

		if (cap_get_flag(c, i, CAP_EFFECTIVE, &val) == 0 && val == CAP_SET)
				 has_effective = 1;
	}

	if (has_effective)
		cap->magic_etc |= VFS_CAP_FLAGS_EFFECTIVE;

	cap_free(c);

	return 0;
}

static void apply_xattrs(struct archive_entry *entry, const cJSON *xattrs)
{
	const cJSON *xattr;
	int ret;

	if (!cJSON_IsObject(xattrs))
		return;

	cJSON_ArrayForEach(xattr, xattrs) {
		if (!cJSON_IsString(xattr))
			continue;

		/* Security caps are a special case and should be an array that is
		 * smushed into one xattr? This will be broken for multiple caps
		 * right now but I just want the cap for ping as normal user right now.
		 */
		if (strcmp(xattr->string, "security.capability") == 0) {
			struct vfs_cap_data cap;

			ret = encode_capability(xattr->valuestring, &cap);
			if (ret)
				return;

			archive_entry_xattr_add_entry(entry, xattr->string, &cap, sizeof(cap));
		}
		/* Normal xattrs */
		else {
			archive_entry_xattr_add_entry(entry, xattr->string,
						xattr->valuestring,
						strlen(xattr->valuestring));
		}
	}
}

static const cJSON *json_xattrs(const cJSON *node)
{
	return cJSON_GetObjectItemCaseSensitive(node, "xattrs");
}

static int add_symlink(struct context *context, const char *path, const char *target)
{
	struct archive_entry __cleanup_archive_entry *entry = NULL;

	entry = archive_entry_new();
	archive_entry_set_pathname(entry, path);
	archive_entry_set_filetype(entry, AE_IFLNK);
	archive_entry_set_symlink(entry, target);
	archive_entry_set_perm(entry, 0777);

	if (archive_write_header(context->tarball, entry) != ARCHIVE_OK)
		return -1;

	return 0;
}

static int do_symlink(struct context *context, const cJSON *node, const char *cwd)
{
	const cJSON *target;
	char path[PATH_MAX];

	target = cJSON_GetObjectItemCaseSensitive(node, "target");
	if (!cJSON_IsString(target)) {
		fprintf(stderr, "symlink '%s' missing 'target'\n", node->string);
		return -EINVAL;
	}

	snprintf(path, sizeof(path), "%s%s", cwd, node->string);

	return add_symlink(context, path, target->valuestring);
}

static int traverse_entries(struct context *context, const cJSON *entries, const char *path);

static int add_file(struct context *context, const char *path,
		    size_t data_len, int (*read_data)(void *dst, size_t len, void *priv), void *read_data_priv,
		    const cJSON *xattrs)
{
	struct archive_entry __cleanup_archive_entry *entry = NULL;

	/* Start a regualar file */
	entry = archive_entry_new();
	archive_entry_set_pathname(entry, path);
	archive_entry_set_filetype(entry, AE_IFREG);
	archive_entry_set_perm(entry, 0644);
	archive_entry_set_size(entry, data_len);

	/* Apply any xattrs */
	apply_xattrs(entry, xattrs);

	if (archive_write_header(context->tarball, entry) != ARCHIVE_OK) {
		fprintf(stderr, "Failed to add file\n");
		return -1;
	}

	while (data_len) {
		size_t read_len = CONTEXT_BUFFSZ;
		int ret;

		if (read_len > data_len)
			read_len = data_len;

		ret = read_data(context->buff, read_len, read_data_priv);
		if (ret <= 0)
			return -EIO;

		ret = archive_write_data(context->tarball, context->buff, ret);
		if (ret < 0)
			return -EIO;

		data_len -= ret;
	}

	return 0;
}

static int read_file(void *dst, size_t len, void *priv)
{
	FILE *f = (FILE *) priv;

	return fread(dst, 1, len, f);
}

static int get_source_path(struct context *context, const cJSON *node, const char *name, char *buf, const char **result)
{
	const cJSON *source;

	source = cJSON_GetObjectItemCaseSensitive(node, "source");
	if (source) {
		if (!cJSON_IsString(source)) {
			fprintf(stderr, "source for %s is not a string\n", node->string);
			return -EINVAL;
		}
		*result = source->valuestring;
		return 0;
	}

	if (context->pattern) {
		sprintf(buf, context->pattern, name);
		*result = buf;
		return 0;
	}

	fprintf(stderr, "file '%s' missing 'source' and no pattern specified?\n", node->string);

	return -EINVAL;
}

static int do_regular(struct context *context, const cJSON *node, const char *cwd)
{
	FILE __cleanup_file *f = NULL;
	char path[PATH_MAX];
	char tmp[PATH_MAX];
	const char *source_path;
	const char *name;
	long len;
	int ret;

	name = node->string;
	snprintf(path, sizeof(path), "%s%s", cwd, name);

	ret = get_source_path(context, node, name, tmp, &source_path);
	if (ret)
		return ret;

	f = fopen(source_path, "rb");
	if (!f) {
		fprintf(stderr, "failed to open '%s'\n", source_path);
		return -1;
	}

	len = file_len(f);

	return add_file(context, path, len, read_file, f, json_xattrs(node));
}

static int add_dir(struct context *context, const char *path)
{
	struct archive_entry __cleanup_archive_entry *entry = NULL;
	int ret;

	entry = archive_entry_new();
	archive_entry_set_pathname(entry, path);
	archive_entry_set_filetype(entry, AE_IFDIR);
	archive_entry_set_perm(entry, 0755);
	ret = archive_write_header(context->tarball, entry);

	if (ret != ARCHIVE_OK)
		return -1;

	return 0;
}

static int do_dir(struct context *context, const cJSON *node, const char *cwd)
{
	const cJSON *entries;
	char tmp[PATH_MAX];
	int ret;

	sprintf(tmp, "%s%s/", cwd, node->string);

	ret = add_dir(context, tmp);
	if (ret)
		return ret;

	entries = cJSON_GetObjectItemCaseSensitive(node, "entries");
	if (cJSON_IsObject(entries)) {
		ret = traverse_entries(context, entries, tmp);
		if (ret)
			return ret;
	}

	return 0;
}

struct entry_handler {
	const char* type;
	int (*cb)(struct context *context, const cJSON *node, const char *cwd);
};

static const struct entry_handler entry_handlers[] = {
	{ "dir", do_dir },
	{ "regular", do_regular },
	{ "symlink", do_symlink },
};

static int traverse_entries(struct context *context, const cJSON *entries, const char *path)
{
	const char *type_str;
	const cJSON *node;
	const cJSON *type;
	int ret;
	int i;

	cJSON_ArrayForEach(node, entries) {
		bool handled = false;

		type = cJSON_GetObjectItemCaseSensitive(node, "type");

		if (type) {
			if (!cJSON_IsString(type)) {
				fprintf(stderr, "Entry '%s' missing 'type'\n", node->string);
				return -EINVAL;
			}
			else
				type_str = type->valuestring;
		}
		else
			/* Default to regular file */
			type_str = "regular";

		printf("%s (%s)\n", node->string, type_str);

		for (i = 0; i < ARRAY_SZ(entry_handlers); i++) {
			if (strcmp(type_str, entry_handlers[i].type) == 0) {
				ret = entry_handlers[i].cb(context, node, path);
				if (ret)
					return ret;

				handled = true;

				break;
			}
		}

		if (!handled) {
			fprintf(stderr, "Unknown type '%s'\n", type_str);
			return -EINVAL;
		}
	}

	return 0;
}

int main(int argc, char **argv)
{
	struct archive __attribute__((cleanup(free_archive))) *tarball = NULL;
	const char *basedir = NULL;
	const char *output = NULL;
	const char *input = NULL;
	const char *pattern = NULL;
	struct group_map *groupmap;
	struct user_map *usermap;
	int opt, ret, numusers, numgroups;
	const char *defaultuser, *defaultgroup;
	const cJSON *entries;
	cJSON *config_json;
	struct context context;

	while ((opt = getopt(argc, argv, "i:o:b:p:")) != -1) {
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
		case 'p':
			pattern = optarg;
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

	/* Config initial parsing */
	ret = parse_config(input, &config_json);
	if (ret)
		return 1;

	ret = parse_users(config_json, &usermap, &numusers);
	if (ret)
		return 1;

	ret = parse_groups(config_json, &groupmap, &numgroups);
	if (ret)
		return 1;

	ret = parse_root(config_json, &entries, &defaultuser, &defaultgroup);
	if (ret)
		return 1;

	/* Warm up a tarball */
	tarball = archive_write_new();
	archive_write_set_format_pax_restricted(tarball);
	archive_write_add_filter_none(tarball);

	/* So GNU Tar can handle our xattrs */
	archive_write_set_options(tarball, "xattrheader=SCHILY");

	if (archive_write_open_filename(tarball, output) != ARCHIVE_OK) {
		fprintf(stderr, "failed to open output: %s\n", archive_error_string(tarball));
		return 1;
	}

	/* Change into the working directory */
	ret = chdir(basedir);
	if (ret)
		return 1;

	/* Gets real here! */
	context.buff = malloc(CONTEXT_BUFFSZ);
	if (!context.buff)
		return 1;

	context.pattern = pattern;
	context.tarball = tarball;
	ret = traverse_entries(&context, entries, "/");
	if (ret)
		return 1;

	/* Pack it up! */
	archive_write_close(tarball);

	return 0;
}
