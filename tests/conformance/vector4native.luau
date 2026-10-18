-- This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
print('testing four-wide native vectors')

assert(is_native())

local function checkVector(v, x, y, z, w)
    assert(is_native())
    assert(type(v) == 'vector' and typeof(v) == 'vector')
    assert(v.x == x and v.y == y and v.z == z and v.w == w)
end

local function checkQuaternion(q, x, y, z, w)
    assert(is_native())
    assert(type(q) == 'quaternion' and typeof(q) == 'quaternion')
    assert(q.x == x and q.y == y and q.z == z and q.w == w)
end

local function identity(value)
    assert(is_native())
    return value
end

local function constructors(x, y, z, w)
    assert(is_native())
    local a = vector.create(x, y, z, w)
    local b = vector.create(x, y, z)
    local c = vector.create(x, y)
    assert(is_native())
    checkVector(a, x, y, z, w)
    checkVector(b, x, y, z, 0)
    checkVector(c, x, y, 0, 0)
    assert(a ~= b)
end

constructors(1, -2, 3, 19)
constructors(-4, 5, -6, -23)

local function arithmetic(a: vector, b: vector)
    assert(is_native())
    checkVector(a + b, 3, 9, 18, 30)
    checkVector(a - b, -1, -3, -6, -10)
    checkVector(a * b, 2, 18, 72, 200)
    checkVector(a / b, 0.5, 0.5, 0.5, 0.5)
    checkVector(-a, -1, -3, -6, -10)
    checkVector(a * 2, 2, 6, 12, 20)
    checkVector(2 * a, 2, 6, 12, 20)
    checkVector(a / 2, 0.5, 1.5, 3, 5)
    checkVector(120 / b, 60, 20, 10, 6)
    assert(is_native())
end

arithmetic(vector.create(1, 3, 6, 10), vector.create(2, 6, 12, 20))

local function builtins(a: vector, b: vector)
    assert(is_native())
    assert(vector.dot(a, b) == 70)
    assert(vector.magnitude(a) == 5)
    local normalized = vector.normalize(a)
    assert(math.abs(normalized.x - 0.6) < 1e-6)
    assert(normalized.y == 0 and normalized.z == 0)
    assert(math.abs(normalized.w - 0.8) < 1e-6)
    checkVector(vector.normalize(vector.create(0, 0, 0, 7)), 0, 0, 0, 1)
    checkVector(vector.normalize(vector.create(0, 0, 0, 0)), 0, 0, 0, 0)
    checkVector(vector.cross(a, b), 0, -27, 21, 0)
    checkVector(vector.cross(b, a), 0, 27, -21, 0)
    assert(is_native())
end

builtins(vector.create(3, 0, 0, 4), vector.create(6, 7, 9, 13))

local function tableCopies(a, b, q, qx, qy, qz, qw)
    assert(is_native())
    local marker = {}
    local callback = identity
    local thread = coroutine.create(function() end)
    local bytes = buffer.create(4)
    local t = {a, false, q, b, marker, 'value', 37, callback, thread, bytes}
    t.first = a
    t.second = b
    t.rotation = q
    t.flag = false
    t[false] = b
    t[marker] = q
    t[a] = b
    local otherKey = vector.create(1, -2, 3, 20)
    t[otherKey] = a
    for i = 1, 32 do
        assert(is_native())
        t['field' .. i] = if i % 2 == 0 then a else b
    end
    for i = 1, 32 do
        assert(is_native())
        local value = t['field' .. i]
        if i % 2 == 0 then
            checkVector(value, 1, -2, 3, 19)
        else
            checkVector(value, -4, 5, -6, -23)
        end
    end
    local index = identity(1)
    local key = identity('first')
    checkVector(t[index], 1, -2, 3, 19)
    checkVector(t.first, 1, -2, 3, 19)
    checkVector(t[key], 1, -2, 3, 19)
    checkVector(t[4], -4, 5, -6, -23)
    checkVector(t.second, -4, 5, -6, -23)
    checkVector(t[false], -4, 5, -6, -23)
    checkVector(t[a], -4, 5, -6, -23)
    checkVector(t[otherKey], 1, -2, 3, 19)
    checkQuaternion(t[3], qx, qy, qz, qw)
    checkQuaternion(t.rotation, qx, qy, qz, qw)
    checkQuaternion(t[marker], qx, qy, qz, qw)
    assert(t[2] == false and t.flag == false and t.missing == nil)
    assert(t[5] == marker and t[6] == 'value' and t[7] == 37)
    assert(t[8] == callback and t[9] == thread and t[10] == bytes)
    t[1], t.first = b, b
    t[4], t.second = a, a
    checkVector(t[1], -4, 5, -6, -23)
    checkVector(t.first, -4, 5, -6, -23)
    checkVector(t[4], 1, -2, 3, 19)
    checkVector(t.second, 1, -2, 3, 19)
    t[3], t.rotation = nil, nil
    assert(t[3] == nil and t.rotation == nil)
    assert(is_native())
