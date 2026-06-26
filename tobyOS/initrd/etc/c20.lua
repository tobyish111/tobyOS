-- C20 proof script for the REAL Lua 5.4 interpreter (win-lua.exe).
-- Exercises the math library -- math.sqrt/sin/exp/log/pi -- which calls the
-- ucrt libm; every one of those returns a `double` in xmm0 and so only works
-- through C20's float-return marshalling gate + the kernel kmath libm. Each
-- result is reduced to an EXACT integer with math.floor(x*1e6 + 0.5) so the
-- sentinel avoids float *printing* (a separate gap) while still proving real
-- floating-point computation round-tripped through the gate. The kernel
-- re-reads the output file to confirm the interpreter genuinely ran this.

local function scaled(x) return math.floor(x * 1e6 + 0.5) end

local sqrt2 = scaled(math.sqrt(2.0))            -- 1.4142135624e6 -> 1414214
local sin30 = scaled(math.sin(math.pi / 6.0))   -- 0.5            -> 500000
local e1    = scaled(math.exp(1.0))             -- 2.7182818285e6 -> 2718282
local lne   = scaled(math.log(math.exp(1.0)))   -- 1.0            -> 1000000
local pival = scaled(math.pi)                   -- 3.1415926536e6 -> 3141593

local line = string.format(
  "TOBYMATH=== sqrt2=%d sin30=%d e=%d lne=%d pi=%d",
  sqrt2, sin30, e1, lne, pival)

local f = assert(io.open("C:\\c20.out", "w"))
f:write(line, "\n")
f:close()

print(line)
os.exit(0)
