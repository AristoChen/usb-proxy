-- mouse_swap_axes.lua
-- Swaps the X and Y movement axes of a USB HID mouse.
-- Useful for testing rotated displays or diagnosing axis assignment bugs.
--
-- 4-byte report layout:
--   byte 1: button bitmask (bit0=left, bit1=right, bit2=middle)
--   byte 2: X movement (int8, relative)
--   byte 3: Y movement (int8, relative)
--   byte 4: scroll wheel (int8, relative)
--
-- Usage in injection.json:
--   { "ep_address": 81, "enable": true, "script_file": "scripts/mouse_swap_axes.lua" }

function transform(data, len)
    if len < 3 then
        return data, len
    end

    -- Swap bytes 2 (X) and 3 (Y)
    data[2], data[3] = data[3], data[2]

    return data, len
end