end

local a = vector.create(1, -2, 3, 19)
local b = vector.create(-4, 5, -6, -23)
local q = quaternion.pack(1, -2, 3, -4)
local qx, qy, qz, qw = quaternion.unpack(q)
assert(qx ~= 0 and qy ~= 0 and qz ~= 0 and qw ~= 0)
checkQuaternion(q, qx, qy, qz, qw)
tableCopies(a, b, q, qx, qy, qz, qw)

local function upvalues(value)
    assert(is_native())
    local captured = value
    local function get()
        assert(is_native())
        return captured
    end
    local function set(nextValue)
        assert(is_native())
        local previous = captured
        captured = nextValue
        return previous
    end
    checkVector(get(), 1, -2, 3, 19)
    checkVector(set(b), 1, -2, 3, 19)
    checkVector(get(), -4, 5, -6, -23)
    checkVector(set(q), -4, 5, -6, -23)
    checkQuaternion(get(), qx, qy, qz, qw)
    checkQuaternion(set(false), qx, qy, qz, qw)
    assert(get() == false)
    assert(set(nil) == false and get() == nil)
    assert(set(a) == nil)
    return get, set
end

local get, set = upvalues(a)
checkVector(get(), 1, -2, 3, 19)
checkVector(set(b), 1, -2, 3, 19)
checkVector(get(), -4, 5, -6, -23)
checkVector(set(q), -4, 5, -6, -23)
checkQuaternion(get(), qx, qy, qz, qw)
checkQuaternion(set(false), qx, qy, qz, qw)
assert(get() == false and set(nil) == false and get() == nil)

local function arguments(first, second, third, fourth, fifth, sixth)
    assert(is_native())
    checkVector(first, 1, -2, 3, 19)
    assert(second == false and third == nil)
    checkQuaternion(fourth, qx, qy, qz, qw)
    checkVector(fifth, -4, 5, -6, -23)
    assert(sixth == 'tail')
    return fifth, fourth, third, second, first, sixth, nil
end

local function varargs(...)
    assert(is_native())
    assert(select('#', ...) == 7)
    local first, second, third, fourth, fifth, sixth, seventh = ...
    checkVector(first, -4, 5, -6, -23)
    checkQuaternion(second, qx, qy, qz, qw)
    assert(third == nil and fourth == false)
    checkVector(fifth, 1, -2, 3, 19)
    assert(sixth == 'tail' and seventh == nil)
    return ...
end

local function forward(...)
    assert(is_native())
    return varargs(...)
end

local ra, rq, rn, rf, rb, rs, re = forward(arguments(a, false, nil, q, b, 'tail'))
checkVector(ra, -4, 5, -6, -23)
checkQuaternion(rq, qx, qy, qz, qw)
assert(rn == nil and rf == false and rs == 'tail' and re == nil)
checkVector(rb, 1, -2, 3, 19)
assert(select('#', forward(arguments(a, false, nil, q, b, 'tail'))) == 7)
local packed = table.pack(forward(arguments(a, false, nil, q, b, 'tail')))
assert(packed.n == 7 and packed[3] == nil and packed[4] == false and packed[6] == 'tail' and packed[7] == nil)
checkVector(packed[1], -4, 5, -6, -23)
checkQuaternion(packed[2], qx, qy, qz, qw)
checkVector(packed[5], 1, -2, 3, 19)

local function checkValue(value, expected)
    assert(is_native())
    local expectedType = type(expected)
    assert(type(value) == expectedType)
    if expectedType == 'vector' then
        checkVector(value, expected.x, expected.y, expected.z, expected.w)
    elseif expectedType == 'quaternion' then
        checkQuaternion(value, qx, qy, qz, qw)
    else
        assert(value == expected)
    end
end

local function logical(left, right)
    assert(is_native())
    return left and right, left or right
end

