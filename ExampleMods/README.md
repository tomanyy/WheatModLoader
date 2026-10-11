# Example mods

Each folder here is a standalone mod. To try one, copy its folder into the
loader's `wheat/mods` directory next to `cs2.exe`. Keep the folder name and
make sure its entry point is named `main.lua`.

The current host API is intentionally small: `events.on(name, callback)` can
subscribe to `mods_loaded` and `frame`; frame callbacks receive elapsed seconds
and a frame number. The `wheat` table provides `api_version`, `mod_name`, and
`now()` (monotonic seconds). Mods have separate sandboxes, so these examples do
not access game internals or each other's state.
