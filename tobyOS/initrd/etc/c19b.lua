-- C19b proof script for the REAL Lua 5.4 interpreter (win-lua.exe).
-- Deliberately integer/string/table only (no floats) so the result is exact
-- and avoids the integer-gate float-return limit. The kernel re-reads the
-- output file to confirm the interpreter genuinely executed this program.

local function fib(n)
  local a, b = 0, 1
  for _ = 1, n do a, b = b, a + b end
  return a
end

-- integer arithmetic + a table of squares
local sum = 0
local sq = {}
for i = 1, 10 do
  sq[i] = i * i
  sum = sum + sq[i]            -- 1+4+9+...+100 = 385
end

-- table iteration + string building
local parts = {}
for _, v in ipairs(sq) do
  parts[#parts + 1] = tostring(v)
end
local joined = table.concat(parts, ",")   -- "1,4,9,16,25,36,49,64,81,100"

-- string library: case, repetition, pattern count
local tag = string.upper("tobylua")        -- "TOBYLUA"
local bars = string.rep("=", 3)            -- "==="
local _, words = string.gsub("the quick brown fox", "%a+", "")  -- 4

local line = string.format("%s%s sum=%d fib25=%d words=%d sq=%s",
                           tag, bars, sum, fib(25), words, joined)

local f = assert(io.open("C:\\c19b.out", "w"))
f:write(line, "\n")
f:close()

print(line)
os.exit(0)
