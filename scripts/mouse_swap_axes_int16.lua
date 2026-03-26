-- mouse_swap_axes_int16.lua
-- Swaps the X and Y movement axes of a USB HID mouse.
-- For mice using int16 little-endian axes with a leading Report ID byte.
-- Useful for rotated displays or diagnosing axis assignment bugs.
--
-- 8-byte report layout (Report ID 1):
--   byte 1: Report ID (0x01)
--   byte 2: button bitmask
--   byte 3-4: X (int16 LE)
--   byte 5-6: Y (int16 LE)
--   byte 7: scroll wheel (int8)
--   byte 8: padding
--
-- Usage in injection.json:
--   { "ep_address": 82, "enable": true, "script_file": "scripts/mouse_swap_axes_int16.lua" }

function transform(data, len)
    if len < 6 then return data, len end
    -- Swap X (bytes 3,4) and Y (bytes 5,6) as a pair to preserve LE encoding
    data[3], data[5] = data[5], data[3]
    data[4], data[6] = data[6], data[4]
    return data, len
end
