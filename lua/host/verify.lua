print("6*7 =", 6*7)

-- integer / float type checks (LUA_32BITS: integer=int32, float=float32)
print("type of 6*7:", math.type(6*7))
print("type of 6/7:", math.type(6/7))
print("maxinteger:", math.maxinteger)
print("mininteger:", math.mininteger)

-- loop
local sum = 0
for i = 1, 100 do
  sum = sum + i
end
print("sum 1..100 =", sum)

-- fibonacci (recursion, function calls, integer math)
local function fib(n)
  if n < 2 then return n end
  return fib(n-1) + fib(n-2)
end
print("fib(20) =", fib(20))

-- string library
local s = "Hello, AmigaOS Lua!"
print("upper:", s:upper())
print("sub:", s:sub(1, 5))
print("format:", string.format("%d-%s-%5.2f", 42, "amiga", 3.14159))
print("find:", s:find("Amiga"))
print("gsub:", s:gsub("o", "0"))
print("rep:", string.rep("ab", 3))

-- table library
local t = {5, 3, 1, 4, 2}
table.sort(t)
print("sorted:", table.concat(t, ","))
table.insert(t, 99)
print("after insert:", table.concat(t, ","))
table.remove(t, 1)
print("after remove:", table.concat(t, ","))

-- math library
print("math.floor(3.7) =", math.floor(3.7))
print("math.sqrt(2) =", math.sqrt(2))
print("math.abs(-5) =", math.abs(-5))
print("math.log(8,2) =", math.log(8,2))
print("math.pi =", math.pi)

-- closures / metatables
local mt = {__add = function(a,b) return {v = a.v + b.v} end}
local a = setmetatable({v=10}, mt)
local b = setmetatable({v=20}, mt)
local c = a + b
print("metatable add:", c.v)

-- coroutines
local co = coroutine.create(function(x)
  for i=1,3 do
    x = coroutine.yield(x+i)
  end
  return "done", x
end)
print(coroutine.resume(co, 1))
print(coroutine.resume(co, 10))
print(coroutine.resume(co, 100))
print(coroutine.resume(co, 1000))

print("ALL TESTS COMPLETED OK")
