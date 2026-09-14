/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The bits tarwak and tarmunge both need: the little helpers, and
 * reading the users, groups and defaults out of a config.
 *
 * Header only, so neither tool has to link the other's object.
 */

#ifndef TARWAK_CONFIG_H
#define TARWAK_CONFIG_H

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <archive.h>
#include <archive_entry.h>
#include <cjson/cJSON.h>

/* Printing macros */
#define error(...) ((void)fprintf(stderr, __VA_ARGS__))

#define __must_check __attribute__((warn_unused_result))

#define ARRAY_SZ(_a) (sizeof(_a) / sizeof(_a[0]))

struct user_map {
	char *name;
	uid_t uid;
};

struct group_map {
	char *name;
	gid_t gid;
};

static inline void free_archive(struct archive **a)
{
	if (*a)
		archive_write_free(*a);
}

#define __cleanup_archive __attribute__((cleanup(free_archive)))

static inline void free_archive_entry(struct archive_entry **entry)
{
	if (*entry)
		archive_entry_free(*entry);
}

#define __cleanup_archive_entry __attribute__((cleanup(free_archive_entry)))

static inline void free_file(FILE **f)
{
	if (*f)
		fclose(*f);
}

#define __cleanup_file __attribute__((cleanup(free_file)))

static inline void free_malloc(void **p)
{
	if (*p)
		free(*p);
}

#define __cleanup_malloc __attribute__((cleanup(free_malloc)))


static inline long file_len(FILE *f)
{
	long len;

	fseek(f, 0, SEEK_END);
	len = ftell(f);
	rewind(f);

	return len;
}

static inline int __must_check lookup_uid(const struct user_map *usermap, unsigned int numusers,
				   const char *name, uid_t *result)
{
	unsigned int i;

	/* This should never be NULL */
	assert(name);

	/* root is built in, doesn't need to be in the map */
	if (strcmp(name, "root") == 0) {
		*result = 0;
		return 0;
	}

	for (i = 0; i < numusers; i++)
		if (strcmp(usermap[i].name, name) == 0) {
			*result = usermap[i].uid;
			return 0;
		}

	return -EINVAL;
}


static inline int __must_check lookup_gid(const struct group_map *groupmap, unsigned int numgroups,
				   const char *name, gid_t *result)
{
	unsigned int i;

	/* This should never be NULL */
	assert(name);

	/* root is built in, doesn't need to be in the map */
	if (strcmp(name, "root") == 0) {
		*result = 0;
		return 0;
	}

	for (i = 0; i < numgroups; i++)
		if (strcmp(groupmap[i].name, name) == 0) {
			*result = groupmap[i].gid;
			return 0;
		}

	return -EINVAL;
}


static inline int __must_check parse_users(const cJSON *config, struct user_map **usermap, int *numusers)
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
		error("No users, everything will be owned by root\n");
		*usermap = NULL;
		*numusers = 0;

		return 0;
	}

	map = calloc(count, sizeof(*map));
	if (!map)
		return -ENOMEM;

	i = 0;
	cJSON_ArrayForEach(entry, users) {
		if (!cJSON_IsNumber(entry)) {
			error("UID for '%s' is not a number\n", entry->string);
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


static inline int __must_check parse_groups(const cJSON *config, struct group_map **groupmap, int *numgroups)
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
		error("No groups, everything will be owned by root\n");
		*groupmap = NULL;
		*numgroups = 0;

		return 0;
	}

	map = calloc(count, sizeof(*map));
	if (!map)
		return -ENOMEM;

	i = 0;
	cJSON_ArrayForEach(entry, groups) {
		if (!cJSON_IsNumber(entry)) {
			error("GID for '%s' is not a number\n", entry->string);
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


static inline void parse_user_group(const cJSON *node, const char **user, const char **group)
{
	const cJSON *_group;
	const cJSON *_user;

	_user = cJSON_GetObjectItemCaseSensitive(node, "user");
	if (cJSON_IsString(_user))
		*user = _user->valuestring;

	_group = cJSON_GetObjectItemCaseSensitive(node, "group");
	if (cJSON_IsString(_group))
		*group = _group->valuestring;
}


static inline int __must_check parse_defaults(const cJSON *config,
		      const char **defaultuser,
		      const char **defaultgroup)
{
	const cJSON *defaults;

	defaults = cJSON_GetObjectItemCaseSensitive(config, "defaults");
	if (!cJSON_IsObject(defaults))
		return 0;

	parse_user_group(defaults, defaultuser, defaultgroup);

	return 0;
}

static inline int __must_check parse_config(const char *config_path, cJSON **result)
{
	void __cleanup_malloc *config_buf = NULL;
	FILE __cleanup_file *config_file = NULL;
	long config_len;
	cJSON *config;
	int ret;

	config_file = fopen(config_path, "r");
	if (!config_file) {
		error("Failed to open config file\n");
		return -1;
	}

	config_len = file_len(config_file);
	if (!config_len)
		return -EINVAL;

	config_buf = malloc(config_len + 1);
	if (!config_buf)
		return -ENOMEM;

	memset(config_buf, 0, config_len);
	ret = fread(config_buf, 1, config_len, config_file);
	if (ret != config_len) {
		error("Failed to read config file\n");
		return -1;
	}

	config = cJSON_Parse(config_buf);

	if (!config) {
		error("Failed to parse config: %s\n", cJSON_GetErrorPtr());
		return -EINVAL;
	}

	*result = config;

	return 0;
}

#endif /* TARWAK_CONFIG_H */
