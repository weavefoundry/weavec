-- The Lua workload of the corpus gate's overhead benchmark (RFC 0030, gate
-- G14): table stores and loads, string building and pattern matching,
-- calls, hashing and sorting, sized to run for a second or two with a
-- clang -O2 build and to stay under 200 MiB. Deterministic: it prints one
-- line of results, which the gate compares between the two builds.

local t = {}
for i = 1, 4000000 do t[i] = i * 2 end
local sum = 0
for _ = 1, 32 do
  for i = 1, #t do sum = sum + t[i] end
end

local parts = {}
for i = 1, 300000 do parts[#parts + 1] = tostring(i) .. "x" end
local str = table.concat(parts)
local words = 0
for _ in string.gmatch(str, "%d+") do words = words + 1 end

local function fib (n)
  if n < 2 then return n end
  return fib(n - 1) + fib(n - 2)
end

local h = {}
for i = 1, 300000 do h["k" .. i] = i end
local hsum = 0
for _, v in pairs(h) do hsum = hsum + v end

local arr = {}
local x = 42
for i = 1, 300000 do
  x = (x * 1103515245 + 12345) % 2147483648
  arr[i] = x
end
table.sort(arr)

local up = 0
for i = 1, 200000 do
  local s = string.format("%d:%s", i, string.rep("ab", i % 7))
  up = up + #string.upper(s)
end

print(sum, words, fib(32), hsum, arr[1], arr[#arr], up)
