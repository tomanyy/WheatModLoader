# Mod API (version 1)

A mod is a folder in `wheat/mods/`:

```
wheat/mods/myMod/
  manifest.lua   -- recommended
  main.lua       -- required, runs once when the mod loads
  *.lua          -- anything main.lua loads with require("name")
  data/          -- created on demand, see `data`
```

## manifest.lua

```lua
return { name = "myMod", version = "1.0.0", apiVersion = 1, author = "you", description = "optional", repository = "https://github.com/you/myMod" }
```

`name` must equal the folder name. `apiVersion` must not be newer than the loader's. The file is evaluated with
an empty environment, so it can only return a table. A mod without a manifest loads with a warning.

For launcher update checks, optionally set `repository` to a GitHub repository URL. The launcher compares the manifest
version with the latest GitHub release tag. Mods hosted elsewhere can instead set `update_url` to an HTTPS JSON endpoint
that returns a `version` string and, optionally, a `release_url` string. Update checks only report the available version
and link; the launcher does not download or replace mod files.

## Events

```lua
events.on("frame", function(dt, frameNumber) end)   -- Windows: render frames; Linux: 10 Hz timer
events.on("mods_loaded", function() end)            -- once, after every mod has loaded
```

Only declared events can be subscribed to. At most 32 handlers per event.

## timers

```lua
local id = timers.after(2.5, fn)   -- once, in 2.5 seconds
local id = timers.every(1, fn)     -- repeating, minimum interval 0.05 s
timers.cancel(id)                  -- true if it existed
```

Timers run on frame boundaries. At most 64 per mod.

## data

Files private to the mod, stored in `<mod>/data/`.

```lua
data.write("settings.txt", text)   -- true, or nil + reason
data.read("settings.txt")          -- string, or nil if missing
data.remove("settings.txt")        -- true if it existed
data.list()                        -- sorted array of names
```

Names: letters, digits, `_`, `-`, at most one `.` extension of up to 8 characters, 64 characters in total. Limits: 1 MB per
file, 32 files, 4 MB in total, and bursts of up to 5 writes with a refill of 5 per second.

## console (read-only)

```lua
console.get("fps_max")        -- value, or nil + reason
console.default("fps_max")    -- the convar's default value
console.allowed("fps_max")    -- policy check only, never touches the engine
console.available()           -- true once the engine side has verified itself
```

Reasons: `not allowed`, `not found`, `unsupported type`, `console unavailable`, `rate limit exceeded`.
Values come back as a boolean, integer, float or string. There is deliberately no way to set a convar or run a
command. Only explicitly listed client settings can be read. The list covers frame rate and input settings,
crosshair, HUD, radar and viewmodel settings, plus selected audio and music settings found in the inspected
game dump. There are no prefix rules; debug, diagnostic and internal convars remain blocked even when their
names share a prefix with an allowed setting. Names must be lowercase letters, digits and `_`.

Engine access is off until you create `wheat/consoleApi.on` next to `winmm.dll`. At startup the loader checks
the engine's data layout against several known convars and switches the API off if anything does not match.
For the currently inspected game build, current numeric values are read at offset 88. Offset 96 can contain
default or cached values and must not be selected by comparing values against defaults.
It writes what it found to `wheat/consoleReport.txt`, line by line, before each engine call. Create
`wheat/dumpConvars` as well to get `wheat/convars.txt`, a list of every convar with its type and flags.
The native Linux preload runtime currently has no CS2 engine-console backend, so console functions report `console unavailable` there.

Installations can restrict the exposed functions with `wheat/apiAllowList.txt`, one entry per line. The launcher edits this list from an installation's **API Allow List** menu. Supported entries are `console.get`, `console.default`, `console.allowed`, `console.available`, and `data`. A listed console function is omitted from the Lua API when not allowed; omitting `data` removes the complete `data` table. If the file is absent, all currently implemented APIs retain their default behavior.

## wheat

```lua
wheat.api_version   -- 1
wheat.mod_name      -- the folder name
wheat.now()         -- monotonic seconds
```

## Everything else

`print` goes to `wheat/loader.log`. The standard `string`, `table`, `math`, `utf8` and `coroutine` libraries are
available. There is no `io`, `os`, `debug` or `package`, and `require` only loads files from the mod's own folder.

## Limits

Each event handler or timer callback gets 200k VM instructions and 20 ms. Each mod gets 16 MB of memory. Repeated
violations disable the mod until its files change. Editing any `.lua` file in a mod reloads it within a couple of
seconds. A reload resets the mod's Lua state, handlers and timers, and keeps its `data/` files. A failed reload leaves
the previous version running.
