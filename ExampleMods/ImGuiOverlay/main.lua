local clicks = 0

events.on("frame", function()
    imgui.begin("Wheat ImGui Example")
    imgui.text("Linux overlay is running")
    imgui.separator()
    if imgui.button("Click me") then
        clicks = clicks + 1
    end
    imgui.same_line()
    imgui.text("Clicks: " .. tostring(clicks))
    imgui.finish()
end)
