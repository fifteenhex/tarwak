#define _XOPEN_SOURCE
#include <assert.h>
#include <limits.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* libarchive */
#include <archive.h>
#include <archive_entry.h>

/* cJSON */
#include <cjson/cJSON.h>

/* libcap/caps */
#include <sys/capability.h>

/* For special files */
#include <sys/sysmacros.h>

/* JSON keys */
#define KEY_ENTITIES "entities"

/* Printing macros */
#define error(...) ((void)fprintf(stderr, __VA_ARGS__))

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
	time_t start_time;
	struct user_map *usermap;
	unsigned int numusers;
	struct group_map *groupmap;
	unsigned int numgroups;
	struct archive *tarball;
	#define CONTEXT_BUFFSZ (1024 * 1024)
	void *buff;

	/* Defaults */
	const char *default_user;
	uid_t default_uid;
	const char *default_group;
	gid_t default_gid;
};

struct metadata {
	bool have_mode;
	mode_t mode;

	bool have_user;
	uid_t uid;
	const char* user;

	bool have_group;
	gid_t gid;
	const char *group;

	time_t ts;
};

/* Holder for properties we'll apply to entities */
struct entity_context {
	struct metadata properties;

	bool have_cap;
	struct vfs_cap_data cap;

	const cJSON *node;
};

#define ENTITY_NODE(_e) (_e->node)

/* Holder for properties that a directory can provide as defines to children */
struct directory_context;

struct directory_context {
	const struct directory_context *parent;
	struct metadata defaults;
	const cJSON *entities;
	const char *path;
};

#define DIR_PATH(_d) (_d->path)

#define __must_check __attribute__((warn_unused_result))

static int __must_check lookup_uid(const struct context *context, const char *name, uid_t *result)
{
	unsigned int i;

	/* This should never be NULL */
	assert(name);

	/* root is built in, doesn't need to be in the map */
	if (strcmp(name, "root") == 0) {
		*result = 0;
		return 0;
	}

	for (i = 0; i < context->numusers; i++)
		if (strcmp(context->usermap[i].name, name) == 0) {
			*result = context->usermap[i].uid;
			return 0;
		}

	return -EINVAL;
}

static int __must_check lookup_gid(const struct context *context, const char *name, gid_t *result)
{
	unsigned int i;

	/* This should never be NULL */
	assert(name);

	/* root is built in, doesn't need to be in the map */
	if (strcmp(name, "root") == 0) {
		*result = 0;
		return 0;
	}

	for (i = 0; i < context->numgroups; i++)
		if (strcmp(context->groupmap[i].name, name) == 0) {
			*result = context->groupmap[i].gid;
			return 0;
		}

	return -EINVAL;
}

/* Clean up helpers */
static void free_archive(struct archive **a)
{
	if (*a)
		archive_write_free(*a);
}

#define __cleanup_archive __attribute__((cleanup(free_archive)))

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

#define __cleanup_malloc __attribute__((cleanup(free_malloc)))

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
	error("usage: %s -i <input> -o <output> -b <basedir> -p <pattern>\n", prog);
}

static int __must_check parse_users(const cJSON *config, struct user_map **usermap, int *numusers)
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

static int __must_check parse_groups(const cJSON *config, struct group_map **groupmap, int *numgroups)
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

static void parse_user_group(const cJSON *node, const char **user, const char **group)
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

static int __must_check parse_root(const cJSON *config,
		      const cJSON **rootentities)
{
	const cJSON *root, *entities;

	root = cJSON_GetObjectItemCaseSensitive(config, "root");
	if (!cJSON_IsObject(root)) {
		error("Missing or invalid 'root' node\n");
		return -EINVAL;
	}

	entities = cJSON_GetObjectItemCaseSensitive(root, KEY_ENTITIES);
	if (!cJSON_IsObject(entities)) {
		error("Missing or invalid 'entities' node\n");
		return -EINVAL;
	}

	*rootentities = entities;


	return 0;
}

