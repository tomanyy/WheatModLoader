# Mod API (version 1)

A mod is a folder in `wheat/mods/`:

```
wheat/mods/myMod/
  manifest.json  -- recommended
  main.lua       -- required, runs once when the mod loads
  *.lua          -- anything main.lua loads with require("name")
  data/          -- created on demand, see `data`
```

## manifest.json

```json
{
  "name": "myMod",
  "version": "1.0.0",
  "apiVersion": 1,
  "author": "you",
  "description": "A short description of the mod",
  "repository": "https://github.com/you/myMod"
}
```

`name` is the display name and does not need to match the folder. The folder name is the internal mod ID and must use only letters, digits, `_`, or `-` (up to 64 characters). `version` must be a string and `apiVersion` must be an integer no newer than the loader's. Optional fields include `author`, `description`, `repository`, and `update_url`. A mod without a manifest loads with a warning and uses its folder name as the display name.

The launcher displays `name`, `version`, `author`, and `description` from this table. To show an icon in **Manage Mods**, place `icon.png`, `icon.jpg`, `icon.jpeg`, or `icon.gif` directly inside the mod folder.

For launcher update checks, optionally set `repository` to a GitHub repository URL. The launcher compares the manifest
version with the latest GitHub release tag. Mods hosted elsewhere can instead set `update_url` to an HTTPS JSON endpoint
that returns a `version` string and, optionally, a `release_url` string. Update checks only report the available version
and link; the launcher does not download or replace mod files.

## Events

```lua
events.on("frame", function(dt, frameNumber) end)   -- Windows: render frames; Linux: Vulkan present frames with a 10 Hz fallback
events.on("mods_loaded", function() end)            -- once, after every mod has loaded
```

Only declared events can be subscribed to. At most 32 handlers per event.

## timers

```lua
local id = timers.after(2.5, fn)   -- once, in 2.5 seconds
local id = timers.every(1, fn)     -- repeating, minimum interval 0.05 s
timers.cancel(id)                  -- true if it existed
```

Timers run on frame boundaries. At most 64 per mod. `wait(seconds)` can suspend the top-level `main.lua` coroutine;
it resumes on a later frame. It is not available inside event handlers or timer callbacks.

```lua
while true do
    print("fps_max:", console.get("fps_max"))
    wait(1)
end
```

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

When `wheat/apiAllowList.txt` explicitly allows at least one `console.*` method, the loader initializes engine access; no separate `consoleApi.on` file is needed. At startup the loader checks
the engine's data layout against several known convars and switches the API off if anything does not match.
For the currently inspected game build, current numeric values are read at offset 88. Offset 96 can contain
default or cached values and should not be used as the current value slot.
It writes what it found to `wheat/consoleReport.txt`, line by line, before each engine call. Create
`wheat/dumpConvars` as well to get `wheat/convars.txt`, a list of every convar with its type and flags.
On native Linux, the loader looks for `VEngineCvar007` in the loaded `libengine2.so`, checks that the expected virtual methods live in CS2 engine modules, and calibrates the convar record layout against known names before enabling reads. Any failed check leaves the API unavailable. The Linux result is written to `wheat/consoleReport.txt`.

## imgui (Linux, experimental)

With the installation's experimental Linux Vulkan layer enabled, a mod can draw an overlay from its `frame` event:

```lua
events.on("frame", function()
    imgui.begin("My mod")
    imgui.text("Hello from Wheat")
    if imgui.button("Click me") then
        print("button clicked")
    end
    imgui.finish()
end)
```

`imgui.begin(title)` defaults to the mod's display name. The API also provides `imgui.separator()` and
`imgui.same_line()`. SDL2 events feed ImGui input, and mouse or keyboard events are withheld from CS2 while ImGui
requests capture. The Vulkan layer draws only when the present queue supports graphics and a compatible swapchain is
available. Without the layer or an available rendering context, drawing calls have no visible effect. Each function
has its own API Allow List entry under the `imgui` category.

Installations can restrict the exposed functions with `wheat/apiAllowList.txt`, one entry per line. The launcher edits this list from an installation's **API Allow List** menu. Supported entries are the four `console.*` functions, `data`, and the six `imgui.*` functions above. A listed console function is omitted from the Lua API when not allowed; omitting `data` removes the complete `data` table. If the file is absent, API functions retain their default exposure, but the engine console backend stays off until the launcher saves an allow list containing at least one `console.*` entry.

## wheat

```lua
wheat.api_version   -- 1
wheat.mod_name      -- the internal folder ID
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
