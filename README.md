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
    "entries": { ... }
}
```

** WORK IN PROGRESS **
