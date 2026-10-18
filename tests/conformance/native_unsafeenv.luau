-- This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details

local native = is_native_if_supported
local raise = error
local protect = pcall
local pack = table.pack
local realAssert = assert
local realType = type
local realMath = math
local makeMetatable = setmetatable
local opaque = opaque_functions
local env = make_unsafe_env(_G)

local function check(value, message)
    if not value then
        raise(message or "unsafe environment check failed")
    end
end

local function discardAssert(value)
    check(native(), "assert entry")
    assert(value)
    check(native(), "after discarded assert")
    return value + 1
end

local function callAssert(value)
    check(native(), "assert result entry")
    local result = assert(value, "message")
    check(native(), "after assert result")
    return result
end

local function multiAssert(value)
    check(native(), "assert multret entry")
    local result = pack(assert(value, "message"))
    check(native(), "after assert multret")
    return result
end

local function callAbs(value)
    check(native(), "math entry")
    local result = math.abs(value)
    check(native(), "after math.abs")
    return result
end

local function callType(value)
    check(native(), "type entry")
    local result = type(value)
    check(native(), "after type")
    return result
end

local effects = ""
local function argument(value)
    effects ..= value
    return value
end

local function callMax()
    check(native(), "arguments entry")
    local result = math.max(argument(1), argument(2), argument(3), argument(4))
    check(native(), "after math.max")
    return result
end

local function callTwo(a, b)
    check(native(), "two arguments entry")
    local result = math.max(a, b)
    check(native(), "after two arguments")
    return result
end

local function callThree(a, b, c)
    check(native(), "three arguments entry")
    local result = math.max(a, b, c)
    check(native(), "after three arguments")
    return result
end

local function readImport()
    check(native(), "import entry")
    local result = unsafe_import_value.branch.value
    check(native(), "after import")
    return result
end

-- Indirect calls prevent O2 from folding builtin calls with literal arguments.
local calls = opaque({
    discardAssert = discardAssert,
    assert = callAssert,
    multiAssert = multiAssert,
    abs = callAbs,
    type = callType,
    max = callMax,
    two = callTwo,
    three = callThree,
    import = readImport,
})

local cases = {
    function()
        check(calls.discardAssert(4) == 5, "discarded assert value")
        check(calls.assert(4) == 4, "assert value")
        check(calls.abs(-7) == 7, "abs value")
        check(calls.type(4) == "number", "type value")
    end,
    function()
        env.unsafe_import_value = { branch = { value = 41 } }
        check(calls.import() == 41, "initial import")
        env.unsafe_import_value = { branch = { value = 42 } }
        check(calls.import() == 42, "replaced import")
        env.unsafe_import_value = makeMetatable({}, {
            __index = function(_, key)
                check(key == "branch", "import metamethod key")
                return { value = 43 }
            end,
        })
        check(calls.import() == 43, "import metamethod value")
        env.unsafe_import_value = nil
        local ok, message = protect(calls.import)
        check(not ok and message:find("attempt to index", 1, true), "import error")
        env.unsafe_import_value = makeMetatable({}, {
            __index = function()
                raise("import metamethod sentinel")
            end,
        })
        ok, message = protect(calls.import)
        check(not ok and message:find("import metamethod sentinel", 1, true), "import metamethod error")
        env.unsafe_import_value = { branch = { value = 44 } }
        check(calls.import() == 44, "import recovery")
    end,
    function()
        local count = 0
        env.assert = function(value, message)
            count += 1
            check(message == "message", "assert message argument")
            return value + 10, nil, "tail"
        end
        check(calls.assert(4) == 14, "Lua assert override")
        local result = calls.multiAssert(4)
        check(result.n == 3 and result[1] == 14 and result[2] == nil and result[3] == "tail", "assert multret")
        check(count == 2, "assert override count")

        env.assert = realType
        check(calls.assert(4) == "number", "C assert override")
        result = calls.multiAssert(4)
        check(result.n == 1 and result[1] == "number", "C assert multret")
        env.assert = function() end
        result = calls.multiAssert(4)
        check(result.n == 0, "assert zero results")
        env.assert = realAssert
    end,
    function()
        env.math = {
            abs = function(value)
                return value * 3
            end,
            max = function(...)
                local values = pack(...)
                check(values.n == 4, "max argument count")
                check(effects == "1234", "max argument order")
                return values[1] + values[2] + values[3] + values[4]
            end,
        }
        check(calls.abs(-7) == -21, "Lua math override")
        check(calls.max() == 10, "four arguments")
        check(effects == "1234", "argument side effects")

        env.math.max = function(a, b, c)
            return a * 100 + b * 10 + (c or 0)
        end
        check(calls.two(1, 2) == 120, "two arguments")
        check(calls.three(1, 2, 3) == 123, "three arguments")
        env.math.abs = realMath.floor
        check(calls.abs(-1.5) == -2, "C math override")
        env.math = realMath
    end,
    function()
        env.type = function(value)
            return value + 1
        end
        check(calls.type(4) == 5, "Lua type override")
        env.type = realMath.abs
        check(calls.type(-4) == 4, "C type override")
        env.type = realType
    end,
    function()
        local lookups = 0
        env.math = makeMetatable({}, {
            __index = function(_, key)
                check(key == "abs", "math metamethod key")
                lookups += 1
                return function(value)
                    return value + 20
                end
            end,
        })
        check(calls.abs(4) == 24, "math metamethod value")
        check(lookups == 1, "math lookup count")
        env.math = realMath
    end,
    function()
        env.assert = function()
            raise("overridden assert error")
        end
        local ok, message = protect(calls.assert, 4)
        check(not ok and message:find("overridden assert error", 1, true), "Lua override error")
        env.assert = false
        ok, message = protect(calls.assert, 4)
        check(not ok and message:find("attempt to call", 1, true), "noncallable override")
        env.assert = realAssert

        ok, message = protect(calls.abs, {})
        check(not ok and message:find("number expected", 1, true), "C builtin error")
        check(calls.assert(4) == 4, "restored assert")
    end,
}

for _, case in cases do
    case()
end

return "OK"
