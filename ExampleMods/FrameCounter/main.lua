local last_reported_frame = 0

events.on("frame", function(delta_seconds, frame_number)
    if frame_number - last_reported_frame >= 600 then
        print(string.format(
            "Frame %d; last frame took %.4f seconds",
            frame_number,
            delta_seconds
        ))
        last_reported_frame = frame_number
    end
end)
