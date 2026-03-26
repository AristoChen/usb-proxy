-- mouse_invert.lua
-- Inverts both X and Y movement axes of a standard USB HID mouse.
--
-- Standard 4-byte mouse report layout (most mice):
--   byte 1: button bitmask (bit0=left, bit1=right, bit2=middle)
--   byte 2: X movement (int8, relative)
--   byte 3: Y movement (int8, relative)
--   byte 4: scroll wheel (int8, relative)
--
-- Usage in injection.json:
--   { "ep_address": 81, "enable": true, "script_file": "scripts/mouse_invert.lua" }

function transform(data, len)
    if len < 3 then
        return data, len
    end

    -- Negate X: interpret as int8 in uint8 space
    data[2] = (-data[2]) & 0xFF
    -- Negate Y
    data[3] = (-data[3]) & 0xFF

    return data, len
end
