local started_at

events.on("mods_loaded", function()
    started_at = wheat.now()
    print("Timer started")
end)

events.on("frame", function()
    if started_at then
        local elapsed = wheat.now() - started_at
        if elapsed >= 30 then
            print(string.format("This mod has been active for %.1f seconds", elapsed))
            started_at = wheat.now()
        end
    end
end)
