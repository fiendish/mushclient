# LuaCOM source contract

[fiendish/luacom](https://github.com/fiendish/luacom) owns the shared LuaCOM code.
`luacom/` is generated from the full commit ID in `manifest.json` and the ordered
patch files listed there. Do not edit the generated files without a patch.

The upstream `integration/mushclient.json` maps every `src/library` file, the
public header, and `COPYRIGHT` to the flat host directory. The import uses exact
Git blob bytes and file modes. It does not copy files from an upstream working
tree. The current inventory has 42 files.

Only `luacom.cpp` has host patches:

1. Keep MUSHclient's exclusion of in-process DLL registration.
2. Keep MUSHclient's control of Lua helper loading.
3. Keep the embedded `StartMessageLoop` guard.

The unpatched files must match upstream exactly. The patched files must match
upstream plus the declared patches exactly. `.gitattributes` prevents line-ending
conversion for the source, manifest, and patches. This also retains whitespace
and the executable mode of `COPYRIGHT` from upstream.

## Update shared code

Make shared fixes and tests in `fiendish/luacom` first. Merge them there after its
checks pass. From a clean MUSHclient checkout, import that full commit ID:

```sh
python3 tools/luacom_vendor.py update --revision FULL_UPSTREAM_COMMIT_ID
git add -- luacom third_party/luacom/manifest.json
python3 tools/luacom_vendor.py check --index
python3 tools/run_luacom_contract.py
```

The tools require Python 3.9 or later and Git. The connection test also requires
Clang with AddressSanitizer and UndefinedBehaviorSanitizer. Add `--source` with a
local LuaCOM Git repository path to use its immutable objects without a fetch.
The connection runner accepts the same option.

An update first checks the old import. It then reconstructs the candidate source
and applies each patch in order. Patch conflicts and inventory errors stop the
update before installation. An installation error restores the old files, modes,
and manifest. If storage also prevents rollback, the tool reports the backup
path and keeps the backup. These steps do not provide crash recovery after a
power loss or forced process termination.

## Change host behavior

Host behavior changes require explicit patch changes. Start from a clean tree,
edit the necessary generated source, and record a new patch with paths relative
to `luacom/`. For example:

```sh
git diff --relative=luacom -- luacom > third_party/luacom/patches/0004-host-change.patch
```

Add the patch path to the end of `manifest.json`, then regenerate and check:

```sh
python3 tools/luacom_vendor.py apply
git add -- luacom third_party/luacom
python3 tools/luacom_vendor.py check --index
```

`apply` explicitly replaces edits to managed source files. It rejects extra
files. Patch files cannot add, delete, rename, or change the modes of exported
files. Every patch must be declared. Send shared changes upstream instead of
adding a host patch.

## Continuous checks

The `LuaCOM source parity` CI job checks the exact file inventory, bytes, and
staged Git modes against a fresh reconstruction. It also tests the import and
publication tools and runs the pinned upstream connection tests against the
host source. Both native Windows builds depend on this job.
