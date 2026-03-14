-- mouse_speed_limit.lua
-- Applies a dead zone and speed cap to USB HID mouse movement.
-- Useful for slowing down high-DPI mice or testing cursor control.
--
-- 4-byte report layout:
--   byte 1: button bitmask (bit0=left, bit1=right, bit2=middle)
--   byte 2: X movement (int8, relative)
--   byte 3: Y movement (int8, relative)
--   byte 4: scroll wheel (int8, relative)
--
-- Parameters (edit below):
--   DEAD_ZONE  – axis values within [-DEAD_ZONE, DEAD_ZONE] are zeroed out
--   MAX_SPEED  – axis values are clamped to [-MAX_SPEED, MAX_SPEED]
--   SCALE      – remaining movement is multiplied by this factor (0.0–1.0)
--
-- Usage in injection.json:
--   { "ep_address": 81, "enable": true, "script_file": "scripts/mouse_speed_limit.lua" }

local DEAD_ZONE = 2
local MAX_SPEED = 20
local SCALE     = 0.5

local function process_axis(raw)
    -- Convert uint8 → int8
    local v = (raw > 127) and (raw - 256) or raw

    -- Dead zone
    if math.abs(v) <= DEAD_ZONE then
        return 0
    end

    -- Scale and clamp
    v = math.floor(v * SCALE + 0.5)
    if v >  MAX_SPEED then v =  MAX_SPEED end
    if v < -MAX_SPEED then v = -MAX_SPEED end

    -- Convert int8 → uint8
    return v & 0xFF
end

function transform(data, len)
    if len < 3 then
        return data, len
    end

    data[2] = process_axis(data[2])  -- X
    data[3] = process_axis(data[3])  -- Y

    return data, len
end
