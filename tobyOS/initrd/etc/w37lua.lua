-- W7f (docs/roadmap-2026-11.md): Lua's errors are longjmps (ldo.c's
-- LUAI_THROW, the C build): every error a pcall catches is a longjmp out of
-- the interpreter's frames back to luaD_rawrunprotected's setjmp. Run by
-- /bin/win-lua.exe; each line is checked against what Windows 11 prints
-- (measured: logs/w32/w7f-host-run.txt). Exits 39 iff all are the same, else
-- 100 + the first that is not.
local got = {}
local function out(...)
  local t = table.pack(...)
  for i = 1, t.n do t[i] = tostring(t[i]) end
  local line = table.concat(t, "\t")
  print(line)
  got[#got + 1] = line
end
local function strip(m) return (tostring(m):gsub("^.-:%d+: ", "")) end

local caught = 0
for i = 1, 2000 do
  local ok, e = pcall(error, "e" .. i)
  if not ok and e == "e" .. i then caught = caught + 1 end
end
out("caught", caught)

-- an error deep in nested calls, and one raised by the VM itself
local function deep(n) if n == 0 then error({ code = 42 }) end return deep(n - 1) + 1 end
local ok, e = pcall(deep, 150)
out("deep", ok, type(e), e.code)
ok, e = pcall(function() local t = nil; return t.x end)
out("vm", ok, strip(e))

-- pcall inside pcall, a message handler, coroutines
ok, e = pcall(function()
  local ok2, e2 = pcall(error, "inner")
  error("outer after " .. tostring(e2))
end)
out("nested", ok, strip(e))
out("xpcall", xpcall(function() error("x", 0) end, function(m) return "handled " .. m end))
local co = coroutine.create(function(a) local b = coroutine.yield(a + 1); error("in co " .. b, 0) end)
out("co1", coroutine.resume(co, 1))
out("co2", coroutine.resume(co, 7))
out("co3", coroutine.status(co))

-- an error with a level, and pcall(error) with nothing
out("level", pcall(function() error("lvl", 2) end))
out("count", select("#", pcall(error)))

local want = {
  "caught\t2000",
  "deep\tfalse\ttable\t42",
  "vm\tfalse\tattempt to index a nil value (local 't')",
  "nested\tfalse\touter after inner",
  "xpcall\tfalse\thandled x",
  "co1\ttrue\t2",
  "co2\tfalse\tin co 7",
  "co3\tdead",
  "level\tfalse\tlvl",
  "count\t2",
}
for i = 1, math.max(#want, #got) do
  if got[i] ~= want[i] then
    print("not as on Windows: line " .. i)
    os.exit(100 + i)
  end
end
print("w37lua: all as on Windows")
os.exit(39)
