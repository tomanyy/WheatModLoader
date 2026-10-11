print("hello from a mod, api " .. wheat.api_version)
events.on("frame", function(dt, frame)
  if frame % 300 == 0 then print("frame " .. frame .. ", dt=" .. dt) end
end)
