// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include "Luau/AssemblyBuilderA64.h"

#include "EmitCommon.h"

#include "lobject.h"
#include "ltm.h"
#include "lstate.h"

// AArch64 ABI reminder:
// Arguments: x0-x7, v0-v7
// Return: x0, v0 (or x8 that points to the address of the resulting structure)
// Volatile: x9-x15, v16-v31 ("caller-saved", any call may change them)
// Intra-procedure-call temporary: x16-x17 (any call or relocated jump may change them, as linker may point branches to veneers to perform long jumps)
// Non-volatile: x19-x28, v8-v15 ("callee-saved", preserved after calls, only bottom half of SIMD registers is preserved!)
// Reserved: x18: reserved for platform use; x29: frame pointer (unless omitted); x30: link register; x31: stack pointer

namespace Luau
{
namespace CodeGen
{

namespace A64
{

// Data that is very common to access is placed in non-volatile registers:
// 1. Constant registers (only loaded during codegen entry)
inline constexpr RegisterA64 rState = x19;         // lua_State* L
inline constexpr RegisterA64 rNativeContext = x20; // NativeContext* context
inline constexpr RegisterA64 rGlobalState = x21;   // global_State* L->global

// 2. Frame registers (reloaded when call frame changes; rBase is also reloaded after all calls that may reallocate stack)
inline constexpr RegisterA64 rConstants = x22; // TValue* k
inline constexpr RegisterA64 rClosure = x23;   // Closure* cl
inline constexpr RegisterA64 rCode = x24;      // Instruction* code
inline constexpr RegisterA64 rBase = x25;      // StkId base

// Native code is as stackless as the interpreter, so we can place some data on the stack once and have it accessible at any point
// See CodeGenA64.cpp for layout
inline constexpr unsigned kStashSlots = 9;  // stashed non-volatile registers
inline constexpr unsigned kTempSlots = 1;   // 8 bytes of temporary space, such luxury!
inline constexpr unsigned kSpillSlots = LUA_VECTOR_SIZE == 4 ? 62 : 22; // slots for spilling temporary registers
inline constexpr unsigned kValueTagSlots = LUA_VECTOR_SIZE == 4 ? 16 : 0;

inline constexpr unsigned kStackSize = (kStashSlots + kTempSlots + kSpillSlots + kValueTagSlots) * 8;

inline constexpr AddressA64 sSpillArea = mem(sp, (kStashSlots + kTempSlots) * 8);
inline constexpr AddressA64 sTemporary = mem(sp, kStashSlots * 8);

// A wide TValue keeps its payload in qN and its tag in a native-stack slot.
// x16/x17 are reserved scratch registers in the wide-value allocator.
inline AddressA64 valueTagAddress(RegisterA64 reg)
{
    CODEGEN_ASSERT(reg.kind == KindA64::q);
    return mem(sp, (kStashSlots + kTempSlots + kSpillSlots) * 8 + reg.index * 4);
}

inline AddressA64 valuePayloadAddress(AssemblyBuilderA64& build, AddressA64 addr)
{
    CODEGEN_ASSERT(addr.kind == AddressKindA64::imm);
    if (addr.data >= -256 && addr.data + int(offsetof(TValue, tt)) <= 255)
        return addr;
    // Both the q payload and the w tag must fit their scaled immediate encodings.
    if (addr.data >= 0 && addr.data % 16 == 0 && unsigned(addr.data + offsetof(TValue, tt)) <= 4095 * 4)
        return addr;

    CODEGEN_ASSERT(addr.base != x17);
    if (addr.data >= 0 && addr.data <= 65535)
        build.add(x17, addr.base, uint16_t(addr.data));
    else
    {
        CODEGEN_ASSERT(addr.base != sp);
        build.mov(x17, addr.data);
        build.add(x17, addr.base, x17);
    }
    return mem(x17);
}

inline void emitLoadTag(AssemblyBuilderA64& build, RegisterA64 dst, RegisterA64 value)
{
#if LUA_VECTOR_SIZE == 4
    build.ldr(dst, valueTagAddress(value));
#else
    build.umov_4s(dst, value, 3);
#endif
}

inline void emitMoveTValue(AssemblyBuilderA64& build, RegisterA64 dst, RegisterA64 src)
{
    if (dst == src)
        return;
    build.mov(dst, src);
#if LUA_VECTOR_SIZE == 4
    build.ldr(w16, valueTagAddress(src));
    build.str(w16, valueTagAddress(dst));
#endif
}

inline void emitLoadTValue(AssemblyBuilderA64& build, RegisterA64 dst, AddressA64 src)
{
#if LUA_VECTOR_SIZE == 4
    CODEGEN_ASSERT(src.kind == AddressKindA64::imm && src.base != x16);
    AddressA64 payload = valuePayloadAddress(build, src);
    build.ldr(dst, payload);
    build.ldr(w16, mem(payload.base, payload.data + offsetof(TValue, tt)));
    build.str(w16, valueTagAddress(dst));
#else
    build.ldr(dst, src);
#endif
}

inline void emitStoreTValue(AssemblyBuilderA64& build, RegisterA64 src, AddressA64 dst)
{
#if LUA_VECTOR_SIZE == 4
    CODEGEN_ASSERT(dst.kind == AddressKindA64::imm && dst.base != x16);
    AddressA64 payload = valuePayloadAddress(build, dst);
    build.str(src, payload);
    build.ldr(w16, valueTagAddress(src));
    build.str(w16, mem(payload.base, payload.data + offsetof(TValue, tt)));
#else
    build.str(src, dst);
#endif
}

inline void emitAddScaled(AssemblyBuilderA64& build, RegisterA64 dst, RegisterA64 base, RegisterA64 index, unsigned size)
{
#if LUA_VECTOR_SIZE == 4
    CODEGEN_ASSERT(size == 24 || size == 48);
    build.mov(x16, int(size));
    if (index.kind == KindA64::w)
    {
        build.ubfx(x17, castReg(KindA64::x, index), 0, 32);
        build.mul(x16, x17, x16);
    }
    else
        build.mul(x16, index, x16);
    build.add(dst, base, x16);
#else
    CODEGEN_ASSERT(size == 16 || size == 32);
    build.add(dst, base, index, size == 16 ? 4 : 5);
#endif
}

inline void emitUpdateBase(AssemblyBuilderA64& build)
{
    build.ldr(rBase, mem(rState, offsetof(lua_State, base)));
}

} // namespace A64
} // namespace CodeGen
} // namespace Luau
