-- Runs a named subset of Lua's own test suite (testes/) for the corpus gate
-- (test/corpus/manifest.json, config "lua"). Invoked from the testes
-- directory as
--
--   ../lua subset.lua gc.lua db.lua calls.lua ...
--
-- It sets up what testes/all.lua sets up in user mode (_U): no internal T
-- tests (they need a build with ltests.h), no long or non-portable tests, no
-- messages about skipped tests. Each named file runs the way all.lua runs
-- it: through string.dump/load (stripped where all.lua strips) except for
-- the files all.lua loads directly, with all.lua's return-value checks.
-- all.lua itself is not used because files.lua needs /dev/full, which macOS
-- lacks; the manifest names the files instead.

_G.ARG = arg      -- main.lua finds the interpreter through ARG
local files = arg -- test files may clear the global 'arg'

_soft = true      -- avoid long or memory-consuming tests
_port = true      -- avoid non-portable tests
_nomsg = true     -- no messages about tests not performed
T = nil           -- no internal tests (they need a build with ltests.h)
debug = nil       -- test files require 'debug' when they need it

function Message (m) end   -- _nomsg: skipped tests are not reported

assert(os.setlocale"C")

-- Files all.lua runs without the dump/undump round trip.
local direct = {["gc.lua"] = true, ["strings.lua"] = true, ["literals.lua"] = true}
-- Files all.lua dumps with debug information stripped.
local strip = {["code.lua"] = true, ["goto.lua"] = true, ["sort.lua"] = true,
               ["verybig.lua"] = true}
-- Return values all.lua checks.
local returns = {["attrib.lua"] = 27, ["locals.lua"] = 5, ["events.lua"] = 12,
                 ["verybig.lua"] = 10}

local function run (name)
  print("\n***** FILE '" .. name .. "'*****")
  local f = assert(loadfile(name))
  if not direct[name] then
    f = assert(load(string.dump(f, strip[name])))
  end
  return f()
end

assert(#files > 0, "usage: lua subset.lua file.lua ...")
for i = 1, #files do
  local name = files[i]
  if name == "gc.lua" then
    require"tracegc".start()   -- all.lua traces collections from gc.lua on
  end
  local result = run(name)
  if name == "calls.lua" then
    assert(result == deep and deep)
    _G.deep = nil
  elseif returns[name] then
    assert(result == returns[name], name .. " returned an unexpected value")
  end
  if name == "verybig.lua" then collectgarbage() end
end

assert(debug == nil, "no test module should define 'debug'")
print("final OK !!!")
