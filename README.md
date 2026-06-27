# TarWak

This is a tool to create tarballs based on a JSON configuration.
I needed this because I have a bunch of random files I want to
make a root filesystem out of. I don't want to make that into a
file structure I can tar up.

This also handles adding a `security.capabilities` xattr to files
that need to have `CAP_NET_RAW` etc without adding that to the
source files directly.

## Config syntax

The configuration is a few configuration items and then a filesystem
tree. `users`, `groups` and `defaults` are optional. `root` is
mandatory and is the root of the filesystem layout you want to
create.

```json
{
    "users":    { ... },
    "groups":   { ... },
    "defaults": { ... },
    "root":     { ... }
}
```

### `users`

This is a map of username to user id, `root:0` is always included
and does not need to be specified. If you only need `root` you don`t
need to define this at all.

```json
"users": {
    "dave": 1000,
    "jim":  1001
}
```

### `groups`

Same as above but for groups. Same points about `root`.

```json
"groups": {
    "kewlguys": 1000,
    "lusers":   1001
}
```

### `defaults`

This sets global defaults. Right now just the `user` and `group`
for directories and files. If not specified these default to `root`

```json
"defaults": {
    "user":  "dave",
    "group": "kewlguys"
}
```

### `root`

This is really just a entity but you can`t override any of its
properties, its always a directory etc. So it only has one key.

```json
"root": {
    "entities": { ... }
}
```

## Entities

Each key in an `entities` object is the path of a filesystem object relative to
the current directory. The path can contain `/` and you don't need to describe
the entire tree but how this works isn't entirely worked out yet. You might
not get the results you expect.

Every entity supports these common fields:

| Field     | Description                                                                    |
|-----------|--------------------------------------------------------------------------------|
| `type`    | Entry type (see below). Defaults to `regular` if omitted                       |
| `user`    | Owner username. Falls back to directory default or global default              |
| `group`   | Owner group name. Falls back to directory default or global default            |
| `mode`    | Octal permission string, e.g. `"0755"`                                         |
| `mtime`   | Modification time as `YYYY-MM-DDTHH:MM:SS`. Defaults to program start time     |
| `xattrs`  | Extended attributes                                                            |
| `feature` | Only pack this entity when a matching feature is on. See [Features](#features) |

### Features

Sometimes you want one config to spit out slightly different tarballs.
Maybe one build wants the debug tools in it and another doesn't. That
is what features are for.

Any entity can have a `feature` field. If it does, the entity only
gets packed when that feature was switched on with `-f` on the command
line. Entities without a `feature` field are always packed, so your
base layout just works like it always did.

```json
"usr/bin/gdb": {
    "source":  "usr/bin/gdb",
    "feature": "debug"
}
```

Now `-f debug` pulls `gdb` in and leaving it off drops it. You can
pass `-f` as many times as you like to turn on more than one feature:

```
tarwak -i config.json -o out.tar -b ./files -f debug -f net
```

If an entity should show up for any one of a few features, use a list
instead of a single string. Any match is enough to pack it.

```json
"usr/bin/busybox": {
    "source":  "usr/bin/busybox",
    "feature": ["debug", "rescue"]
}
```

If you put a `feature` on a `dir` and that feature is off, everything
inside the directory gets dropped along with it, so you don't have to
tag every single child.

### `dir`

A directory. May contain nested `entities` and a `defaults` block that sets
fallback `user`, `group`, and `mode` for all children.

```json
"bin": {
    "type":  "dir",
    "user":  "root",
    "group": "root",
    "mode":  "0755",
    "defaults": {
        "user":  "root",
        "group": "root",
        "mode":  "0755"
    },
    "entries": {
        "passwd": {
            "source": "mypasswordbinary.elf"
        }
    }
}
```

### `regular`

A regular file. The source file is read from the basedir (`-b`).

| Field    | Required | Description                                         |
|----------|----------|-----------------------------------------------------|
| `source` | yes*     | Path to source file, relative to `-b <basedir>`     |

\* If `-p <pattern>` is given, `source` may be omitted and the path is derived
from the pattern with the entity key. This is very wonky and how it works might
be changed later. 

```json
"usr/bin/kewlprog": {
    "type":   "regular",
    "source": "usr/bin/kewlprog",
    "mode":   "0755",
    "user":   "root",
    "group":  "root"
}
```

### `symlink`

A symbolic link.

| Field    | Required | Description          |
|----------|----------|----------------------|
| `target` | yes      | The symlink target   |

```json
"usr/bin/notkewlprog": {
    "type":   "symlink",
    "target": "/opt/prog"
}
```

### `fifo`

A named pipe. I have no idea what these would be needed for
but here they are.

```json
"run/pipeymcpipeface": {
    "type":  "fifo",
    "user":  "root",
    "group": "root",
    "mode":  "0600"
}
```

### `char` / `block`

A character or block device node.

| Field   | Required | Description         |
|---------|----------|---------------------|
| `major` | yes      | Major device number |
| `minor` | yes      | Minor device number |

```json
"dev/null": {
    "type":  "char",
    "major": 1,
    "minor": 3,
    "mode":  "0666"
},
"dev/sda": {
    "type":  "block",
    "major": 8,
    "minor": 0,
    "mode":  "0660",
    "group": "disk"
}
```