local marker = {}
local values = {a, b, q, false, true, 0, 'text', marker, identity, coroutine.create(function() end), buffer.create(1)}
for i = 1, #values + 1 do
    assert(is_native())
    local left = values[i]
    for j = 1, #values + 1 do
        assert(is_native())
        local right = values[j]
        local both, either = logical(left, right)
        if left == nil or left == false then
            checkValue(both, left)
            checkValue(either, right)
        else
            checkValue(both, right)
            checkValue(either, left)
        end
    end
end

local function iterate(first, second, rotation)
    assert(is_native())
    local sequence = {first, false, rotation, second, 'tail'}
    local count = 0
    for index, value in ipairs(sequence) do
        assert(is_native())
        count += 1
        assert(index == count)
        if index == 1 then checkVector(value, 1, -2, 3, 19)
        elseif index == 2 then assert(value == false)
        elseif index == 3 then checkQuaternion(value, qx, qy, qz, qw)
        elseif index == 4 then checkVector(value, -4, 5, -6, -23)
        else assert(value == 'tail') end
    end
    assert(count == 5)
    local mixed = {first, false, second, rotation = rotation, tail = 'tail'}
    local seen = {}
    count = 0
    for key, value in pairs(mixed) do
        assert(is_native())
        count += 1
        seen[key] = true
        if key == 1 then checkVector(value, 1, -2, 3, 19)
        elseif key == 2 then assert(value == false)
        elseif key == 3 then checkVector(value, -4, 5, -6, -23)
        elseif key == 'rotation' then checkQuaternion(value, qx, qy, qz, qw)
        else assert(key == 'tail' and value == 'tail') end
    end
    assert(count == 5 and seen[1] and seen[2] and seen[3] and seen.rotation and seen.tail)
    count = 0
    for key, value in mixed do
        assert(is_native())
        checkValue(value, mixed[key])
        count += 1
    end
    assert(count == 5)
end

iterate(a, b, q)

local function quaternionFallback(rotation, identityRotation, v)
    assert(is_native())
    assert(type(rotation) == 'quaternion' and rotation ~= v)
    assert(identityRotation == quaternion.pack(0, 0, 0, 1))
    assert(quaternion.pack(0, 0, 0, 1) == identityRotation)
    assert(rotation ~= quaternion.pack(0, 0, 0, 1))
    assert(quaternion.pack(0, 0, 0, 1) ~= rotation)
    if rotation == quaternion.pack(0, 0, 0, 1) then
        error('nonidentity rotation compares equal to identity')
    end
    if quaternion.pack(0, 0, 0, 1) ~= identityRotation then
        error('identity rotation compares unequal to identity')
    end
    checkQuaternion(identity(rotation), qx, qy, qz, qw)
    assert(rotation.X == qx and rotation.Y == qy and rotation.Z == qz and rotation.W == qw)
    checkQuaternion(rotation * identityRotation, qx, qy, qz, qw)
    checkQuaternion(identityRotation * rotation, qx, qy, qz, qw)
    checkVector(identityRotation * v, 1, -2, 3, 0)
    local conjugate = quaternion.conjugate(rotation)
    checkQuaternion(conjugate, -qx, -qy, -qz, qw)
    local x, y, z, w = quaternion.unpack(identity(rotation))
    assert(x == qx and y == qy and z == qz and w == qw)
    local t = {rotation, result = conjugate}
    checkQuaternion(t[1], qx, qy, qz, qw)
    checkQuaternion(t.result, -qx, -qy, -qz, qw)
end

quaternionFallback(q, quaternion.pack(0, 0, 0, 1), a)

local function spills(first: vector, second: vector)
    local v0, v1, v2, v3 = first, first + second, first + second * 2, first + second * 3
    local v4, v5, v6, v7 = first + second * 4, first + second * 5, first + second * 6, first + second * 7
    local v8, v9, v10, v11 = first + second * 8, first + second * 9, first + second * 10, first + second * 11
    local v12, v13, v14, v15 = first + second * 12, first + second * 13, first + second * 14, first + second * 15
    local v16, v17, v18, v19 = first + second * 16, first + second * 17, first + second * 18, first + second * 19
    assert(is_native())
    return v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7 + v8 + v9 + v10 + v11 + v12 + v13 + v14 + v15 + v16 + v17 + v18 + v19
end

checkVector(spills(a, b), -740, 910, -1080, -3990)
assert(is_native())
return 'OK'
