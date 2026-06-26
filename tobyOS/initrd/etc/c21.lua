-- C21 capstone for the REAL Lua 5.4 interpreter (win-lua.exe): print computed
-- FLOATS as decimal text. C20 proved Lua could *compute* with doubles (results
-- were reduced to integers to dodge float printing); this prints them directly.
--
-- NOTE on scope: win-lua is a mingw build, so its string.format/tostring render
-- floats with libmingwex IN-PROCESS (not the kernel win32_vformat engine that
-- C21 teaches to format doubles -- that path is proven by win-printf21.exe).
-- This script is the real-application capstone: a genuine interpreter computing
-- real doubles (sqrt/division, some via the C20 gate) and emitting correct
-- decimal strings end-to-end. The kernel re-reads the file to confirm it ran.

local quarter = string.format("%.3f", 1.0 / 4.0)        -- 0.250
local tenth   = string.format("%g",   0.1)              -- 0.1
local third   = string.format("%.14g", 10.0 / 3.0)      -- 3.3333333333333
local root2   = string.format("%.6f", math.sqrt(2.0))   -- 1.414214

local line = string.format(
  "C21LUA=== quarter=%s tenth=%s third=%s root2=%s",
  quarter, tenth, third, root2)

local f = assert(io.open("C:\\c21l.out", "w"))
f:write(line, "\n")
f:close()

print(line)
os.exit(0)