static int __must_check parse_defaults(const cJSON *config,
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

static int __must_check parse_config(const char *config_path, cJSON **result)
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

static time_t parse_timestamp(const char *str)
{
	struct tm tm = { 0 };

	if (!strptime(str, "%Y-%m-%dT%H:%M:%S", &tm)) {
		error("invalid timestamp '%s', expected YYYY-MM-DDTHH:MM:SS\n", str);
		return (time_t)-1;
	}

	return mktime(&tm);
}

static int __must_check encode_capability(const char *str, struct vfs_cap_data *cap)
{
	int has_effective = 0;
	cap_flag_value_t val;
	cap_t c = { 0 };
	int i;

	cap->magic_etc = VFS_CAP_REVISION_2;

	c = cap_from_text(str);
	if (!c) {
		error("failed to parse capability string '%s'\n", str);
		return -EINVAL;
	}

	/* This is copy/paste, I hope it works! */
	for (i = 0; i <= CAP_LAST_CAP; i++) {
		int idx = i / 32;
		int bit = i % 32;

		if (cap_get_flag(c, i, CAP_PERMITTED, &val) == 0 && val == CAP_SET)
		    cap->data[idx].permitted |= (1u << bit);

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

static void collect_timestamps(const cJSON *node,
			       const struct context *context,
			       struct entity_context *entity_context)
{
	struct metadata *properties = &entity_context->properties;
	const cJSON *mtime;
	time_t ts;

	mtime = cJSON_GetObjectItemCaseSensitive(node, "mtime");

	if (cJSON_IsString(mtime)) {
		ts = parse_timestamp(mtime->valuestring);
		if (ts == (time_t)-1)
			ts = context->start_time;
	} else {
		ts = context->start_time;
	}

	properties->ts = ts;
}

static void apply_timestamps(struct archive_entry *entry,
			     const struct entity_context *entity_context)
{
	const struct metadata *properties = &entity_context->properties;

	archive_entry_set_mtime(entry, properties->ts, 0);
}

/* Extract metadata fields from an object */
static int __must_check parse_metadata(const struct context *context,
			   const cJSON *node,
			   struct metadata *metadata)
{
	const char *group = NULL;
	const char *user = NULL;
	const cJSON *mode;
	int ret;

	/* Just in case */
	memset(metadata, 0, sizeof(*metadata));

	mode = cJSON_GetObjectItemCaseSensitive(node, "mode");
	if (cJSON_IsString(mode)) {
		metadata->mode = (mode_t)strtol(mode->valuestring, NULL, 8);
		metadata->have_mode = true;
	}

	parse_user_group(node, &user, &group);

	if (user) {
		uid_t uid;
		ret = lookup_uid(context, user, &uid);
		if (ret)
			return ret;

		metadata->uid = uid;
		metadata->user = user;
		metadata->have_user = true;
	}

	if (group) {
		gid_t gid;
		ret = lookup_gid(context, group, &gid);
		if (ret)
			return ret;

		metadata->gid = gid;
		metadata->group = group;
		metadata->have_group = true;
	}

	return 0;
}

/* Work out the effective metadata for an entity */
static void collect_metadata(const struct context *context,
			     const struct directory_context *directory_context,
			     struct entity_context *entity_context,
			     const char *type)
{
	const struct metadata *defaults = &directory_context->defaults;
	struct metadata *properties = &entity_context->properties;

	if (properties->have_mode) {
		/* nop: its already there */
	}
	/* Use default mode if one wasn't specified and exists */
	else if (defaults->have_mode)
		properties->mode = defaults->mode;
	/* Otherwise revert to some sensible defaults */
	else {
		if (strcmp(type, "dir") == 0)
			properties->mode = 0500;
		else
			properties->mode = 0400;
	}

	if (properties->have_user) {
		/* nop: its already there */
	}
	/* Use default user if one wasn't specified and exists */
	else if (defaults->have_user) {
		properties->uid = defaults->uid;
		properties->user = defaults->user;
	}
	/* Use the global default */
	else {
		properties->uid = context->default_uid;
		properties->user = context->default_user;
	}

	if (properties->have_group) {
		/* nop: its already there */
	}
	/* Use default group if one wasn't specified and exists */
	else if (defaults->have_group) {
		properties->gid = defaults->gid;
		properties->group = defaults->group;
	}
	/* Use the global default */
	else {
		properties->gid = context->default_gid;
		properties->group = context->default_group;
	}
}

/* Jam metadata into archive entry */
static void _apply_metadata(struct archive_entry *entry,
			    const struct entity_context *entity_context,
			    bool set_perm)
{
	const struct metadata *properties = &entity_context->properties;

	if (set_perm)
		archive_entry_set_perm(entry, properties->mode);
	archive_entry_set_uid(entry, properties->uid);
	archive_entry_set_uname(entry, properties->user);
	archive_entry_set_gid(entry, properties->gid);
	archive_entry_set_gname(entry, properties->group);
}

static void apply_metadata(struct archive_entry *entry,
			   const struct entity_context *entity_context)
{
	_apply_metadata(entry, entity_context, true);
}

#define XATTR_SEC_CAP "security.capability"

static int __must_check parse_xattrs(const cJSON *node, struct entity_context *entity_context)
{
	const cJSON *xattrs;
	const cJSON *xattr;
	int ret;

	xattrs = cJSON_GetObjectItemCaseSensitive(node, "xattrs");

	/* No extended attributes */
	if (!xattrs)
		return 0;

	if (!cJSON_IsObject(xattrs))
		return -EINVAL;

	cJSON_ArrayForEach(xattr, xattrs) {
		if (!cJSON_IsString(xattr))
			continue;

		/* Security caps are a special case and should be an array that is
		 * smushed into one xattr? This will be broken for multiple caps
		 * right now but I just want the cap for ping as normal user right now.
		 */
		if (strcmp(xattr->string, XATTR_SEC_CAP) == 0) {
			const char *cap = xattr->valuestring;

			error("Adding security capability: %s\n", cap);
			ret = encode_capability(cap, &entity_context->cap);
			if (ret)
				return ret;

			entity_context->have_cap = true;
		}
		/* Normal xattrs */
		else {

		}
	}

	return 0;
}

static void apply_xattrs(struct archive_entry *entry, const struct entity_context *entity_context)
{
	if (entity_context->have_cap) {
		const struct vfs_cap_data *cap = &entity_context->cap;

		archive_entry_xattr_add_entry(entry, XATTR_SEC_CAP, cap, sizeof(*cap));
	}

#if 0
	archive_entry_xattr_add_entry(entry, xattr->string,
				xattr->valuestring,
				strlen(xattr->valuestring));
#endif
}

static struct archive_entry __must_check *start_entity(const char *path, unsigned int type)
{
	struct archive_entry *entry;

	entry = archive_entry_new();
	if (!entry)
		return NULL;

	archive_entry_set_pathname(entry, path);
	archive_entry_set_filetype(entry, type);

	return entry;
}

/* Symlinks */
static int add_symlink(const struct context *context,
		       const struct entity_context *entity_context,
		       const char *path,
		       const char *target)
{
	struct archive_entry __cleanup_archive_entry *entry = NULL;

	entry = start_entity(path, AE_IFLNK);
	if (!entry)
		return -ENOMEM;

	archive_entry_set_symlink(entry, target);

	apply_timestamps(entry, entity_context);

	/* Apply the user/group, but hardcode the permissions */
	_apply_metadata(entry, entity_context, false);
	archive_entry_set_perm(entry, 0777);

	if (archive_write_header(context->tarball, entry) != ARCHIVE_OK)
		return -1;

	return 0;
}

static int do_symlink(const struct context *context,
		      const struct directory_context *directory_context,
		      struct entity_context *entity_context)
{
	const cJSON *node = ENTITY_NODE(entity_context);
	const cJSON *target;
	char path[PATH_MAX];

	target = cJSON_GetObjectItemCaseSensitive(node, "target");
	if (!cJSON_IsString(target)) {
		error("symlink '%s' missing 'target'\n", node->string);
		return -EINVAL;
	}

	snprintf(path, sizeof(path), "%s%s", DIR_PATH(directory_context), node->string);

	return add_symlink(context, entity_context, path, target->valuestring);
}

/* Normal files */
static int add_file(const struct context *context,
		    const struct entity_context *entity_context,
		    const char *path,
		    size_t data_len, int (*read_data)(void *dst, size_t len, void *priv), void *read_data_priv)
{
	struct archive_entry __cleanup_archive_entry *entry = NULL;

	/* Start a regualar file */
	entry = start_entity(path, AE_IFREG);
	if (!entry)
		return -ENOMEM;

	archive_entry_set_size(entry, data_len);

	/* Apply file permissions */
	apply_metadata(entry, entity_context);

	/* Apply timestamps */
	apply_timestamps(entry, entity_context);

	/* Apply any xattrs */
	apply_xattrs(entry, entity_context);

	if (archive_write_header(context->tarball, entry) != ARCHIVE_OK) {
		error("Failed to add file\n");
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

static int get_source_path(const struct context *context,
			   const cJSON *node,
			   const char *name,
			   char *buf,
			   const char **result)
{
	const cJSON *source;

	source = cJSON_GetObjectItemCaseSensitive(node, "source");
	if (source) {
		if (!cJSON_IsString(source)) {
			error("source for %s is not a string\n", node->string);
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

	error("file '%s' missing 'source' and no pattern specified?\n", node->string);

	return -EINVAL;
}

static int do_regular(const struct context *context,
		      const struct directory_context *directory_context,
		      struct entity_context *entity_context)
{
	const cJSON *node = ENTITY_NODE(entity_context);
	FILE __cleanup_file *f = NULL;
	char path[PATH_MAX];
	char tmp[PATH_MAX];
	const char *source_path;
	const char *name;
	long len;
	int ret;

	name = node->string;
	snprintf(path, sizeof(path), "%s%s", directory_context->path, name);

	ret = get_source_path(context, node, name, tmp, &source_path);
	if (ret)
		return ret;

	f = fopen(source_path, "rb");
	if (!f) {
		error("failed to open '%s'\n", source_path);
		return -1;
	}

	len = file_len(f);

	return add_file(context, entity_context, path, len, read_file, f);
}

/* Directories */
static int add_dir(const struct context *context,
		   const struct entity_context *entity_context,
		   const char *path)
{
	struct archive_entry __cleanup_archive_entry *entry = NULL;
	int ret;

	entry = start_entity(path, AE_IFDIR);
	if (!entry)
		return -ENOMEM;

	/* Apply timestamps */
	apply_timestamps(entry, entity_context);

	/* Apply permissions, user, group */
	apply_metadata(entry, entity_context);

	ret = archive_write_header(context->tarball, entry);

	if (ret != ARCHIVE_OK)
		return -1;

	return 0;
}

static int traverse_entities(const struct context *context,
			    const struct directory_context *directory_context);

static int do_dir(const struct context *context,
		  const struct directory_context *directory_context,
		  struct entity_context *entity_context)
{
	struct directory_context this_directory_context = {
		.parent = directory_context,
	};
	const cJSON *node = ENTITY_NODE(entity_context);
	const cJSON *defaults;
	const cJSON *entities;
	char tmp[PATH_MAX];
	int ret;

	sprintf(tmp, "%s%s/", DIR_PATH(directory_context), node->string);

	ret = add_dir(context, entity_context, tmp);
	if (ret)
		return ret;

	entities = cJSON_GetObjectItemCaseSensitive(node, KEY_ENTITIES);
	if (cJSON_IsObject(entities)) {
		/* Setup the context to be used for our children */
		this_directory_context.entities = entities;
		this_directory_context.path = tmp;

		/* Grab the defaults if any. */
		defaults = cJSON_GetObjectItemCaseSensitive(node, "defaults");
		if (cJSON_IsObject(defaults)) {
			ret = parse_metadata(context, defaults, &this_directory_context.defaults);
			if (ret)
				return ret;
		}

		ret = traverse_entities(context, &this_directory_context);
		if (ret)
			return ret;
	}

	return 0;
}

/* FIFOs, not sure if we really need this,.. */
static int add_fifo(const struct context *context,
		    const struct entity_context *entity_context,
		    const char *path)
{
	struct archive_entry __cleanup_archive_entry *entry = NULL;
	int ret;

	entry = start_entity(path, AE_IFIFO);
	if (!entry)
		return -ENOMEM;

	apply_metadata(entry, entity_context);
	apply_timestamps(entry, entity_context);

	ret = archive_write_header(context->tarball, entry);
	if (ret != ARCHIVE_OK)
		return -1;

	return 0;
}

static int do_fifo(const struct context *context,
		   const struct directory_context *directory_context,
		   struct entity_context *entity_context)
{
	const cJSON *node = ENTITY_NODE(entity_context);
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "%s%s",
		 DIR_PATH(directory_context),
		 node->string);

	return add_fifo(context, entity_context, path);
}

/* block and char */
static int add_device(const struct context *context,
		      const struct entity_context *entity_context,
		      const char *path, unsigned int filetype,
		      unsigned int major, unsigned int minor)
{
	struct archive_entry __cleanup_archive_entry *entry = NULL;
	int ret;

	entry = start_entity(path, filetype);
	if (!entry)
		return -ENOMEM;

	archive_entry_set_rdev(entry, makedev(major, minor));
	apply_metadata(entry, entity_context);
	apply_timestamps(entry, entity_context);

	ret = archive_write_header(context->tarball, entry);
	if (ret != ARCHIVE_OK)
		return -1;

	return 0;
}

static int do_device(const struct context *context,
		     const struct directory_context *directory_context,
		     struct entity_context *entity_context,
		     unsigned int filetype)
{
	const cJSON *node = ENTITY_NODE(entity_context);
	const cJSON *major, *minor;
	char path[PATH_MAX];

	major = cJSON_GetObjectItemCaseSensitive(node, "major");
	minor = cJSON_GetObjectItemCaseSensitive(node, "minor");

	if (!cJSON_IsNumber(major) || !cJSON_IsNumber(minor)) {
		error("device '%s' missing 'major' or 'minor'\n", node->string);
		return -EINVAL;
	}

	snprintf(path, sizeof(path), "%s%s", DIR_PATH(directory_context), node->string);

	return add_device(context, entity_context, path, filetype,
			  (unsigned int)major->valueint,
			  (unsigned int)minor->valueint);
}

static int do_char(const struct context *context,
		   const struct directory_context *directory_context,
		   struct entity_context *entity_context)
{
	return do_device(context, directory_context, entity_context, AE_IFCHR);
}

static int do_block(const struct context *context,
		    const struct directory_context *directory_context,
		    struct entity_context *entity_context)
{
	return do_device(context, directory_context, entity_context, AE_IFBLK);
}

struct entry_handler {
	const char* type;
	int (*cb)(const struct context *context,
		  const struct directory_context *directory_context,
		  struct entity_context *entity_context);
};

static const struct entry_handler entry_handlers[] = {
	{ "dir", do_dir },
	{ "regular", do_regular },
	{ "symlink", do_symlink },
	{ "fifo", do_fifo },
	{ "char", do_char },
	{ "block", do_block },
};

static int traverse_entities(const struct context *context,
			    const struct directory_context *directory_context)
{
	const char *type_str;
	const cJSON *node;
	const cJSON *type;
	int ret;
	int i;

	cJSON_ArrayForEach(node, directory_context->entities) {
		struct entity_context entity_context = {
			.node = node,
		};
		bool handled = false;

		type = cJSON_GetObjectItemCaseSensitive(node, "type");

		if (type) {
			if (!cJSON_IsString(type)) {
				error("Entry '%s' missing 'type'\n", node->string);
				return -EINVAL;
			}
			else
				type_str = type->valuestring;
		}
		else
			/* Default to regular file */
			type_str = "regular";

		/* Pull out the entities own local properties */
		ret = parse_metadata(context, node, &entity_context.properties);
		if (ret)
			return ret;

		/* Get the extended attributes */
		ret = parse_xattrs(node, &entity_context);
		if (ret)
			return ret;

		printf("%s (%s)\n", node->string, type_str);

		/* Collect together all of the properties that are valid for any type */
		collect_timestamps(node, context, &entity_context);
		collect_metadata(context, directory_context, &entity_context, type_str);

		for (i = 0; i < ARRAY_SZ(entry_handlers); i++) {
			if (strcmp(type_str, entry_handlers[i].type) == 0) {
				ret = entry_handlers[i].cb(context, directory_context, &entity_context);
				if (ret)
					return ret;

				handled = true;

				break;
			}
		}

		if (!handled) {
			error("Unknown type '%s'\n", type_str);
			return -EINVAL;
		}
	}

	return 0;
}

int main(int argc, char **argv)
{
	struct archive __cleanup_archive *tarball = NULL;
	struct directory_context root_directory_context = { 0 };
	struct context context = { 0 };
	const char *basedir = NULL;
	const char *output = NULL;
	const char *input = NULL;
	const char *pattern = NULL;
	struct group_map *groupmap;
	struct user_map *usermap;
	int opt, ret, numusers, numgroups;
	const char *defaultgroup = NULL;
	const char *defaultuser = NULL;
	const cJSON *entities;
	cJSON *config_json;

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

	ret = parse_root(config_json, &entities);
	if (ret)
		return 1;

	ret = parse_defaults(config_json, &defaultuser, &defaultgroup);
	if (ret)
		return 1;

	/* Warm up a tarball */
	tarball = archive_write_new();
	archive_write_set_format_pax_restricted(tarball);
	archive_write_add_filter_none(tarball);

	/* So GNU Tar can handle our xattrs */
	archive_write_set_options(tarball, "xattrheader=SCHILY");

	if (archive_write_open_filename(tarball, output) != ARCHIVE_OK) {
		error("failed to open output: %s\n", archive_error_string(tarball));
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

	/* Initialise global context */
	context.start_time = time(NULL);
	context.usermap = usermap;
	context.numusers = numusers;
	context.groupmap = groupmap;
	context.numgroups = numgroups;
	context.pattern = pattern;
	context.tarball = tarball;

	/* Setup defaults */
	context.default_user = defaultuser ? defaultuser : "root";
	ret = lookup_uid(&context, context.default_user, &context.default_uid);
	if (ret)
		return 1;

	context.default_group = defaultgroup ? defaultgroup : "root";
	ret = lookup_gid(&context, context.default_group, &context.default_gid);
	if (ret)
		return 1;

	/* Initilise context for root directory */
	root_directory_context.entities = entities;
	root_directory_context.path = "/";

	/* Print out some useful info before going for it */
	printf("Starting TAR creation..\n");
	printf("default user: \'%s\', default group \'%s\'\n",
		context.default_user, context.default_group);

	ret = traverse_entities(&context, &root_directory_context);
	if (ret)
		return 1;

	/* Pack it up! */
	archive_write_close(tarball);

	return 0;
}
