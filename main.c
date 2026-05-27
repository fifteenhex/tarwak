#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* libarchive */
#include <archive.h>
#include <archive_entry.h>

/* cJSON */
#include <cjson/cJSON.h>

struct user_map {
	char *name;
	uid_t uid;
};

static void free_archive(struct archive **a)
{
	if (*a)
		archive_write_free(*a);
}

static void free_file(FILE **f)
{
	if (*f)
		fclose(*f);
}

static void free_malloc(void **p)
{
	if (*p)
		free(*p);
}

static void usage(const char *prog)
{
	fprintf(stderr, "usage: %s -i <input> -o <output> -b <basedir>\n", prog);
}

static int parse_users(const cJSON *config, struct user_map **usermap, int *numusers)
{
	struct user_map *map;
	const cJSON *users;
	const cJSON *entry;
	int count;
	int i;
	users = cJSON_GetObjectItemCaseSensitive(config, "users");
	if (!cJSON_IsObject(users)) {
		fprintf(stderr, "Missing or invalid 'users' node\n");
		return -EINVAL;
	}

	count = cJSON_GetArraySize(users);
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

static int parse_config(const char *config_path, cJSON **result)
{
	FILE __attribute__((cleanup(free_file))) *config_file = NULL;
	void __attribute__((cleanup(free_malloc))) *config_buf = NULL;
	long config_len;
	cJSON *config;
	int ret;

	config_file = fopen(config_path, "r");
	if (!config_file) {
		fprintf(stderr, "Failed to open config file\n");
		return -1;
	}

	fseek(config_file, 0, SEEK_END);
	config_len = ftell(config_file);
	rewind(config_file);

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

int main(int argc, char **argv)
{
	struct archive __attribute__((cleanup(free_archive))) *tarball = NULL;
	const char *basedir = NULL;
	const char *output = NULL;
	const char *input = NULL;
	struct user_map *usermap;
	int opt, ret, numusers;
	cJSON *config_json;

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

	ret = parse_config(input, &config_json);
	if (ret)
		return 1;

	ret = parse_users(config_json, &usermap, &numusers);

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
