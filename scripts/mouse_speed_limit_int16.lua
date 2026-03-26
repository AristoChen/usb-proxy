-- mouse_speed_limit_int16.lua
-- Applies a dead zone and speed cap to USB HID mouse movement.
-- For mice using int16 little-endian axes with a leading Report ID byte.
--
-- 8-byte report layout (Report ID 1):
--   byte 1: Report ID (0x01)
--   byte 2: button bitmask
--   byte 3-4: X (int16 LE)
--   byte 5-6: Y (int16 LE)
--   byte 7: scroll wheel (int8)
--   byte 8: padding
--
-- Parameters (edit below):
--   DEAD_ZONE  – axis values within [-DEAD_ZONE, DEAD_ZONE] are zeroed out
--   MAX_SPEED  – axis values are clamped to [-MAX_SPEED, MAX_SPEED]
--   SCALE      – remaining movement is multiplied by this factor (0.0–1.0)
--
-- Usage in injection.json:
--   { "ep_address": 82, "enable": true, "script_file": "scripts/mouse_speed_limit_int16.lua" }

local DEAD_ZONE = 5
local MAX_SPEED = 100
local SCALE     = 0.5

local function process_axis(lo, hi)
    -- Reconstruct signed int16 from little-endian bytes
    local v = lo | (hi << 8)
    if v >= 32768 then v = v - 65536 end

    -- Dead zone
    if math.abs(v) <= DEAD_ZONE then return 0, 0 end

    -- Scale and clamp
    v = math.floor(v * SCALE + 0.5)
    if v >  MAX_SPEED then v =  MAX_SPEED end
    if v < -MAX_SPEED then v = -MAX_SPEED end

    -- Re-encode as little-endian unsigned
    if v < 0 then v = v + 65536 end
    return v & 0xFF, (v >> 8) & 0xFF
end

function transform(data, len)
    if len < 6 then return data, len end
    data[3], data[4] = process_axis(data[3], data[4])  -- X
    data[5], data[6] = process_axis(data[5], data[6])  -- Y
    return data, len
end
