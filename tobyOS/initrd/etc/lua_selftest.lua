-- lua_selftest.lua -- exercises the tobyOS Lua interpreter for the boot
-- harness (LUATEST_BOOT). Prints LUATEST sentinels; the kernel harness
-- greps for "LUATEST: ALL OK".
--
-- NOTE: this interpreter has no closures/upvalues, so `fails` is a GLOBAL
-- (a `local` here would not be visible inside check()).

fails = 0
function check(cond, name)
  if cond then
    print("LUATEST: " .. name .. " OK")
  else
    print("LUATEST: " .. name .. " FAIL")
    fails = fails + 1
  end
end

-- core language: functions, arithmetic
function add(a, b) return a + b end
check(add(2, 3) == 5, "function-call")

-- numeric for
local sum = 0
for i = 1, 10 do sum = sum + i end
print("LUATEST: (sum=" .. sum .. ")")
check(sum == 55, "numeric-for")

-- tables, incl. indexed assignment t[k]=v
local t = {}
t.x = 7
t[1] = "a"
check(t.x == 7 and t[1] == "a", "tables")

-- string library
check(string.sub("hello", 2, 4) == "ell", "string.sub")
check(string.len("hello") == 5, "string.len")

local fmt = string.format("%d-%s-%.2f", 42, "hi", 3.14159)
print("LUATEST: (fmt=[" .. fmt .. "])")
check(fmt == "42-hi-3.14", "string.format")
check(string.rep("ab", 3) == "ababab", "string.rep")
check(string.upper("Toby") == "TOBY", "string.upper")
check(string.lower("Toby") == "toby", "string.lower")
check(math.max(3, 9, 5) == 9, "math.max")
check(math.min(3, 9, 5) == 3, "math.min")
check(math.ceil(3.2) == 4, "math.ceil")

if fails == 0 then
  print("LUATEST: ALL OK")
else
  print("LUATEST: " .. fails .. " FAILED")
end
