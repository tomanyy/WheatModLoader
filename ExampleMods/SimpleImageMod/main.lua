local clicks = 0

events.on("frame", function()
    imgui.begin("Simple Image Mod")
    imgui.text("Hello from Wheat!")
    if imgui.image then
        imgui.image("assets/wheat.png", 96, 96)
    else
        imgui.text("Allow imgui.image in this installation's API Allow List to show the picture.")
    end
    imgui.separator()
    if imgui.button("Click me") then
        clicks = clicks + 1
    end
    imgui.same_line()
    imgui.text("Clicks: " .. tostring(clicks))
    imgui.finish()
end)
