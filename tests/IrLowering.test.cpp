// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "lua.h"
#include "lualib.h"
#include "luacode.h"

#include "Luau/BytecodeBuilder.h"
#include "Luau/CodeGen.h"
#include "Luau/Compiler.h"
#include "Luau/Parser.h"
#include "Luau/IrBuilder.h"

#include "doctest.h"
#include "ScopedFlags.h"
#include "ConformanceIrHooks.h"

#include <memory>
#include <string_view>

LUAU_FASTFLAG(LuauIntegerFastcalls)
LUAU_FASTFLAG(LuauCodegenInteger3)
LUAU_FASTFLAG(LuauIntegerType2)
LUAU_FASTFLAG(LuauCodegenBufferInteger)
LUAU_FASTFLAG(LuauIntegerBufferFastcalls)
LUAU_FASTFLAG(LuauEmitCallFeedback)
LUAU_FASTFLAG(LuauBackedgeHeapCheck)
LUAU_FASTFLAG(LuauCodegenPropagateFallbackTags)
LUAU_FASTFLAG(LuauCodegenIntegerCompare)
LUAU_FASTFLAG(LuauCompileReuseLocalRegs)
LUAU_FASTFLAG(LuauLoadRemapOptionalUserdata)
LUAU_FASTFLAG(LuauCodegenConstPropMinOffset)
LUAU_FASTFLAG(DebugLuauUserDefinedClassesRuntime)

#define ensureVectorSize3() \
    if constexpr (LUA_VECTOR_SIZE != 3) \
    return
#define ensureVectorFloat() \
    if constexpr (LUA_VECTOR_DOUBLE == 1) \
    return

static void luauLibraryConstantLookup(const char* library, const char* member, Luau::CompileConstant* constant)
{
    // While 'vector' library constants are a Luau built-in, their constant value depends on the embedder LUA_VECTOR_SIZE value
    if (strcmp(library, "vector") == 0)
    {
        if (strcmp(member, "zero") == 0)
            return Luau::setCompileConstantVector(constant, 0.0f, 0.0f, 0.0f, 0.0f);

        if (strcmp(member, "one") == 0)
            return Luau::setCompileConstantVector(constant, 1.0f, 1.0f, 1.0f, 0.0f);
    }

    if (strcmp(library, "Vector3") == 0)
    {
        if (strcmp(member, "xAxis") == 0)
            return Luau::setCompileConstantVector(constant, 1.0f, 0.0f, 0.0f, 0.0f);

        if (strcmp(member, "yAxis") == 0)
            return Luau::setCompileConstantVector(constant, 0.0f, 1.0f, 0.0f, 0.0f);
    }

    if (strcmp(library, "test") == 0)
    {
        if (strcmp(member, "some_nil") == 0)
            return luau_set_compile_constant_nil(constant);

        if (strcmp(member, "some_boolean") == 0)
            return luau_set_compile_constant_boolean(constant, 1);

        if (strcmp(member, "some_number") == 0)
            return luau_set_compile_constant_number(constant, 4.75);

        if (strcmp(member, "some_vector") == 0)
            return luau_set_compile_constant_vector(constant, 1.0f, 2.0f, 4.0f, 8.0f);

        if (strcmp(member, "some_string") == 0)
            return luau_set_compile_constant_string(constant, "test", 4);
    }
}

static int luauLibraryTypeLookup(const char* library, const char* member)
{
    if (strcmp(library, "Vector3") == 0)
    {
        if (strcmp(member, "xAxis") == 0)
            return LuauBytecodeType::LBC_TYPE_VECTOR;

        if (strcmp(member, "yAxis") == 0)
            return LuauBytecodeType::LBC_TYPE_VECTOR;
    }

    return LuauBytecodeType::LBC_TYPE_ANY;
}

class LoweringFixture
{
public:
    LoweringFixture()
    {
        static const char* kUserdataCompileTypes[] = {"vec2", "color", "mat3", "vertex", nullptr};
        static const char* kLibrariesWithConstants[] = {"vector", "Vector3", "test", nullptr};

        compilationOptions.optimizationLevel = 2;
        compilationOptions.debugLevel = 1;
        compilationOptions.typeInfoLevel = 1;
        compilationOptions.vectorCtor = "vector";
        compilationOptions.vectorType = "vector";
        compilationOptions.vectorPrecision = 0;
        compilationOptions.userdataTypes = kUserdataCompileTypes;
        compilationOptions.librariesWithKnownMembers = kLibrariesWithConstants;
        compilationOptions.libraryMemberTypeCb = luauLibraryTypeLookup;
        compilationOptions.libraryMemberConstantCb = luauLibraryConstantLookup;

        compilationOptionsC.optimizationLevel = 2;
        compilationOptionsC.debugLevel = 1;
        compilationOptionsC.typeInfoLevel = 1;
        compilationOptionsC.vectorCtor = "vector";
        compilationOptionsC.vectorType = "vector";
        compilationOptionsC.vectorPrecision = 0;
        compilationOptionsC.userdataTypes = kUserdataCompileTypes;
        compilationOptionsC.librariesWithKnownMembers = kLibrariesWithConstants;
        compilationOptionsC.libraryMemberTypeCb = luauLibraryTypeLookup;
        compilationOptionsC.libraryMemberConstantCb = luauLibraryConstantLookup;

        assemblyOptions.compilationOptions.hooks.vectorAccessBytecodeType = vectorAccessBytecodeType;
        assemblyOptions.compilationOptions.hooks.vectorNamecallBytecodeType = vectorNamecallBytecodeType;
        assemblyOptions.compilationOptions.hooks.vectorAccess = vectorAccess;
        assemblyOptions.compilationOptions.hooks.vectorNamecall = vectorNamecall;

        assemblyOptions.compilationOptions.hooks.userdataAccessBytecodeType = userdataAccessBytecodeType;
        assemblyOptions.compilationOptions.hooks.userdataMetamethodBytecodeType = userdataMetamethodBytecodeType;
        assemblyOptions.compilationOptions.hooks.userdataNamecallBytecodeType = userdataNamecallBytecodeType;
        assemblyOptions.compilationOptions.hooks.userdataAccess = userdataAccess;
        assemblyOptions.compilationOptions.hooks.userdataMetamethod = userdataMetamethod;
        assemblyOptions.compilationOptions.hooks.userdataNamecall = userdataNamecall;

        // Runtime mapping is specifically created to NOT match the compilation mapping
        assemblyOptions.compilationOptions.userdataTypes = kUserdataRunTypes;

        assemblyOptions.outputBinary = false;
        assemblyOptions.includeAssembly = false;
        assemblyOptions.includeIr = true;
        assemblyOptions.includeOutlinedCode = false;
        assemblyOptions.includeIrTypes = false;
        assemblyOptions.includeRegSpills = false;

        assemblyOptions.includeIrPrefix = Luau::CodeGen::IncludeIrPrefix::No;
        assemblyOptions.includeUseInfo = Luau::CodeGen::IncludeUseInfo::No;
        assemblyOptions.includeCfgInfo = Luau::CodeGen::IncludeCfgInfo::No;
        assemblyOptions.includeRegFlowInfo = Luau::CodeGen::IncludeRegFlowInfo::No;
    }

    Luau::CompileOptions compilationOptions = {};
    lua_CompileOptions compilationOptionsC = {};

    Luau::CodeGen::AssemblyOptions assemblyOptions = {};

    void initializeCodegen(lua_State* L)
    {
        if (Luau::CodeGen::isSupported())
        {
            // Type remapper requires the codegen runtime
            Luau::CodeGen::create(L);

            Luau::CodeGen::setUserdataRemapper(
                L,
                kUserdataRunTypes,
                [](void* context, const char* str, size_t len) -> uint8_t
                {
                    const char** types = (const char**)context;

                    uint8_t index = 0;

                    std::string_view sv{str, len};

                    for (; *types; ++types)
                    {
                        if (sv == *types)
                            return index;

                        index++;
                    }

                    return 0xff;
                }
            );
        }
    }

    std::string getCodegenAssembly(
        const char* source,
        bool includeIrTypes = false,
        int debugLevel = 1,
        int optimizationLevel = 2,
        bool clipToFirstReturn = false
    )
    {
        // TODO: move this into a per-test setup
        compilationOptions.optimizationLevel = optimizationLevel;
        compilationOptions.debugLevel = debugLevel;
        assemblyOptions.includeIrTypes = includeIrTypes;

        Luau::Allocator allocator;
        Luau::AstNameTable names(allocator);
        Luau::ParseResult result = Luau::Parser::parse(source, strlen(source), names, allocator);

        if (!result.errors.empty())
            throw Luau::ParseErrors(result.errors);

        Luau::BytecodeBuilder bcb;
        Luau::compileOrThrow(bcb, result, names, compilationOptions);

        std::string bytecode = bcb.getBytecode();
        std::unique_ptr<lua_State, void (*)(lua_State*)> globalState(luaL_newstate(), lua_close);
        lua_State* L = globalState.get();

        initializeCodegen(L);

        if (luau_load(L, "name", bytecode.data(), bytecode.size(), 0) == 0)
        {
            // For IR, we don't care about assembly, but we want a stable target
            assemblyOptions.target = Luau::CodeGen::AssemblyOptions::Target::X64_SystemV;

            std::string result = Luau::CodeGen::getAssembly(L, -1, assemblyOptions, nullptr);

            if (Luau::CodeGen::isSupported())
            {
                // Checking that other target lower correctly as well
                assemblyOptions.target = Luau::CodeGen::AssemblyOptions::Target::A64;

                Luau::CodeGen::getAssembly(L, -1, assemblyOptions, nullptr);
            }

            if (clipToFirstReturn)
            {
                if (auto pos = result.find("RETURN"); pos != std::string::npos)
                {
                    if (auto newLine = result.find('\n', pos); newLine != std::string::npos)
                        return result.substr(0, newLine + 1);
                }
            }

            return result;
        }

        FAIL("Failed to load bytecode");
        return "";
    }

    std::string getCodegenAssemblyUsingCApi(const char* source, bool includeIrTypes = false, int debugLevel = 1)
    {
        // TODO: move this into a per-test setup
        compilationOptionsC.debugLevel = debugLevel;
        assemblyOptions.includeIrTypes = includeIrTypes;

        size_t bytecodeSize = 0;
        char* bytecode = luau_compile(source, strlen(source), &compilationOptionsC, &bytecodeSize);
        REQUIRE(bytecode);

        std::unique_ptr<lua_State, void (*)(lua_State*)> globalState(luaL_newstate(), lua_close);
        lua_State* L = globalState.get();

        initializeCodegen(L);

        if (luau_load(L, "name", bytecode, bytecodeSize, 0) == 0)
        {
            free(bytecode);

            // For IR, we don't care about assembly, but we want a stable target
            assemblyOptions.target = Luau::CodeGen::AssemblyOptions::Target::X64_SystemV;

            return Luau::CodeGen::getAssembly(L, -1, assemblyOptions, nullptr);
        }

        free(bytecode);

        FAIL("Failed to load bytecode");
        return "";
    }

    std::string getCodegenHeader(const char* source)
    {
        std::string assembly = getCodegenAssembly(source, /* includeIrTypes */ true, /* debugLevel */ 2);

        // Skip functions until we get the last one
        while (assembly.find("; function ", 1) != std::string::npos)
            assembly = assembly.substr(assembly.find("; function ", 1));

        auto bytecodeStart = assembly.find("bb_bytecode_0:");

        if (bytecodeStart == std::string::npos)
            bytecodeStart = assembly.find("bb_0:");

        REQUIRE(bytecodeStart != std::string::npos);

        return assembly.substr(0, bytecodeStart);
    }
};

TEST_SUITE_BEGIN("IrLowering");

TEST_CASE_FIXTURE(LoweringFixture, "UnsafeEnvFastcallUsesNativeFallback")
{
    assemblyOptions.includeOutlinedCode = true;
    std::string ir = getCodegenAssembly(R"(
local function checked(value)
    assert(value)
    return math.abs(value)
end
)");

    CHECK(ir.find("CHECK_SAFE_ENV bb_") != std::string::npos);
    CHECK(ir.find("implicit CHECK_SAFE_ENV") == std::string::npos);
    CHECK(ir.find("CHECK_SAFE_ENV exit(") == std::string::npos);
    CHECK(ir.find("GET_CACHED_IMPORT") != std::string::npos);
    CHECK(ir.find("GET_IMPORT") != std::string::npos);
    CHECK(ir.find("CALL R") != std::string::npos);
}

TEST_CASE_FIXTURE(LoweringFixture, "UnsafeEnvStandaloneImportUsesNativeFallback")
{
    assemblyOptions.includeOutlinedCode = true;
    std::string ir = getCodegenAssembly(R"(
local function imported()
    return external.branch.value
end
)");

    CHECK(ir.find("CHECK_SAFE_ENV bb_") != std::string::npos);
    CHECK(ir.find("implicit CHECK_SAFE_ENV") == std::string::npos);
    CHECK(ir.find("CHECK_SAFE_ENV exit(") == std::string::npos);
    CHECK(ir.find("GET_CACHED_IMPORT") != std::string::npos);
    CHECK(ir.find("GET_IMPORT") != std::string::npos);
}

TEST_CASE_FIXTURE(LoweringFixture, "VectorReciprocal")
{
    ensureVectorFloat();

    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function vecrcp(a: vector)
    return 1 / a
end
)"),
        R"(
; function vecrcp($arg0) line 2
bb_0:
  CHECK_TAG R0, tvector, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %7 = FLOAT_TO_VEC 1
  %8 = LOAD_TVALUE R0, 0i, tvector
  %9 = DIV_VEC %7, %8
  %10 = TAG_VECTOR %9
  STORE_TVALUE R1, %10
  INTERRUPT 1u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "VectorComponentRead")
{
    ensureVectorFloat();
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function compsum(a: vector)
    return a.X + a.Y + a.Z
end
)"),
        R"(
; function compsum($arg0) line 2
bb_0:
  CHECK_TAG R0, tvector, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %6 = LOAD_FLOAT R0, 0i
  %7 = FLOAT_TO_NUM %6
  %12 = LOAD_FLOAT R0, 4i
  %13 = FLOAT_TO_NUM %12
  %22 = ADD_NUM %7, %13
  %27 = LOAD_FLOAT R0, 8i
  %28 = FLOAT_TO_NUM %27
  %37 = ADD_NUM %22, %28
  STORE_DOUBLE R1, %37
  STORE_TAG R1, tnumber
  INTERRUPT 8u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "VectorAdd")
{
    ensureVectorFloat();

    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function vec3add(a: vector, b: vector)
    return a + b
end
)"),
        R"(
; function vec3add($arg0, $arg1) line 2
bb_0:
  CHECK_TAG R0, tvector, exit(entry)
  CHECK_TAG R1, tvector, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %10 = LOAD_TVALUE R0, 0i, tvector
  %11 = LOAD_TVALUE R1, 0i, tvector
  %12 = ADD_VEC %10, %11
  %13 = TAG_VECTOR %12
  STORE_TVALUE R2, %13
  INTERRUPT 1u
  RETURN R2, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "VectorMinus")
{
    ensureVectorFloat();

    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function vec3minus(a: vector)
    return -a
end
)"),
        R"(
; function vec3minus($arg0) line 2
bb_0:
  CHECK_TAG R0, tvector, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %6 = LOAD_TVALUE R0, 0i, tvector
  %7 = UNM_VEC %6
  %8 = TAG_VECTOR %7
  STORE_TVALUE R1, %8
  INTERRUPT 1u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "VectorSubMulDiv")
{
    ensureVectorFloat();

    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function vec3combo(a: vector, b: vector, c: vector, d: vector)
    return a * b - c / d
end
)"),
        R"(
; function vec3combo($arg0, $arg1, $arg2, $arg3) line 2
bb_0:
  CHECK_TAG R0, tvector, exit(entry)
  CHECK_TAG R1, tvector, exit(entry)
  CHECK_TAG R2, tvector, exit(entry)
  CHECK_TAG R3, tvector, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %14 = LOAD_TVALUE R0, 0i, tvector
  %15 = LOAD_TVALUE R1, 0i, tvector
  %16 = MUL_VEC %14, %15
  %23 = LOAD_TVALUE R2, 0i, tvector
  %24 = LOAD_TVALUE R3, 0i, tvector
  %25 = DIV_VEC %23, %24
  %34 = SUB_VEC %16, %25
  %35 = TAG_VECTOR %34
  STORE_TVALUE R4, %35
  INTERRUPT 3u
  RETURN R4, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "VectorSubMulDiv2")
{
    ensureVectorFloat();

    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function vec3combo(a: vector)
    local tmp = a * a
    return (tmp - tmp) / (tmp + tmp)
end
)"),
        R"(
; function vec3combo($arg0) line 2
bb_0:
  CHECK_TAG R0, tvector, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %8 = LOAD_TVALUE R0, 0i, tvector
  %10 = MUL_VEC %8, %8
  %19 = SUB_VEC %10, %10
  %28 = ADD_VEC %10, %10
  %37 = DIV_VEC %19, %28
  %38 = TAG_VECTOR %37
  STORE_TVALUE R2, %38
  INTERRUPT 4u
  RETURN R2, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "VectorMulDivMixed")
{
    ensureVectorFloat();

    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function vec3combo(a: vector, b: vector, c: vector, d: vector)
    return a * 2 + b / 4 + 0.5 * c + 40 / d
end
)"),
        R"(
; function vec3combo($arg0, $arg1, $arg2, $arg3) line 2
bb_0:
  CHECK_TAG R0, tvector, exit(entry)
  CHECK_TAG R1, tvector, exit(entry)
  CHECK_TAG R2, tvector, exit(entry)
  CHECK_TAG R3, tvector, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %12 = LOAD_TVALUE R0, 0i, tvector
  %14 = FLOAT_TO_VEC 2
  %15 = MUL_VEC %12, %14
  %20 = LOAD_TVALUE R1, 0i, tvector
  %22 = FLOAT_TO_VEC 4
  %23 = DIV_VEC %20, %22
  %32 = ADD_VEC %15, %23
  %37 = LOAD_TVALUE R2, 0i, tvector
  %39 = FLOAT_TO_VEC 0.5
  %40 = MUL_VEC %37, %39
  %49 = ADD_VEC %32, %40
  %55 = FLOAT_TO_VEC 40
  %56 = LOAD_TVALUE R3, 0i, tvector
  %57 = DIV_VEC %55, %56
  %66 = ADD_VEC %49, %57
  %67 = TAG_VECTOR %66
  STORE_TVALUE R4, %67
  INTERRUPT 7u
  RETURN R4, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "VectorLerp")
{
    ensureVectorFloat();

    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function vec3lerp(a: vector, b: vector, t: number)
    return vector.lerp(a, b, t)
end
)"),
        R"(
; function vec3lerp($arg0, $arg1, $arg2) line 2
bb_0:
  CHECK_TAG R0, tvector, exit(entry)
  CHECK_TAG R1, tvector, exit(entry)
  CHECK_TAG R2, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  CHECK_SAFE_ENV bb_fallback_3
  %15 = LOAD_TVALUE R0, 0i, tvector
  %16 = LOAD_TVALUE R1, 0i, tvector
  %17 = LOAD_DOUBLE R2
  %18 = NUM_TO_FLOAT %17
  %19 = FLOAT_TO_VEC %18
  %20 = FLOAT_TO_VEC 1
  %21 = SUB_VEC %16, %15
  %22 = MULADD_VEC %21, %19, %15
  %23 = SELECT_VEC %22, %16, %19, %20
  %24 = TAG_VECTOR %23
  STORE_TVALUE R3, %24
  JUMP bb_4
bb_4:
  INTERRUPT 8u
  RETURN R3, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "VectorMinMax")
{
    ensureVectorFloat();

    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function vecops(a: vector, b: vector)
    return vector.min(a, b), vector.max(a, b)
end
)"),
        R"(
; function vecops($arg0, $arg1) line 2
bb_0:
  CHECK_TAG R0, tvector, exit(entry)
  CHECK_TAG R1, tvector, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  CHECK_SAFE_ENV bb_fallback_3
  %11 = LOAD_TVALUE R0, 0i, tvector
  %12 = LOAD_TVALUE R1, 0i, tvector
  %13 = MIN_VEC %12, %11
  %14 = TAG_VECTOR %13
  STORE_TVALUE R2, %14
  JUMP bb_4
bb_4:
  CHECK_SAFE_ENV bb_fallback_7
  %36 = LOAD_TVALUE R0, 0i, tvector
  %37 = LOAD_TVALUE R1, 0i, tvector
  %38 = MAX_VEC %37, %36
  %39 = TAG_VECTOR %38
  STORE_TVALUE R3, %39
  JUMP bb_8
bb_8:
  INTERRUPT 14u
  RETURN R2, 2i
)"
    );
}
TEST_CASE_FIXTURE(LoweringFixture, "VectorFloorCeilAbs")
{
    ensureVectorFloat();

    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function vecops(a: vector)
    return vector.abs(a), vector.floor(a), vector.ceil(a)
end
)"),
        R"(
; function vecops($arg0) line 2
bb_0:
  CHECK_TAG R0, tvector, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  CHECK_SAFE_ENV bb_fallback_3
  %7 = LOAD_TVALUE R0, 0i, tvector
  %8 = ABS_VEC %7
  %9 = TAG_VECTOR %8
  STORE_TVALUE R1, %9
  JUMP bb_linear_15
bb_linear_15:
  %68 = FLOOR_VEC %7
  %69 = TAG_VECTOR %68
  STORE_TVALUE R2, %69
  %74 = CEIL_VEC %7
  %75 = TAG_VECTOR %74
  STORE_TVALUE R3, %75
  INTERRUPT 15u
  RETURN R1, 3i
bb_4:
  CHECK_SAFE_ENV bb_fallback_7
  %27 = LOAD_TVALUE R0, 0i, tvector
  %28 = FLOOR_VEC %27
  %29 = TAG_VECTOR %28
  STORE_TVALUE R2, %29
  JUMP bb_8
bb_8:
  CHECK_SAFE_ENV bb_fallback_11
  %47 = LOAD_TVALUE R0, 0i, tvector
  %48 = CEIL_VEC %47
  %49 = TAG_VECTOR %48
  STORE_TVALUE R3, %49
  JUMP bb_12
bb_12:
  INTERRUPT 15u
  RETURN R1, 3i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "ExtraMathMemoryOperands")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(a: number, b: number, c: number, d: number, e: number)
    return math.floor(a) + math.ceil(b) + math.round(c) + math.sqrt(d) + math.abs(e)
end
)"),
        R"(
; function foo($arg0, $arg1, $arg2, $arg3, $arg4) line 2
bb_0:
  CHECK_TAG R0, tnumber, exit(entry)
  CHECK_TAG R1, tnumber, exit(entry)
  CHECK_TAG R2, tnumber, exit(entry)
  CHECK_TAG R3, tnumber, exit(entry)
  CHECK_TAG R4, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  CHECK_SAFE_ENV bb_fallback_3
  %16 = FLOOR_NUM R0
  JUMP bb_linear_23
bb_linear_23:
  %152 = CEIL_NUM R1
  %162 = ADD_NUM %16, %152
  %167 = ROUND_NUM R2
  %176 = ADD_NUM %162, %167
  %181 = SQRT_NUM R3
  %190 = ADD_NUM %176, %181
  %195 = ABS_NUM R4
  %204 = ADD_NUM %190, %195
  STORE_DOUBLE R5, %204
  STORE_TAG R5, tnumber
  INTERRUPT 29u
  RETURN R5, 1i
bb_4:
  CHECK_SAFE_ENV bb_fallback_7
  %36 = CEIL_NUM R1
  STORE_DOUBLE R10, %36
  STORE_TAG R10, tnumber
  JUMP bb_8
bb_8:
  CHECK_TAG R9, tnumber, exit(10)
  CHECK_TAG R10, tnumber, exit(10)
  %56 = LOAD_DOUBLE R9
  %58 = ADD_NUM %56, R10
  STORE_DOUBLE R8, %58
  STORE_TAG R8, tnumber
  CHECK_SAFE_ENV bb_fallback_11
  %65 = ROUND_NUM R2
  STORE_DOUBLE R9, %65
  JUMP bb_12
bb_12:
  CHECK_TAG R8, tnumber, exit(16)
  CHECK_TAG R9, tnumber, exit(16)
  %85 = LOAD_DOUBLE R8
  %87 = ADD_NUM %85, R9
  STORE_DOUBLE R7, %87
  STORE_TAG R7, tnumber
  CHECK_SAFE_ENV bb_fallback_15
  %94 = SQRT_NUM R3
  STORE_DOUBLE R8, %94
  JUMP bb_16
bb_16:
  CHECK_TAG R7, tnumber, exit(22)
  CHECK_TAG R8, tnumber, exit(22)
  %114 = LOAD_DOUBLE R7
  %116 = ADD_NUM %114, R8
  STORE_DOUBLE R6, %116
  STORE_TAG R6, tnumber
  CHECK_SAFE_ENV bb_fallback_19
  %123 = ABS_NUM R4
  STORE_DOUBLE R7, %123
  JUMP bb_20
bb_20:
  CHECK_TAG R6, tnumber, exit(28)
  CHECK_TAG R7, tnumber, exit(28)
  %143 = LOAD_DOUBLE R6
  %145 = ADD_NUM %143, R7
  STORE_DOUBLE R5, %145
  STORE_TAG R5, tnumber
  INTERRUPT 29u
  RETURN R5, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "DseInitialStackState")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo()
    while {} do
        local _ = not _,{}
        _ = nil
    end
end
)"),
        R"(
; function foo() line 2
bb_bytecode_0:
  SET_SAVEDPC 1u
  %1 = NEW_TABLE 0u, 0u
  STORE_POINTER R0, %1
  STORE_TAG R0, ttable
  CHECK_GC
  JUMP bb_2
bb_2:
  CHECK_SAFE_ENV bb_fallback_3
  GET_CACHED_IMPORT R1, K1 (nil), 1073741824u ('_'), 4u
  JUMP bb_4
bb_4:
  SET_SAVEDPC 7u
  %17 = NEW_TABLE 0u, 0u
  STORE_POINTER R1, %17
  STORE_TAG R1, ttable
  CHECK_GC
  STORE_TAG R0, tnil
  INTERRUPT 9u
  JUMP bb_bytecode_0
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "DseInitialStackState2")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(a)
    math.frexp(a)
    return a
end
)"),
        R"(
; function foo($arg0) line 2
bb_bytecode_0:
  CHECK_SAFE_ENV bb_fallback_1
  CHECK_TAG R0, tnumber, exit(1)
  FASTCALL 14u, R1, R0, 2i
  JUMP bb_2
bb_2:
  INTERRUPT 5u
  RETURN R0, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "StringCompare")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(a)
    return a == "test"
end
)"
               ),
        R"(
; function foo($arg0) line 2
bb_bytecode_0:
  %0 = LOAD_TAG R0
  %1 = LOAD_POINTER R0
  %2 = LOAD_POINTER K0 ('test')
  %3 = CMP_SPLIT_TVALUE %0, tstring, %1, %2, eq
  STORE_TAG R1, tboolean
  STORE_INT R1, %3
  JUMP bb_bytecode_2
bb_bytecode_2:
  INTERRUPT 4u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "StringCompareAnnotated")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(a: string)
    return a == "test"
end
)"
               ),
        R"(
; function foo($arg0) line 2
bb_0:
  CHECK_TAG R0, tstring, exit(entry)
  JUMP bb_4
bb_4:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %5 = LOAD_POINTER R0
  %6 = LOAD_POINTER K0 ('test')
  %7 = CMP_SPLIT_TVALUE tstring, tstring, %5, %6, eq
  STORE_TAG R1, tboolean
  STORE_INT R1, %7
  JUMP bb_bytecode_3
bb_bytecode_3:
  INTERRUPT 4u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "NilCompare")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(a)
    return a == nil
end
)"
               ),
        R"(
; function foo($arg0) line 2
bb_bytecode_0:
  %0 = LOAD_TAG R0
  %1 = CMP_TAG %0, tnil, eq
  STORE_TAG R1, tboolean
  STORE_INT R1, %1
  JUMP bb_bytecode_2
bb_bytecode_2:
  INTERRUPT 4u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "BooleanCompare")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(a)
    return { a == true, a == false, a ~= true, a ~= false }
end
)"
               ),
        R"(
; function foo($arg0) line 2
bb_bytecode_0:
  SET_SAVEDPC 1u
  %1 = NEW_TABLE 4u, 0u
  STORE_POINTER R1, %1
  STORE_TAG R1, ttable
  CHECK_GC
  %5 = LOAD_TAG R0
  %6 = LOAD_INT R0
  %7 = CMP_SPLIT_TVALUE %5, tboolean, %6, 1i, eq
  STORE_TAG R2, tboolean
  STORE_INT R2, %7
  JUMP bb_bytecode_2
bb_bytecode_2:
  %16 = CMP_SPLIT_TVALUE %5, tboolean, %6, 0i, eq
  STORE_TAG R3, tboolean
  STORE_INT R3, %16
  JUMP bb_bytecode_4
bb_bytecode_4:
  %25 = CMP_SPLIT_TVALUE %5, tboolean, %6, 1i, not_eq
  STORE_TAG R4, tboolean
  STORE_INT R4, %25
  JUMP bb_bytecode_6
bb_bytecode_6:
  %34 = CMP_SPLIT_TVALUE %5, tboolean, %6, 0i, not_eq
  STORE_TAG R5, tboolean
  STORE_INT R5, %34
  JUMP bb_bytecode_8
bb_bytecode_8:
  SETLIST 18u, R1, R2, 4i, 1u, 4u
  INTERRUPT 20u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "NumberCompare")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(a)
    return { a == 4.0, a ~= 3.0 }
end
)"
               ),
        R"(
; function foo($arg0) line 2
bb_bytecode_0:
  SET_SAVEDPC 1u
  %1 = NEW_TABLE 2u, 0u
  STORE_POINTER R1, %1
  STORE_TAG R1, ttable
  CHECK_GC
  %5 = LOAD_TAG R0
  %6 = LOAD_DOUBLE R0
  %7 = CMP_SPLIT_TVALUE %5, tnumber, %6, 4, eq
  STORE_TAG R2, tboolean
  STORE_INT R2, %7
  JUMP bb_bytecode_2
bb_bytecode_2:
  %16 = CMP_SPLIT_TVALUE %5, tnumber, %6, 3, not_eq
  STORE_TAG R3, tboolean
  STORE_INT R3, %16
  JUMP bb_bytecode_4
bb_bytecode_4:
  SETLIST 10u, R1, R2, 2i, 1u, 2u
  INTERRUPT 12u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "NumberCompare2")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(a, b, c)
    return { a == b, a ~= c }
end
)"
               ),
        R"(
; function foo($arg0, $arg1, $arg2) line 2
bb_bytecode_0:
  SET_SAVEDPC 1u
  %1 = NEW_TABLE 2u, 0u
  STORE_POINTER R3, %1
  STORE_TAG R3, ttable
  CHECK_GC
  CHECK_TAG R0, tnumber, bb_fallback_5
  CHECK_TAG R1, tnumber, bb_fallback_5
  %9 = LOAD_DOUBLE R0
  %10 = LOAD_DOUBLE R1
  %11 = CMP_SPLIT_TVALUE tnumber, tnumber, %9, %10, eq
  STORE_INT R4, %11
  STORE_TAG R4, tboolean
  JUMP bb_bytecode_2
bb_bytecode_2:
  CHECK_TAG R0, tnumber, bb_fallback_6
  CHECK_TAG R2, tnumber, bb_fallback_6
  %27 = LOAD_DOUBLE R0
  %28 = LOAD_DOUBLE R2
  %29 = CMP_SPLIT_TVALUE tnumber, tnumber, %27, %28, not_eq
  STORE_INT R5, %29
  STORE_TAG R5, tboolean
  JUMP bb_bytecode_4
bb_bytecode_4:
  SETLIST 10u, R3, R4, 2i, 1u, undef
  INTERRUPT 12u
  RETURN R3, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "NumberCompare3")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(a: number, b: number, c: number)
    return { a == b, a ~= c }
end
)"
               ),
        R"(
; function foo($arg0, $arg1, $arg2) line 2
bb_0:
  CHECK_TAG R0, tnumber, exit(entry)
  CHECK_TAG R1, tnumber, exit(entry)
  CHECK_TAG R2, tnumber, exit(entry)
  JUMP bb_6
bb_6:
  JUMP bb_bytecode_1
bb_bytecode_1:
  SET_SAVEDPC 1u
  %9 = NEW_TABLE 2u, 0u
  STORE_POINTER R3, %9
  STORE_TAG R3, ttable
  CHECK_GC
  %17 = LOAD_DOUBLE R0
  %18 = LOAD_DOUBLE R1
  %19 = CMP_SPLIT_TVALUE tnumber, tnumber, %17, %18, eq
  STORE_INT R4, %19
  STORE_TAG R4, tboolean
  JUMP bb_bytecode_3
bb_bytecode_3:
  %31 = LOAD_DOUBLE R2
  %32 = CMP_SPLIT_TVALUE tnumber, tnumber, %17, %31, not_eq
  STORE_INT R5, %32
  STORE_TAG R5, tboolean
  JUMP bb_bytecode_5
bb_bytecode_5:
  SETLIST 10u, R3, R4, 2i, 1u, 2u
  INTERRUPT 12u
  RETURN R3, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "TypeCompare")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(a)
    return type(a) == "number"
end
)"
               ),
        R"(
; function foo($arg0) line 2
bb_bytecode_0:
  CHECK_SAFE_ENV bb_fallback_3
  %1 = LOAD_TAG R0
  %2 = GET_TYPE %1
  STORE_POINTER R2, %2
  STORE_TAG R2, tstring
  JUMP bb_4
bb_4:
  %18 = LOAD_TAG R2
  %19 = LOAD_POINTER R2
  %20 = LOAD_POINTER K2 ('number')
  %21 = CMP_SPLIT_TVALUE %18, tstring, %19, %20, eq
  STORE_TAG R1, tboolean
  STORE_INT R1, %21
  JUMP bb_bytecode_2
bb_bytecode_2:
  INTERRUPT 9u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "TypeofCompare")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(a)
    return typeof(a) == "number"
end
)"
               ),
        R"(
; function foo($arg0) line 2
bb_bytecode_0:
  CHECK_SAFE_ENV bb_fallback_3
  %1 = GET_TYPEOF R0
  STORE_POINTER R2, %1
  STORE_TAG R2, tstring
  JUMP bb_4
bb_4:
  %17 = LOAD_TAG R2
  %18 = LOAD_POINTER R2
  %19 = LOAD_POINTER K2 ('number')
  %20 = CMP_SPLIT_TVALUE %17, tstring, %18, %19, eq
  STORE_TAG R1, tboolean
  STORE_INT R1, %20
  JUMP bb_bytecode_2
bb_bytecode_2:
  INTERRUPT 9u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "TypeofCompareCustom")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(a)
    return typeof(a) == "User"
end
)"
               ),
        R"(
; function foo($arg0) line 2
bb_bytecode_0:
  CHECK_SAFE_ENV bb_fallback_3
  %1 = GET_TYPEOF R0
  STORE_POINTER R2, %1
  STORE_TAG R2, tstring
  JUMP bb_4
bb_4:
  %17 = LOAD_TAG R2
  %18 = LOAD_POINTER R2
  %19 = LOAD_POINTER K2 ('User')
  %20 = CMP_SPLIT_TVALUE %17, tstring, %18, %19, eq
  STORE_TAG R1, tboolean
  STORE_INT R1, %20
  JUMP bb_bytecode_2
bb_bytecode_2:
  INTERRUPT 9u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "TypeCondition")
{
    // TODO: opportunity - bb_4 already made sure %1 == R0.tag is a number, check in bb_3 can be removed
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(a, b)
    if type(a) == "number" then
        return a + b
    end
    return nil
end
)"
               ),
        R"(
; function foo($arg0, $arg1) line 2
bb_bytecode_0:
  CHECK_SAFE_ENV bb_fallback_2
  %1 = LOAD_TAG R0
  %2 = GET_TYPE %1
  STORE_POINTER R2, %2
  STORE_TAG R2, tstring
  JUMP bb_3
bb_3:
  JUMP_EQ_TAG R2, tstring, bb_7, bb_bytecode_1
bb_7:
  %20 = LOAD_POINTER R2
  %21 = LOAD_POINTER K2 ('number')
  JUMP_EQ_POINTER %20, %21, bb_6, bb_bytecode_1
bb_6:
  CHECK_TAG R0, tnumber, bb_fallback_8
  CHECK_TAG R1, tnumber, bb_fallback_8
  %27 = LOAD_DOUBLE R0
  %29 = ADD_NUM %27, R1
  STORE_DOUBLE R2, %29
  STORE_TAG R2, tnumber
  JUMP bb_9
bb_9:
  INTERRUPT 8u
  RETURN R2, 1i
bb_bytecode_1:
  STORE_TAG R2, tnil
  INTERRUPT 10u
  RETURN R2, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "TypeCondition2")
{
    // TODO: opportunity - bb_4 already made sure env is safe, check in bb_3 can be removed
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(a, b)
    if type(a) == "number" and type(b) == "number" then
        return a + b
    end
    return nil
end
)"
               ),
        R"(
; function foo($arg0, $arg1) line 2
bb_bytecode_0:
  CHECK_SAFE_ENV bb_fallback_2
  %1 = LOAD_TAG R0
  %2 = GET_TYPE %1
  STORE_POINTER R2, %2
  STORE_TAG R2, tstring
  JUMP bb_3
bb_3:
  JUMP_EQ_TAG R2, tstring, bb_7, bb_bytecode_1
bb_7:
  %20 = LOAD_POINTER R2
  %21 = LOAD_POINTER K2 ('number')
  JUMP_EQ_POINTER %20, %21, bb_6, bb_bytecode_1
bb_6:
  CHECK_SAFE_ENV bb_fallback_8
  %24 = LOAD_TAG R1
  %25 = GET_TYPE %24
  STORE_POINTER R2, %25
  STORE_TAG R2, tstring
  JUMP bb_9
bb_9:
  JUMP_EQ_TAG R2, tstring, bb_13, bb_bytecode_1
bb_13:
  %43 = LOAD_POINTER R2
  %44 = LOAD_POINTER K2 ('number')
  JUMP_EQ_POINTER %43, %44, bb_12, bb_bytecode_1
bb_12:
  CHECK_TAG R0, tnumber, bb_fallback_14
  CHECK_TAG R1, tnumber, bb_fallback_14
  %50 = LOAD_DOUBLE R0
  %52 = ADD_NUM %50, R1
  STORE_DOUBLE R2, %52
  STORE_TAG R2, tnumber
  JUMP bb_15
bb_15:
  INTERRUPT 15u
  RETURN R2, 1i
bb_bytecode_1:
  STORE_TAG R2, tnil
  INTERRUPT 17u
  RETURN R2, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "AssertTypeGuard")
{
    // TODO: opportunity - CHECK_TRUTHY indirectly establishes that %1 is a number for CHECK_TAG in bb_5
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(a)
    assert(type(a) == "number")
    return a * 2
end
)"
               ),
        R"(
; function foo($arg0) line 2
bb_bytecode_0:
  CHECK_SAFE_ENV bb_fallback_3
  %1 = LOAD_TAG R0
  %2 = GET_TYPE %1
  STORE_POINTER R3, %2
  STORE_TAG R3, tstring
  JUMP bb_4
bb_4:
  %18 = LOAD_TAG R3
  %19 = LOAD_POINTER R3
  %20 = LOAD_POINTER K2 ('number')
  %21 = CMP_SPLIT_TVALUE %18, tstring, %19, %20, eq
  STORE_TAG R2, tboolean
  STORE_INT R2, %21
  JUMP bb_bytecode_2
bb_bytecode_2:
  CHECK_SAFE_ENV bb_fallback_7
  CHECK_TRUTHY R2, R2, exit(10)
  JUMP bb_8
bb_8:
  CHECK_TAG R0, tnumber, bb_fallback_11
  %45 = LOAD_DOUBLE R0
  %46 = ADD_NUM %45, %45
  STORE_DOUBLE R1, %46
  STORE_TAG R1, tnumber
  JUMP bb_12
bb_12:
  INTERRUPT 14u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "VectorConstantTag")
{
    ensureVectorFloat();

    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function vecrcp(a: vector)
    return vector(1, 2, 3) + a
end
)"),
        R"(
; function vecrcp($arg0) line 2
bb_0:
  CHECK_TAG R0, tvector, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %4 = LOAD_TVALUE K0 (1, 2, 3), 0i, tvector
  %11 = LOAD_TVALUE R0, 0i, tvector
  %12 = ADD_VEC %4, %11
  %13 = TAG_VECTOR %12
  STORE_TVALUE R1, %13
  INTERRUPT 2u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "VectorNamecall")
{
    ScopedFastFlag emitCallFb{FFlag::LuauEmitCallFeedback, true};

    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function abs(a: vector)
    return a:Abs()
end
)"),
        R"(
; function abs($arg0) line 2
bb_0:
  CHECK_TAG R0, tvector, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  FALLBACK_NAMECALL 0u, R1, R0, K0 ('Abs')
  INTERRUPT 2u
  SET_SAVEDPC 3u
  CALL R1, 1i, -1i
  INTERRUPT 3u
  RETURN R1, -1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "VectorRandomProp")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(a: vector)
    return a.XX + a.YY + a.ZZ
end
)"),
        R"(
; function foo($arg0) line 2
bb_0:
  CHECK_TAG R0, tvector, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  FALLBACK_GETTABLEKS 0u, R3, R0, K0 ('XX')
  FALLBACK_GETTABLEKS 2u, R4, R0, K1 ('YY')
  CHECK_TAG R3, tnumber, bb_fallback_3
  CHECK_TAG R4, tnumber, bb_fallback_3
  %14 = LOAD_DOUBLE R3
  %16 = ADD_NUM %14, R4
  STORE_DOUBLE R2, %16
  STORE_TAG R2, tnumber
  JUMP bb_4
bb_4:
  FALLBACK_GETTABLEKS 5u, R3, R0, K2 ('ZZ')
  CHECK_TAG R2, tnumber, bb_fallback_5
  CHECK_TAG R3, tnumber, bb_fallback_5
  %30 = LOAD_DOUBLE R2
  %32 = ADD_NUM %30, R3
  STORE_DOUBLE R1, %32
  STORE_TAG R1, tnumber
  JUMP bb_6
bb_6:
  INTERRUPT 8u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "VectorCustomAccess")
{
    ensureVectorFloat();

    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function vec3magn(a: vector)
    return a.Magnitude * 3
end
)"),
        R"(
; function vec3magn($arg0) line 2
bb_0:
  CHECK_TAG R0, tvector, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %6 = LOAD_FLOAT R0, 0i
  %7 = LOAD_FLOAT R0, 4i
  %8 = LOAD_FLOAT R0, 8i
  %9 = MUL_FLOAT %6, %6
  %10 = MUL_FLOAT %7, %7
  %11 = MUL_FLOAT %8, %8
  %12 = ADD_FLOAT %9, %10
  %13 = ADD_FLOAT %12, %11
  %14 = SQRT_FLOAT %13
  %15 = FLOAT_TO_NUM %14
  %21 = MUL_NUM %15, 3
  STORE_DOUBLE R1, %21
  STORE_TAG R1, tnumber
  INTERRUPT 3u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "VectorCustomNamecall")
{
    ensureVectorFloat();

    ScopedFastFlag emitCallFb{FFlag::LuauEmitCallFeedback, true};

    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function vec3dot(a: vector, b: vector)
    return (a:Dot(b))
end
)"),
        R"(
; function vec3dot($arg0, $arg1) line 2
bb_0:
  CHECK_TAG R0, tvector, exit(entry)
  CHECK_TAG R1, tvector, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %6 = LOAD_TVALUE R1, 0i, tvector
  %12 = LOAD_FLOAT R0, 0i
  %13 = EXTRACT_VEC %6, 0i
  %14 = MUL_FLOAT %12, %13
  %15 = LOAD_FLOAT R0, 4i
  %16 = EXTRACT_VEC %6, 1i
  %17 = MUL_FLOAT %15, %16
  %18 = LOAD_FLOAT R0, 8i
  %19 = EXTRACT_VEC %6, 2i
  %20 = MUL_FLOAT %18, %19
  %21 = ADD_FLOAT %14, %17
  %22 = ADD_FLOAT %21, %20
  %23 = FLOAT_TO_NUM %22
  STORE_DOUBLE R2, %23
  STORE_TAG R2, tnumber
  INTERRUPT 5u
  RETURN R2, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "VectorCustomNamecall2")
{
    ensureVectorFloat();

    ScopedFastFlag emitCallFb{FFlag::LuauEmitCallFeedback, true};

    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function vec3dot(a: vector)
    return (a:Dot(vector.create(1, 2, 3)))
end
)"),
        R"(
; function vec3dot($arg0) line 2
bb_0:
  CHECK_TAG R0, tvector, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %10 = LOAD_FLOAT R0, 0i
  %13 = LOAD_FLOAT R0, 4i
  %15 = ADD_FLOAT %13, %13
  %16 = LOAD_FLOAT R0, 8i
  %18 = MUL_FLOAT %16, 3
  %19 = ADD_FLOAT %10, %15
  %20 = ADD_FLOAT %19, %18
  %21 = FLOAT_TO_NUM %20
  STORE_DOUBLE R1, %21
  STORE_TAG R1, tnumber
  INTERRUPT 5u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "VectorCustomAccessChain")
{
    ensureVectorFloat();

    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(a: vector, b: vector)
    return a.Unit * b.Magnitude
end
)"),
        R"(
; function foo($arg0, $arg1) line 2
bb_0:
  CHECK_TAG R0, tvector, exit(entry)
  CHECK_TAG R1, tvector, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %8 = LOAD_FLOAT R0, 0i
  %9 = LOAD_FLOAT R0, 4i
  %10 = LOAD_FLOAT R0, 8i
  %11 = MUL_FLOAT %8, %8
  %12 = MUL_FLOAT %9, %9
  %13 = MUL_FLOAT %10, %10
  %14 = ADD_FLOAT %11, %12
  %15 = ADD_FLOAT %14, %13
  %16 = SQRT_FLOAT %15
  %17 = DIV_FLOAT 1, %16
  %18 = MUL_FLOAT %8, %17
  %19 = MUL_FLOAT %9, %17
  %20 = MUL_FLOAT %10, %17
  STORE_VECTOR R3, %18, %19, %20
  STORE_TAG R3, tvector
  %25 = LOAD_FLOAT R1, 0i
  %26 = LOAD_FLOAT R1, 4i
  %27 = LOAD_FLOAT R1, 8i
  %28 = MUL_FLOAT %25, %25
  %29 = MUL_FLOAT %26, %26
  %30 = MUL_FLOAT %27, %27
  %31 = ADD_FLOAT %28, %29
  %32 = ADD_FLOAT %31, %30
  %33 = SQRT_FLOAT %32
  %41 = LOAD_TVALUE R3, 0i, tvector
  %44 = FLOAT_TO_VEC %33
  %45 = MUL_VEC %41, %44
  %46 = TAG_VECTOR %45
  STORE_TVALUE R2, %46
  INTERRUPT 5u
  RETURN R2, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "VectorCustomNamecallChain")
{
    ensureVectorFloat();

    ScopedFastFlag emitCallFb{FFlag::LuauEmitCallFeedback, true};

    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(n: vector, b: vector, t: vector)
    return n:Cross(t):Dot(b) + 1
end
)"),
        R"(
; function foo($arg0, $arg1, $arg2) line 2
bb_0:
  CHECK_TAG R0, tvector, exit(entry)
  CHECK_TAG R1, tvector, exit(entry)
  CHECK_TAG R2, tvector, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %8 = LOAD_TVALUE R2, 0i, tvector
  %14 = LOAD_FLOAT R0, 0i
  %15 = EXTRACT_VEC %8, 0i
  %16 = LOAD_FLOAT R0, 4i
  %17 = EXTRACT_VEC %8, 1i
  %18 = LOAD_FLOAT R0, 8i
  %19 = EXTRACT_VEC %8, 2i
  %20 = MUL_FLOAT %16, %19
  %21 = MUL_FLOAT %18, %17
  %22 = SUB_FLOAT %20, %21
  %23 = MUL_FLOAT %18, %15
  %24 = MUL_FLOAT %14, %19
  %25 = SUB_FLOAT %23, %24
  %26 = MUL_FLOAT %14, %17
  %27 = MUL_FLOAT %16, %15
  %28 = SUB_FLOAT %26, %27
  STORE_VECTOR R4, %22, %25, %28
  STORE_TAG R4, tvector
  %31 = LOAD_TVALUE R1, 0i, tvector
  %37 = LOAD_FLOAT R4, 0i
  %38 = EXTRACT_VEC %31, 0i
  %39 = MUL_FLOAT %37, %38
  %40 = LOAD_FLOAT R4, 4i
  %41 = EXTRACT_VEC %31, 1i
  %42 = MUL_FLOAT %40, %41
  %43 = LOAD_FLOAT R4, 8i
  %44 = EXTRACT_VEC %31, 2i
  %45 = MUL_FLOAT %43, %44
  %46 = ADD_FLOAT %39, %42
  %47 = ADD_FLOAT %46, %45
  %48 = FLOAT_TO_NUM %47
  %54 = ADD_NUM %48, 1
  STORE_DOUBLE R3, %54
  STORE_TAG R3, tnumber
  INTERRUPT 11u
  RETURN R3, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "VectorCustomNamecallChain2")
{
    ensureVectorFloat();

    ScopedFastFlag emitCallFb{FFlag::LuauEmitCallFeedback, true};
    ScopedFastFlag luauCodegenPropagateFallbackTags{FFlag::LuauCodegenPropagateFallbackTags, true};

    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
type Vertex = {n: vector, b: vector}

local function foo(v: Vertex, t: vector)
    return v.n:Cross(t):Dot(v.b) + 1
end
)"),
        R"(
; function foo($arg0, $arg1) line 4
bb_0:
  CHECK_TAG R0, ttable, exit(entry)
  CHECK_TAG R1, tvector, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %8 = LOAD_POINTER R0
  %9 = GET_SLOT_NODE_ADDR %8, 0u, K1 ('n')
  CHECK_SLOT_MATCH %9, K1 ('n'), bb_fallback_3
  %11 = LOAD_TVALUE %9, 0i
  STORE_TVALUE R3, %11
  JUMP bb_4
bb_4:
  %16 = LOAD_TVALUE R1, 0i, tvector
  CHECK_TAG R3, tvector, bb_exit_7
   ; exit sync: R5, {%16}
  %22 = LOAD_FLOAT R3, 0i
  %23 = EXTRACT_VEC %16, 0i
  %24 = LOAD_FLOAT R3, 4i
  %25 = EXTRACT_VEC %16, 1i
  %26 = LOAD_FLOAT R3, 8i
  %27 = EXTRACT_VEC %16, 2i
  %28 = MUL_FLOAT %24, %27
  %29 = MUL_FLOAT %26, %25
  %30 = SUB_FLOAT %28, %29
  %31 = MUL_FLOAT %26, %23
  %32 = MUL_FLOAT %22, %27
  %33 = SUB_FLOAT %31, %32
  %34 = MUL_FLOAT %22, %25
  %35 = MUL_FLOAT %24, %23
  %36 = SUB_FLOAT %34, %35
  STORE_VECTOR R3, %30, %33, %36
  %41 = LOAD_POINTER R0
  %42 = GET_SLOT_NODE_ADDR %41, 7u, K3 ('b')
  CHECK_SLOT_MATCH %42, K3 ('b'), bb_fallback_5
  %44 = LOAD_TVALUE %42, 0i
  STORE_TVALUE R5, %44
  JUMP bb_6
bb_6:
  CHECK_TAG R5, tvector, exit(9)
  %53 = LOAD_FLOAT R3, 0i
  %54 = LOAD_FLOAT R5, 0i
  %55 = MUL_FLOAT %53, %54
  %56 = LOAD_FLOAT R3, 4i
  %57 = LOAD_FLOAT R5, 4i
  %58 = MUL_FLOAT %56, %57
  %59 = LOAD_FLOAT R3, 8i
  %60 = LOAD_FLOAT R5, 8i
  %61 = MUL_FLOAT %59, %60
  %62 = ADD_FLOAT %55, %58
  %63 = ADD_FLOAT %62, %61
  %64 = FLOAT_TO_NUM %63
  %70 = ADD_NUM %64, 1
  STORE_DOUBLE R2, %70
  STORE_TAG R2, tnumber
  INTERRUPT 14u
  RETURN R2, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "VectorLoadFloatPropagation")
{
    ensureVectorFloat();
    ensureVectorSize3();

    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(t: vector)
    t = t * 2
    return vector.create(t.x, t.y, t.x)
end
)"),
        R"(
; function foo($arg0) line 2
bb_0:
  CHECK_TAG R0, tvector, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %6 = LOAD_TVALUE R0, 0i, tvector
  %8 = FLOAT_TO_VEC 2
  %9 = MUL_VEC %6, %8
  %10 = TAG_VECTOR %9
  STORE_TVALUE R0, %10
  %14 = EXTRACT_VEC %9, 0i
  %15 = FLOAT_TO_NUM %14
  STORE_DOUBLE R2, %15
  STORE_TAG R2, tnumber
  %20 = EXTRACT_VEC %9, 1i
  %21 = FLOAT_TO_NUM %20
  STORE_DOUBLE R3, %21
  STORE_TAG R3, tnumber
  STORE_DOUBLE R4, %15
  STORE_TAG R4, tnumber
  CHECK_SAFE_ENV bb_fallback_3
  STORE_VECTOR R1, %14, %20, %14
  STORE_TAG R1, tvector
  JUMP bb_4
bb_4:
  INTERRUPT 11u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "VectorLibraryChain")
{
    ensureVectorFloat();
    ensureVectorSize3();

    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(a: vector, b: vector)
    return vector.normalize(a) * (vector.magnitude(b) + vector.dot(a, b))
end
)"),
        R"(
; function foo($arg0, $arg1) line 2
bb_0:
  CHECK_TAG R0, tvector, exit(entry)
  CHECK_TAG R1, tvector, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  CHECK_SAFE_ENV bb_fallback_3
  SET_SAVEDPC 1u
  %8 = INVOKE_FASTCALL 79u, R3, R0, undef, undef, 1i, 1i
  CHECK_FASTCALL_RES %8, bb_fallback_3
  JUMP bb_linear_15
bb_linear_15:
  %93 = LOAD_TVALUE R1, 0i, tvector
  %94 = DOT_VEC %93, %93
  %95 = SQRT_FLOAT %94
  %96 = FLOAT_TO_NUM %95
  %101 = LOAD_TVALUE R0, 0i, tvector
  %103 = DOT_VEC %101, %93
  %104 = FLOAT_TO_NUM %103
  %114 = ADD_NUM %96, %104
  CHECK_TAG R3, tvector, bb_exit_16
   ; exit sync: R6, R5, R4, {%104, %96, %114}
  %119 = LOAD_TVALUE R3, 0i, tvector
  %120 = NUM_TO_FLOAT %114
  %121 = FLOAT_TO_VEC %120
  %122 = MUL_VEC %119, %121
  %123 = TAG_VECTOR %122
  STORE_TVALUE R2, %123
  INTERRUPT 19u
  RETURN R2, 1i
bb_4:
  CHECK_SAFE_ENV bb_fallback_7
  %25 = LOAD_TVALUE R1, 0i, tvector
  %26 = DOT_VEC %25, %25
  %27 = SQRT_FLOAT %26
  %28 = FLOAT_TO_NUM %27
  STORE_DOUBLE R5, %28
  STORE_TAG R5, tnumber
  JUMP bb_8
bb_8:
  CHECK_SAFE_ENV bb_fallback_11
  %49 = LOAD_TVALUE R0, 0i, tvector
  %50 = LOAD_TVALUE R1, 0i, tvector
  %51 = DOT_VEC %49, %50
  %52 = FLOAT_TO_NUM %51
  STORE_DOUBLE R6, %52
  STORE_TAG R6, tnumber
  JUMP bb_12
bb_12:
  CHECK_TAG R5, tnumber, exit(17)
  CHECK_TAG R6, tnumber, exit(17)
  %74 = LOAD_DOUBLE R5
  %76 = ADD_NUM %74, R6
  CHECK_TAG R3, tvector, bb_exit_17
   ; exit sync: R4, {%76}
  %83 = LOAD_TVALUE R3, 0i, tvector
  %85 = NUM_TO_FLOAT %76
  %86 = FLOAT_TO_VEC %85
  %87 = MUL_VEC %83, %86
  %88 = TAG_VECTOR %87
  STORE_TVALUE R2, %88
  INTERRUPT 19u
  RETURN R2, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "VectorIdiv")
{
    ensureVectorFloat();
    ensureVectorSize3();

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(x: vector): vector
    x *= 1.5
    x -= x // 1
    x -= vector.create(0.5, 0.5, 0.5)
    return x
end
)"
               ),
        R"(
; function foo($arg0) line 2
bb_0:
  CHECK_TAG R0, tvector, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %6 = LOAD_TVALUE R0, 0i, tvector
  %8 = FLOAT_TO_VEC 1.5
  %9 = MUL_VEC %6, %8
  %16 = FLOAT_TO_VEC 1
  %17 = IDIV_VEC %9, %16
  %26 = SUB_VEC %9, %17
  %29 = LOAD_TVALUE K2 (0.5, 0.5, 0.5), 0i, tvector
  %37 = SUB_VEC %26, %29
  %38 = TAG_VECTOR %37
  STORE_TVALUE R0, %38
  INTERRUPT 5u
  RETURN R0, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "VectorNumberMixed1")
{
    ensureVectorFloat();

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(vectors: {vector}, i)
    local t = i / 100
    return vectors[i] * (1 - t)
end
)",
                   false,
                   1,
                   2,
                   true
               ),
        R"(
; function foo($arg0, $arg1) line 2
bb_0:
  CHECK_TAG R0, ttable, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  CHECK_TAG R1, tnumber, bb_fallback_3
  %6 = LOAD_DOUBLE R1
  %7 = DIV_NUM %6, 100
  STORE_DOUBLE R2, %7
  STORE_TAG R2, tnumber
  JUMP bb_linear_11
bb_linear_11:
  %60 = LOAD_POINTER R0
  %62 = TRY_NUM_TO_INDEX %6, bb_fallback_5
  %63 = SUB_INT %62, 1i
  CHECK_ARRAY_SIZE %60, %63, bb_fallback_5
  CHECK_NO_METATABLE %60, bb_fallback_5
  %66 = GET_ARR_ADDR %60, %63
  %67 = LOAD_TVALUE %66
  STORE_TVALUE R4, %67
  %73 = SUB_NUM 1, %7
  CHECK_TAG R4, tvector, bb_exit_12
   ; exit sync: R5, {%73}
  %83 = NUM_TO_FLOAT %73
  %84 = FLOAT_TO_VEC %83
  %85 = MUL_VEC %67, %84
  %86 = TAG_VECTOR %85
  STORE_TVALUE R3, %86
  INTERRUPT 4u
  RETURN R3, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "VectorNumberMixed2")
{
    ensureVectorFloat();

    assemblyOptions.includeOutlinedCode = true;

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(vectors: {vector}, i: string, t: {})
    return vectors[i] * (1 - t)
end
)"
               ),
        R"(
; function foo($arg0, $arg1, $arg2) line 2
bb_0:
  CHECK_TAG R0, ttable, exit(entry)
  CHECK_TAG R1, tstring, exit(entry)
  CHECK_TAG R2, ttable, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  SET_SAVEDPC 1u
  GET_TABLE R4, R0, R1
  JUMP bb_fallback_3
bb_4:
  CHECK_TAG R4, tvector, exit(2)
  CHECK_TAG R5, tnumber, bb_fallback_5
  %24 = LOAD_TVALUE R4, 0i, tvector
  %25 = LOAD_DOUBLE R5
  %26 = NUM_TO_FLOAT %25
  %27 = FLOAT_TO_VEC %26
  %28 = MUL_VEC %24, %27
  %29 = TAG_VECTOR %28
  STORE_TVALUE R3, %29
  JUMP bb_6
bb_6:
  INTERRUPT 3u
  RETURN R3, 1i
bb_fallback_3:
  SET_SAVEDPC 2u
  DO_ARITH R5, K0 (1), R2, 9i
  JUMP bb_4
bb_fallback_5:
  SET_SAVEDPC 3u
  DO_ARITH R3, R4, R5, 10i
  JUMP bb_6
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "VectorReverseOps")
{
    ensureVectorFloat();

    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function vecrcp(a: vector)
    return vector(1, 2, 3) + a
end
)"),
        R"(
; function vecrcp($arg0) line 2
bb_0:
  CHECK_TAG R0, tvector, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %4 = LOAD_TVALUE K0 (1, 2, 3), 0i, tvector
  %11 = LOAD_TVALUE R0, 0i, tvector
  %12 = ADD_VEC %4, %11
  %13 = TAG_VECTOR %12
  STORE_TVALUE R1, %13
  INTERRUPT 2u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "UserDataGetIndex")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function getxy(a: Point)
    return a.x + a.y
end
)"),
        R"(
; function getxy($arg0) line 2
bb_0:
  CHECK_TAG R0, tuserdata, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  FALLBACK_GETTABLEKS 0u, R2, R0, K0 ('x')
  FALLBACK_GETTABLEKS 2u, R3, R0, K1 ('y')
  CHECK_TAG R2, tnumber, bb_fallback_3
  CHECK_TAG R3, tnumber, bb_fallback_3
  %14 = LOAD_DOUBLE R2
  %16 = ADD_NUM %14, R3
  STORE_DOUBLE R1, %16
  STORE_TAG R1, tnumber
  JUMP bb_4
bb_4:
  INTERRUPT 5u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "UserDataSetIndex")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function setxy(a: Point)
    a.x = 3
    a.y = 4
end
)"),
        R"(
; function setxy($arg0) line 2
bb_0:
  CHECK_TAG R0, tuserdata, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  STORE_DOUBLE R1, 3
  STORE_TAG R1, tnumber
  FALLBACK_SETTABLEKS 1u, R1, R0, K0 ('x')
  STORE_DOUBLE R1, 4
  FALLBACK_SETTABLEKS 4u, R1, R0, K1 ('y')
  INTERRUPT 6u
  RETURN R0, 0i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "UserDataNamecall")
{
    ScopedFastFlag emitCallFb{FFlag::LuauEmitCallFeedback, true};

    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function getxy(a: Point)
    return a:GetX() + a:GetY()
end
)"),
        R"(
; function getxy($arg0) line 2
bb_0:
  CHECK_TAG R0, tuserdata, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  FALLBACK_NAMECALL 0u, R2, R0, K0 ('GetX')
  INTERRUPT 2u
  SET_SAVEDPC 4u
  CALL R2, 1i, 1i
  FALLBACK_NAMECALL 4u, R3, R0, K1 ('GetY')
  INTERRUPT 6u
  SET_SAVEDPC 8u
  CALL R3, 1i, 1i
  CHECK_TAG R2, tnumber, bb_fallback_3
  CHECK_TAG R3, tnumber, bb_fallback_3
  %20 = LOAD_DOUBLE R2
  %22 = ADD_NUM %20, R3
  STORE_DOUBLE R1, %22
  STORE_TAG R1, tnumber
  JUMP bb_4
bb_4:
  INTERRUPT 9u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "EntryBlockChecksAreNotInferred")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
function eq(a: number, b: number, limit)
  if not limit then limit = 0.125 end
  return math.abs(a - b) <= limit
end
)"
               ),
        R"(
; function eq($arg0, $arg1, $arg2) line 2
bb_0:
  CHECK_TAG R0, tnumber, exit(entry)
  CHECK_TAG R1, tnumber, exit(entry)
  JUMP bb_5
bb_5:
  JUMP bb_bytecode_1
bb_bytecode_1:
  JUMP_IF_TRUTHY R2, bb_bytecode_2, bb_6
bb_6:
  STORE_DOUBLE R2, 0.125
  STORE_TAG R2, tnumber
  JUMP bb_bytecode_2
bb_bytecode_2:
  %14 = LOAD_DOUBLE R0
  %16 = SUB_NUM %14, R1
  STORE_DOUBLE R5, %16
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_7
  %23 = ABS_NUM %16
  STORE_DOUBLE R4, %23
  STORE_TAG R4, tnumber
  JUMP bb_8
bb_8:
  CHECK_TAG R4, tnumber, bb_fallback_12
  CHECK_TAG R2, tnumber, bb_fallback_12
  %42 = LOAD_DOUBLE R2
  JUMP_CMP_NUM R4, %42, le, bb_bytecode_3, bb_11
bb_11:
  STORE_INT R3, 0i
  STORE_TAG R3, tboolean
  JUMP bb_bytecode_4
bb_bytecode_3:
  STORE_INT R3, 1i
  STORE_TAG R3, tboolean
  JUMP bb_bytecode_4
bb_bytecode_4:
  INTERRUPT 11u
  RETURN R3, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "EntryBlockChecksWithOptional1")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
function eq(a: number?, b: number)
  return if a ~= nil then a + b else b
end
)"
               ),
        R"(
; function eq($arg0, $arg1) line 2
bb_0:
  %0 = LOAD_TAG R0
  JUMP_EQ_TAG %0, tnil, bb_3, bb_4
bb_4:
  CHECK_TAG %0, tnumber, exit(entry)
  JUMP bb_3
bb_3:
  CHECK_TAG R1, tnumber, exit(entry)
  JUMP bb_5
bb_5:
  JUMP bb_bytecode_1
bb_bytecode_1:
  JUMP_EQ_TAG R0, tnil, bb_bytecode_2, bb_6
bb_6:
  CHECK_TAG R0, tnumber, exit(2)
  %14 = LOAD_DOUBLE R0
  %16 = ADD_NUM %14, R1
  STORE_DOUBLE R2, %16
  STORE_TAG R2, tnumber
  INTERRUPT 3u
  RETURN R2, 1i
bb_bytecode_2:
  %21 = LOAD_TVALUE R1, 0i, tnumber
  STORE_TVALUE R2, %21
  INTERRUPT 5u
  RETURN R2, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "EntryBlockChecksWithOptional2")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
function eq(a: number, b: number?)
  return if b ~= nil then a + b else a
end
)"
               ),
        R"(
; function eq($arg0, $arg1) line 2
bb_0:
  CHECK_TAG R0, tnumber, exit(entry)
  %2 = LOAD_TAG R1
  JUMP_EQ_TAG %2, tnil, bb_3, bb_4
bb_4:
  CHECK_TAG %2, tnumber, exit(entry)
  JUMP bb_3
bb_3:
  JUMP bb_bytecode_1
bb_bytecode_1:
  JUMP_EQ_TAG R1, tnil, bb_bytecode_2, bb_5
bb_5:
  CHECK_TAG R1, tnumber, exit(2)
  %13 = LOAD_DOUBLE R0
  %15 = ADD_NUM %13, R1
  STORE_DOUBLE R2, %15
  STORE_TAG R2, tnumber
  INTERRUPT 3u
  RETURN R2, 1i
bb_bytecode_2:
  %20 = LOAD_TVALUE R0, 0i, tnumber
  STORE_TVALUE R2, %20
  INTERRUPT 5u
  RETURN R2, 1i
)"
    );
}

// This test captures how R4 check was previously incorrectly removed
TEST_CASE_FIXTURE(LoweringFixture, "EntryBlockChecksWithOptional3")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
function eq(a: string, b: string?, c: {string}?, d: number?, e: {x: number}, f: number?)
  if b then
    return a
  else
    return c
  end
end
)"
               ),
        R"(
; function eq($arg0, $arg1, $arg2, $arg3, $arg4, $arg5) line 2
bb_0:
  CHECK_TAG R0, tstring, exit(entry)
  %2 = LOAD_TAG R1
  JUMP_EQ_TAG %2, tnil, bb_3, bb_4
bb_4:
  CHECK_TAG %2, tstring, exit(entry)
  JUMP bb_3
bb_3:
  %6 = LOAD_TAG R2
  JUMP_EQ_TAG %6, tnil, bb_5, bb_6
bb_6:
  CHECK_TAG %6, ttable, exit(entry)
  JUMP bb_5
bb_5:
  %10 = LOAD_TAG R3
  JUMP_EQ_TAG %10, tnil, bb_7, bb_8
bb_8:
  CHECK_TAG %10, tnumber, exit(entry)
  JUMP bb_7
bb_7:
  CHECK_TAG R4, ttable, exit(entry)
  %16 = LOAD_TAG R5
  JUMP_EQ_TAG %16, tnil, bb_9, bb_10
bb_10:
  CHECK_TAG %16, tnumber, exit(entry)
  JUMP bb_9
bb_9:
  JUMP bb_bytecode_1
bb_bytecode_1:
  JUMP_IF_FALSY R1, bb_bytecode_2, bb_11
bb_11:
  INTERRUPT 1u
  RETURN R0, 1i
bb_bytecode_2:
  INTERRUPT 2u
  RETURN R2, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "ExplicitUpvalueAndLocalTypes")
{
    ensureVectorFloat();

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local y: vector = ...

local function getsum(t)
    local x: vector = t
    return x.X + x.Y + y.X + y.Y
end
)",
                   /* includeIrTypes */ true
               ),
        R"(
; function getsum($arg0) line 4
; U0: vector
; R0: vector from 0 to 14
bb_bytecode_0:
  CHECK_TAG R0, tvector, exit(0)
  %2 = LOAD_FLOAT R0, 0i
  %3 = FLOAT_TO_NUM %2
  %8 = LOAD_FLOAT R0, 4i
  %9 = FLOAT_TO_NUM %8
  %18 = ADD_NUM %3, %9
  %21 = GET_UPVALUE U0
  STORE_TVALUE R4, %21
  CHECK_TAG R4, tvector, bb_exit_1
   ; exit sync: R5, R3, {%9, %18}
  %25 = EXTRACT_VEC %21, 0i
  %26 = FLOAT_TO_NUM %25
  %35 = ADD_NUM %18, %26
  STORE_TVALUE R3, %21
  %42 = EXTRACT_VEC %21, 1i
  %43 = FLOAT_TO_NUM %42
  %52 = ADD_NUM %35, %43
  STORE_DOUBLE R1, %52
  STORE_TAG R1, tnumber
  INTERRUPT 13u
  RETURN R1, 1i
)"
    );
}

// In this test, we are able to only load t[n] and u[n] once, since there are no modifications
// Our fast-path lowering checks that there is no metatable and all accesses are in bounds
TEST_CASE_FIXTURE(LoweringFixture, "DuplicateArrayLoads1")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(n: number, t: {number}, u: {number})
    return t[n] * t[n] + u[n] * u[n]
end
)",
                   false,
                   1,
                   2,
                   true
               ),
        R"(
; function foo($arg0, $arg1, $arg2) line 2
bb_0:
  CHECK_TAG R0, tnumber, exit(entry)
  CHECK_TAG R1, ttable, exit(entry)
  CHECK_TAG R2, ttable, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %12 = LOAD_POINTER R1
  %13 = LOAD_DOUBLE R0
  %14 = TRY_NUM_TO_INDEX %13, bb_fallback_3
  %15 = SUB_INT %14, 1i
  CHECK_ARRAY_SIZE %12, %15, bb_fallback_3
  CHECK_NO_METATABLE %12, bb_fallback_3
  %18 = GET_ARR_ADDR %12, %15
  %19 = LOAD_TVALUE %18
  STORE_TVALUE R5, %19
  JUMP bb_linear_17
bb_linear_17:
  STORE_TVALUE R6, %19
  CHECK_TAG R5, tnumber, bb_fallback_7
  %131 = LOAD_DOUBLE R5
  %133 = MUL_NUM %131, %131
  STORE_DOUBLE R4, %133
  STORE_TAG R4, tnumber
  %137 = LOAD_POINTER R2
  CHECK_ARRAY_SIZE %137, %15, bb_fallback_9
  CHECK_NO_METATABLE %137, bb_fallback_9
  %143 = GET_ARR_ADDR %137, %15
  %144 = LOAD_TVALUE %143
  STORE_TVALUE R6, %144
  STORE_TVALUE R7, %144
  CHECK_TAG R6, tnumber, bb_fallback_13
  %161 = LOAD_DOUBLE R6
  %163 = MUL_NUM %161, %161
  %173 = ADD_NUM %133, %163
  STORE_DOUBLE R3, %173
  STORE_TAG R3, tnumber
  INTERRUPT 7u
  RETURN R3, 1i
)"
    );
}

// In this test, we only load t[a][b] once, since there are no modifications
// Our fast-path lowering checks that there is no metatable and all accesses are in bounds
TEST_CASE_FIXTURE(LoweringFixture, "DuplicateArrayLoads2")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function test(t, a: number, b: number)
    return t[a][b].x + t[a][b].y + t[a][b].z
end
)",
                   false,
                   1,
                   2,
                   true
               ),
        R"(
; function test($arg0, $arg1, $arg2) line 2
bb_0:
  CHECK_TAG R1, tnumber, exit(entry)
  CHECK_TAG R2, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  CHECK_TAG R0, ttable, bb_fallback_3
  %10 = LOAD_POINTER R0
  %11 = LOAD_DOUBLE R1
  %12 = TRY_NUM_TO_INDEX %11, bb_fallback_3
  %13 = SUB_INT %12, 1i
  CHECK_ARRAY_SIZE %10, %13, bb_fallback_3
  CHECK_NO_METATABLE %10, bb_fallback_3
  %16 = GET_ARR_ADDR %10, %13
  %17 = LOAD_TVALUE %16
  STORE_TVALUE R6, %17
  JUMP bb_linear_25
bb_linear_25:
  CHECK_TAG R6, ttable, bb_fallback_5
  %168 = LOAD_POINTER R6
  %169 = LOAD_DOUBLE R2
  %170 = TRY_NUM_TO_INDEX %169, bb_fallback_5
  %171 = SUB_INT %170, 1i
  CHECK_ARRAY_SIZE %168, %171, bb_fallback_5
  CHECK_NO_METATABLE %168, bb_fallback_5
  %174 = GET_ARR_ADDR %168, %171
  %175 = LOAD_TVALUE %174
  STORE_TVALUE R5, %175
  CHECK_TAG R5, ttable, bb_fallback_7
  %180 = LOAD_POINTER R5
  %181 = GET_SLOT_NODE_ADDR %180, 2u, K0 ('x')
  CHECK_SLOT_MATCH %181, K0 ('x'), bb_fallback_7
  %183 = LOAD_TVALUE %181, 0i
  STORE_TVALUE R5, %183
  STORE_TVALUE R6, %175
  %213 = GET_SLOT_NODE_ADDR %180, 6u, K1 ('y')
  CHECK_SLOT_MATCH %213, K1 ('y'), bb_fallback_13
  %215 = LOAD_TVALUE %213, 0i
  STORE_TVALUE R6, %215
  CHECK_TAG R5, tnumber, bb_fallback_15
  CHECK_TAG R6, tnumber, bb_fallback_15
  %222 = LOAD_DOUBLE R5
  %224 = ADD_NUM %222, R6
  STORE_DOUBLE R4, %224
  STORE_TAG R4, tnumber
  STORE_TVALUE R5, %175
  %255 = GET_SLOT_NODE_ADDR %180, 11u, K2 ('z')
  CHECK_SLOT_MATCH %255, K2 ('z'), bb_fallback_21
  %257 = LOAD_TVALUE %255, 0i
  STORE_TVALUE R5, %257
  CHECK_TAG R5, tnumber, bb_fallback_23
  %266 = ADD_NUM %224, R5
  STORE_DOUBLE R3, %266
  STORE_TAG R3, tnumber
  INTERRUPT 14u
  RETURN R3, 1i
)"
    );
}

// This test shows that writes to separate array elements do not interfere with each other
// Our fast-path lowering checks that there is no metatable and all accesses are in bounds
TEST_CASE_FIXTURE(LoweringFixture, "DuplicateArrayLoads3")
{
    ensureVectorSize3();

    // TODO: opportunity - only one array size check should be enough here
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function test(t: { x: number, y: number }, a: number)
    t[1] += a
    t[2] += a * a

    t[1] = t[1] - t[2]
end
)",
                   false,
                   1,
                   2,
                   true
               ),
        R"(
; function test($arg0, $arg1) line 2
bb_0:
  CHECK_TAG R0, ttable, exit(entry)
  CHECK_TAG R1, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %8 = LOAD_POINTER R0
  CHECK_ARRAY_SIZE %8, 0i, bb_fallback_3
  CHECK_NO_METATABLE %8, bb_fallback_3
  %11 = GET_ARR_ADDR %8, 0i
  %12 = LOAD_TVALUE %11, 0i
  STORE_TVALUE R2, %12
  JUMP bb_linear_23
bb_linear_23:
  CHECK_TAG R2, tnumber, bb_fallback_5
  %144 = LOAD_DOUBLE R2
  %145 = LOAD_DOUBLE R1
  %146 = ADD_NUM %144, %145
  STORE_DOUBLE R2, %146
  CHECK_READONLY %8, bb_fallback_7
  STORE_SPLIT_TVALUE %11, tnumber, %146, 0i
  CHECK_ARRAY_SIZE %8, 1i, bb_fallback_9
  %162 = LOAD_TVALUE %11, 16i
  STORE_TVALUE R2, %162
  %166 = MUL_NUM %145, %145
  STORE_DOUBLE R3, %166
  STORE_TAG R3, tnumber
  CHECK_TAG R2, tnumber, bb_fallback_11
  %171 = LOAD_DOUBLE R2
  %172 = ADD_NUM %171, %166
  STORE_SPLIT_TVALUE %11, tnumber, %172, 16i
  %204 = SUB_NUM %146, %172
  STORE_SPLIT_TVALUE %11, tnumber, %204, 0i
  INTERRUPT 11u
  RETURN R0, 0i
)"
    );
}

// This test shows that writes to separate array elements using non-constant expressions don't have enough analysis to not interfere
// Our fast-path lowering checks that there is no metatable and all accesses are in bounds
TEST_CASE_FIXTURE(LoweringFixture, "DuplicateArrayLoads4")
{
    ScopedFastFlag luauCodegenPropagateFallbackTags{FFlag::LuauCodegenPropagateFallbackTags, true};

    // TODO: opportunity 1 - if we can figure out that i+1 is exactly 1 integer slot away, we can reduce arithmetic
    // TODO: opportunity 2 - store at [i + 1] shouldn't invalidate value at [i]
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function test(t: { x: number, y: number }, a: number, i: number)
    t[i] += a
    t[i + 1] += a * a

    t[i] = t[i] - t[i + 1]
end
)",
                   false,
                   1,
                   2,
                   true
               ),
        R"(
; function test($arg0, $arg1, $arg2) line 2
bb_0:
  CHECK_TAG R0, ttable, exit(entry)
  CHECK_TAG R1, tnumber, exit(entry)
  CHECK_TAG R2, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %12 = LOAD_POINTER R0
  %13 = LOAD_DOUBLE R2
  %14 = TRY_NUM_TO_INDEX %13, bb_fallback_3
  %15 = SUB_INT %14, 1i
  CHECK_ARRAY_SIZE %12, %15, bb_fallback_3
  CHECK_NO_METATABLE %12, bb_fallback_3
  %18 = GET_ARR_ADDR %12, %15
  %19 = LOAD_TVALUE %18
  STORE_TVALUE R3, %19
  JUMP bb_linear_23
bb_linear_23:
  CHECK_TAG R3, tnumber, bb_fallback_5
  %193 = LOAD_DOUBLE R3
  %194 = LOAD_DOUBLE R1
  %195 = ADD_NUM %193, %194
  STORE_DOUBLE R3, %195
  CHECK_READONLY %12, bb_fallback_7
  STORE_SPLIT_TVALUE %18, tnumber, %195
  %211 = ADD_NUM %13, 1
  STORE_DOUBLE R3, %211
  %215 = TRY_NUM_TO_INDEX %211, bb_fallback_9
  %216 = SUB_INT %215, 1i
  CHECK_ARRAY_SIZE %12, %216, bb_fallback_9
  %219 = GET_ARR_ADDR %12, %216
  %220 = LOAD_TVALUE %219
  STORE_TVALUE R4, %220
  %224 = MUL_NUM %194, %194
  STORE_DOUBLE R5, %224
  STORE_TAG R5, tnumber
  CHECK_TAG R4, tnumber, bb_fallback_11
  %229 = LOAD_DOUBLE R4
  %230 = ADD_NUM %229, %224
  STORE_SPLIT_TVALUE %219, tnumber, %230
  %252 = LOAD_TVALUE %18
  STORE_TVALUE R4, %252
  %265 = LOAD_TVALUE %219
  STORE_TVALUE R5, %265
  CHECK_TAG R4, tnumber, bb_fallback_19
  %272 = LOAD_DOUBLE R4
  %274 = SUB_NUM %272, %230
  STORE_SPLIT_TVALUE %18, tnumber, %274
  INTERRUPT 13u
  RETURN R0, 0i
)"
    );
}

// This test checks that in case of known constants, we propagate them in full and can recover the constant difference
// Our fast-path lowering checks that there is no metatable and all accesses are in bounds
TEST_CASE_FIXTURE(LoweringFixture, "DuplicateArrayLoads5")
{
    ensureVectorSize3();

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function test(t: { x: number, y: number })
    t[1] = 14
    t[2] = 28

    t[1] = t[1] - t[2]
end
)",
                   false,
                   1,
                   2,
                   true
               ),
        R"(
; function test($arg0) line 2
bb_0:
  CHECK_TAG R0, ttable, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  STORE_DOUBLE R1, 14
  STORE_TAG R1, tnumber
  %8 = LOAD_POINTER R0
  CHECK_ARRAY_SIZE %8, 0i, bb_fallback_3
  CHECK_NO_METATABLE %8, bb_fallback_3
  CHECK_READONLY %8, bb_fallback_3
  %12 = GET_ARR_ADDR %8, 0i
  STORE_SPLIT_TVALUE %12, tnumber, 14, 0i
  JUMP bb_linear_15
bb_linear_15:
  STORE_DOUBLE R1, 28
  CHECK_ARRAY_SIZE %8, 1i, bb_fallback_5
  STORE_SPLIT_TVALUE %12, tnumber, 28, 16i
  STORE_SPLIT_TVALUE %12, tnumber, -14, 0i
  INTERRUPT 8u
  RETURN R0, 0i
)"
    );
}

// This test checks that writing to constant index after an unknown one invalidates it
TEST_CASE_FIXTURE(LoweringFixture, "DuplicateArrayLoads6")
{
    ensureVectorSize3();

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function test(t: { x: number, y: number }, a: number, i: number)
    t[i] = 2
    t[2] = 4
    return t[i] * 2
end
)",
                   false,
                   1,
                   2,
                   true
               ),
        R"(
; function test($arg0, $arg1, $arg2) line 2
bb_0:
  CHECK_TAG R0, ttable, exit(entry)
  CHECK_TAG R1, tnumber, exit(entry)
  CHECK_TAG R2, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  STORE_DOUBLE R3, 2
  STORE_TAG R3, tnumber
  %14 = LOAD_POINTER R0
  %15 = LOAD_DOUBLE R2
  %16 = TRY_NUM_TO_INDEX %15, bb_fallback_3
  %17 = SUB_INT %16, 1i
  CHECK_ARRAY_SIZE %14, %17, bb_fallback_3
  CHECK_NO_METATABLE %14, bb_fallback_3
  CHECK_READONLY %14, bb_fallback_3
  %21 = GET_ARR_ADDR %14, %17
  STORE_SPLIT_TVALUE %21, tnumber, 2
  JUMP bb_linear_11
bb_linear_11:
  STORE_DOUBLE R3, 4
  CHECK_ARRAY_SIZE %14, 1i, bb_fallback_5
  %80 = GET_ARR_ADDR %14, 0i
  STORE_SPLIT_TVALUE %80, tnumber, 4, 16i
  %90 = LOAD_TVALUE %21
  STORE_TVALUE R4, %90
  CHECK_TAG R4, tnumber, bb_fallback_9
  %95 = LOAD_DOUBLE R4
  %96 = ADD_NUM %95, %95
  STORE_DOUBLE R3, %96
  INTERRUPT 6u
  RETURN R3, 1i
)"
    );
}

// This test checks that loads from the same keys are removed
// Note that CHECK_SLOT_MATCH ensures that key is in mainposition and not nil, so metatable is not triggered
TEST_CASE_FIXTURE(LoweringFixture, "TableNodeLoadStoreProp1")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function test(t: { u: number, a: { b: number, c: { x: number, y: number } } })
    return t.a.b + t.a.c.x + t.a.c.y
end
)",
                   false,
                   1,
                   2,
                   true
               ),
        R"(
; function test($arg0) line 2
bb_0:
  CHECK_TAG R0, ttable, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %6 = LOAD_POINTER R0
  %7 = GET_SLOT_NODE_ADDR %6, 0u, K0 ('a')
  CHECK_SLOT_MATCH %7, K0 ('a'), bb_fallback_3
  %9 = LOAD_TVALUE %7, 0i
  STORE_TVALUE R3, %9
  JUMP bb_linear_23
bb_linear_23:
  CHECK_TAG R3, ttable, bb_fallback_5
  %114 = LOAD_POINTER R3
  %115 = GET_SLOT_NODE_ADDR %114, 2u, K1 ('b')
  CHECK_SLOT_MATCH %115, K1 ('b'), bb_fallback_5
  %117 = LOAD_TVALUE %115, 0i
  STORE_TVALUE R3, %117
  STORE_TVALUE R4, %9
  %129 = GET_SLOT_NODE_ADDR %114, 6u, K2 ('c')
  CHECK_SLOT_MATCH %129, K2 ('c'), bb_fallback_9
  %131 = LOAD_TVALUE %129, 0i
  STORE_TVALUE R4, %131
  CHECK_TAG R4, ttable, bb_fallback_11
  %136 = LOAD_POINTER R4
  %137 = GET_SLOT_NODE_ADDR %136, 8u, K3 ('x')
  CHECK_SLOT_MATCH %137, K3 ('x'), bb_fallback_11
  %139 = LOAD_TVALUE %137, 0i
  STORE_TVALUE R4, %139
  CHECK_TAG R3, tnumber, bb_fallback_13
  CHECK_TAG R4, tnumber, bb_fallback_13
  %146 = LOAD_DOUBLE R3
  %148 = ADD_NUM %146, R4
  STORE_DOUBLE R2, %148
  STORE_TAG R2, tnumber
  STORE_TVALUE R3, %131
  %169 = GET_SLOT_NODE_ADDR %136, 15u, K4 ('y')
  CHECK_SLOT_MATCH %169, K4 ('y'), bb_fallback_19
  %171 = LOAD_TVALUE %169, 0i
  STORE_TVALUE R3, %171
  CHECK_TAG R3, tnumber, bb_fallback_21
  %180 = ADD_NUM %148, R3
  STORE_DOUBLE R1, %180
  STORE_TAG R1, tnumber
  INTERRUPT 18u
  RETURN R1, 1i
)"
    );
}

// This test checks that stores to distinct keys do not interfere with each other
// If the table pointers were different, they cannot have the same storage, if they are the same, slots are different
// Additionally, because we know that 'nil' is not stored, we do not have to recheck that with CHECK_NODE_VALUE
// Note that CHECK_SLOT_MATCH ensures that key is in mainposition and not nil, so metatable is not triggered
TEST_CASE_FIXTURE(LoweringFixture, "TableNodeLoadStoreProp2")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function test(t: { x: number, y: number }, a: number)
    t.x += a
    t.y += a * a

    t.x = t.x - t.y
end
)",
                   false,
                   1,
                   2,
                   true
               ),
        R"(
; function test($arg0, $arg1) line 2
bb_0:
  CHECK_TAG R0, ttable, exit(entry)
  CHECK_TAG R1, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %8 = LOAD_POINTER R0
  %9 = GET_SLOT_NODE_ADDR %8, 0u, K0 ('x')
  CHECK_SLOT_MATCH %9, K0 ('x'), bb_fallback_3
  %11 = LOAD_TVALUE %9, 0i
  STORE_TVALUE R2, %11
  JUMP bb_linear_23
bb_linear_23:
  CHECK_TAG R2, tnumber, bb_fallback_5
  %130 = LOAD_DOUBLE R2
  %131 = LOAD_DOUBLE R1
  %132 = ADD_NUM %130, %131
  STORE_DOUBLE R2, %132
  CHECK_READONLY %8, bb_fallback_7
  STORE_SPLIT_TVALUE %9, tnumber, %132, 0i
  %144 = GET_SLOT_NODE_ADDR %8, 5u, K1 ('y')
  CHECK_SLOT_MATCH %144, K1 ('y'), bb_fallback_9
  %146 = LOAD_TVALUE %144, 0i
  STORE_TVALUE R2, %146
  %150 = MUL_NUM %131, %131
  STORE_DOUBLE R3, %150
  STORE_TAG R3, tnumber
  CHECK_TAG R2, tnumber, bb_fallback_11
  %155 = LOAD_DOUBLE R2
  %156 = ADD_NUM %155, %150
  STORE_SPLIT_TVALUE %144, tnumber, %156, 0i
  %185 = SUB_NUM %132, %156
  STORE_SPLIT_TVALUE %9, tnumber, %185, 0i
  INTERRUPT 18u
  RETURN R0, 0i
)"
    );
}

// In this test we write an unknown key and t.x can be affected and has to be reloaded
TEST_CASE_FIXTURE(LoweringFixture, "TableNodeLoadStoreProp3")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function test(t: { x: number, y: number }, a: string)
    t.x = 2
    t[a] = 4
    return t.x * 2
end
)",
                   false,
                   1,
                   2,
                   true
               ),
        R"(
; function test($arg0, $arg1) line 2
bb_0:
  CHECK_TAG R0, ttable, exit(entry)
  CHECK_TAG R1, tstring, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  STORE_DOUBLE R2, 2
  STORE_TAG R2, tnumber
  %10 = LOAD_POINTER R0
  %11 = GET_SLOT_NODE_ADDR %10, 1u, K0 ('x')
  CHECK_SLOT_MATCH %11, K0 ('x'), bb_fallback_3
  CHECK_READONLY %10, bb_fallback_3
  STORE_SPLIT_TVALUE %11, tnumber, 2, 0i
  JUMP bb_linear_9
bb_linear_9:
  STORE_DOUBLE R2, 4
  SET_SAVEDPC 5u
  SET_TABLE R2, R0, R1
  %50 = LOAD_POINTER R0
  %51 = GET_SLOT_NODE_ADDR %50, 5u, K0 ('x')
  CHECK_SLOT_MATCH %51, K0 ('x'), bb_fallback_5
  %53 = LOAD_TVALUE %51, 0i
  STORE_TVALUE R3, %53
  CHECK_TAG R3, tnumber, bb_fallback_7
  %58 = LOAD_DOUBLE R3
  %59 = ADD_NUM %58, %58
  STORE_DOUBLE R2, %59
  INTERRUPT 8u
  RETURN R2, 1i
)"
    );
}

// In this test, write to an array part cannot affect the node part
// Our fast-path lowering checks that there is no metatable and all accesses are in bounds, so rehash is not possible
TEST_CASE_FIXTURE(LoweringFixture, "TableNodeLoadStoreProp4")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function test(t: { x: number, y: number }, a: string)
    t.x = 2
    t[1] = nil
    return t.x * 2
end
)",
                   false,
                   1,
                   2,
                   true
               ),
        R"(
; function test($arg0, $arg1) line 2
bb_0:
  CHECK_TAG R0, ttable, exit(entry)
  CHECK_TAG R1, tstring, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  STORE_DOUBLE R2, 2
  STORE_TAG R2, tnumber
  %10 = LOAD_POINTER R0
  %11 = GET_SLOT_NODE_ADDR %10, 1u, K0 ('x')
  CHECK_SLOT_MATCH %11, K0 ('x'), bb_fallback_3
  CHECK_READONLY %10, bb_fallback_3
  STORE_SPLIT_TVALUE %11, tnumber, 2, 0i
  JUMP bb_linear_11
bb_linear_11:
  STORE_TAG R2, tnil
  CHECK_ARRAY_SIZE %10, 0i, bb_fallback_5
  CHECK_NO_METATABLE %10, bb_fallback_5
  %62 = GET_ARR_ADDR %10, 0i
  %63 = LOAD_TVALUE R2, 0i, tnil
  STORE_TVALUE %62, %63, 0i
  STORE_DOUBLE R2, 4
  STORE_TAG R2, tnumber
  INTERRUPT 8u
  RETURN R2, 1i
)"
    );
}

// This test is based on an example of texture bilinear interpolation, t.w/t.h only have to be loaded once
TEST_CASE_FIXTURE(LoweringFixture, "TableNodeLoadStoreProp5")
{
    ensureVectorFloat();
    ensureVectorSize3();

    ScopedFastFlag luauCodegenPropagateFallbackTags{FFlag::LuauCodegenPropagateFallbackTags, true};
    ScopedFastFlag luauCompileReuseLocalRegs{FFlag::LuauCompileReuseLocalRegs, true};

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function test(t: { w: number, h: number, data: {vector} }, uv: vector)
    uv *= vector.create(t.w, t.h)
    uv -= vector.create(.5,.5)
    local uv0 = vector.floor(uv)
    local uv1 = vector.ceil(uv)
    local a = uv - uv0
    local x0 = uv0.x % t.w
    local x1 = uv1.x % t.w
    local y0 = (uv0.y % t.h) * t.w
    local y1 = (uv1.y % t.h) * t.w
    return a, x0, x1, y0, y1
end
)",
                   false,
                   1,
                   2,
                   true
               ),
        R"(
; function test($arg0, $arg1) line 2
bb_0:
  CHECK_TAG R0, ttable, exit(entry)
  CHECK_TAG R1, tvector, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %8 = LOAD_POINTER R0
  %9 = GET_SLOT_NODE_ADDR %8, 0u, K0 ('w')
  CHECK_SLOT_MATCH %9, K0 ('w'), bb_fallback_3
  %11 = LOAD_TVALUE %9, 0i
  STORE_TVALUE R3, %11
  JUMP bb_linear_43
bb_linear_43:
  %284 = GET_SLOT_NODE_ADDR %8, 2u, K1 ('h')
  CHECK_SLOT_MATCH %284, K1 ('h'), bb_fallback_5
  %286 = LOAD_TVALUE %284, 0i
  STORE_TVALUE R4, %286
  CHECK_SAFE_ENV bb_fallback_7
  CHECK_TAG R3, tnumber, exit(6)
  CHECK_TAG R4, tnumber, exit(6)
  %294 = LOAD_DOUBLE R3
  %295 = LOAD_DOUBLE R4
  %296 = NUM_TO_FLOAT %294
  %297 = NUM_TO_FLOAT %295
  STORE_VECTOR R2, %296, %297, 0
  STORE_TAG R2, tvector
  %305 = LOAD_TVALUE R1, 0i, tvector
  %306 = LOAD_TVALUE R2, 0i, tvector
  %307 = MUL_VEC %305, %306
  %310 = LOAD_TVALUE K5 (0.5, 0.5, 0), 0i, tvector
  %312 = SUB_VEC %307, %310
  %316 = FLOOR_VEC %312
  %317 = TAG_VECTOR %316
  %324 = CEIL_VEC %312
  %334 = SUB_VEC %312, %316
  %335 = TAG_VECTOR %334
  STORE_TVALUE R4, %335
  %337 = EXTRACT_VEC %317, 0i
  %338 = FLOAT_TO_NUM %337
  STORE_TVALUE R7, %11
  %351 = MOD_NUM %338, %294
  STORE_DOUBLE R5, %351
  STORE_TAG R5, tnumber
  %357 = EXTRACT_VEC %324, 0i
  %358 = FLOAT_TO_NUM %357
  STORE_DOUBLE R7, %358
  STORE_TVALUE R8, %11
  %371 = MOD_NUM %358, %294
  STORE_SPLIT_TVALUE R6, tnumber, %371
  %375 = EXTRACT_VEC %316, 1i
  %376 = FLOAT_TO_NUM %375
  %389 = MOD_NUM %376, %295
  STORE_DOUBLE R8, %389
  STORE_TVALUE R9, %11
  %405 = MUL_NUM %389, %294
  STORE_DOUBLE R7, %405
  %409 = EXTRACT_VEC %324, 1i
  %410 = FLOAT_TO_NUM %409
  STORE_DOUBLE R9, %410
  %423 = MOD_NUM %410, %295
  STORE_DOUBLE R8, %423
  %439 = MUL_NUM %423, %294
  STORE_SPLIT_TVALUE R8, tnumber, %439
  INTERRUPT 50u
  RETURN R4, 5i
)"
    );
}

// This test checks that in case of known constants, we propagate them in full and can recover the constant difference
TEST_CASE_FIXTURE(LoweringFixture, "TableNodeLoadStoreProp6")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function test(t: { x: number, y: number })
    t.x = 14
    t.y = 28

    t.x = t.x - t.y
end
)",
                   false,
                   1,
                   2,
                   true
               ),
        R"(
; function test($arg0) line 2
bb_0:
  CHECK_TAG R0, ttable, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  STORE_DOUBLE R1, 14
  STORE_TAG R1, tnumber
  %8 = LOAD_POINTER R0
  %9 = GET_SLOT_NODE_ADDR %8, 1u, K0 ('x')
  CHECK_SLOT_MATCH %9, K0 ('x'), bb_fallback_3
  CHECK_READONLY %8, bb_fallback_3
  STORE_SPLIT_TVALUE %9, tnumber, 14, 0i
  JUMP bb_linear_15
bb_linear_15:
  STORE_DOUBLE R1, 28
  %82 = GET_SLOT_NODE_ADDR %8, 4u, K1 ('y')
  CHECK_SLOT_MATCH %82, K1 ('y'), bb_fallback_5
  STORE_SPLIT_TVALUE %82, tnumber, 28, 0i
  STORE_SPLIT_TVALUE %9, tnumber, -14, 0i
  INTERRUPT 13u
  RETURN R0, 0i
)"
    );
}

// This test shows a table key swap
// Invalidating CHECK_SLOT_MATCH of one key with nil does not cause CHECK_NODE_VALUE of the other
TEST_CASE_FIXTURE(LoweringFixture, "TableNodeLoadStoreProp7")
{
    // TODO: opportunity - table barrier is not needed when values come from the same table
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function test(t: { x: number, y: number })
    t.x, t.y = t.y, t.x
end
)",
                   false,
                   1,
                   2,
                   true
               ),
        R"(
; function test($arg0) line 2
bb_0:
  CHECK_TAG R0, ttable, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %6 = LOAD_POINTER R0
  %7 = GET_SLOT_NODE_ADDR %6, 0u, K0 ('y')
  CHECK_SLOT_MATCH %7, K0 ('y'), bb_fallback_3
  %9 = LOAD_TVALUE %7, 0i
  STORE_TVALUE R1, %9
  JUMP bb_linear_11
bb_linear_11:
  %51 = GET_SLOT_NODE_ADDR %6, 2u, K1 ('x')
  CHECK_SLOT_MATCH %51, K1 ('x'), bb_fallback_5
  %53 = LOAD_TVALUE %51, 0i
  STORE_TVALUE R2, %53
  CHECK_READONLY %6, bb_fallback_7
  STORE_TVALUE %51, %9, 0i
  BARRIER_TABLE_FORWARD %6, R1, undef
  STORE_TVALUE %7, %53, 0i
  BARRIER_TABLE_FORWARD %6, R2, undef
  INTERRUPT 8u
  RETURN R0, 0i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "LoadEnvReuse")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(a: number, b: number)
    x = a
    y = b
    x = b
end
)",
                   false,
                   1,
                   2,
                   true
               ),
        R"(
; function foo($arg0, $arg1) line 2
bb_0:
  CHECK_TAG R0, tnumber, exit(entry)
  CHECK_TAG R1, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %6 = LOAD_ENV
  %7 = GET_SLOT_NODE_ADDR %6, 0u, K0 ('x')
  CHECK_SLOT_MATCH %7, K0 ('x'), bb_fallback_3
  CHECK_READONLY %6, bb_fallback_3
  %10 = LOAD_TVALUE R0, 0i, tnumber
  STORE_TVALUE %7, %10, 0i
  JUMP bb_linear_9
bb_linear_9:
  %39 = GET_SLOT_NODE_ADDR %6, 2u, K1 ('y')
  CHECK_SLOT_MATCH %39, K1 ('y'), bb_fallback_5
  %42 = LOAD_TVALUE R1, 0i, tnumber
  STORE_TVALUE %39, %42, 0i
  STORE_TVALUE %7, %42, 0i
  INTERRUPT 6u
  RETURN R0, 0i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "CheckReadonlyEliminationOnSsaValues")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(t: { y: { a: number, b: number, c: number } })
    t.y.a = t.y.b -- this kills 'readonly' state tracking through VM RegisterLink
    t.y.c = 3
end
)",
                   false,
                   1,
                   2,
                   true
               ),
        R"(
; function foo($arg0) line 2
bb_0:
  CHECK_TAG R0, ttable, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %6 = LOAD_POINTER R0
  %7 = GET_SLOT_NODE_ADDR %6, 0u, K0 ('y')
  CHECK_SLOT_MATCH %7, K0 ('y'), bb_fallback_3
  %9 = LOAD_TVALUE %7, 0i
  STORE_TVALUE R1, %9
  JUMP bb_linear_15
bb_linear_15:
  STORE_TVALUE R2, %9
  CHECK_TAG R2, ttable, bb_fallback_7
  %80 = LOAD_POINTER R2
  %81 = GET_SLOT_NODE_ADDR %80, 4u, K1 ('b')
  CHECK_SLOT_MATCH %81, K1 ('b'), bb_fallback_7
  %83 = LOAD_TVALUE %81, 0i
  STORE_TVALUE R2, %83
  %89 = GET_SLOT_NODE_ADDR %80, 6u, K2 ('a')
  CHECK_SLOT_MATCH %89, K2 ('a'), bb_fallback_9
  CHECK_READONLY %80, bb_fallback_9
  STORE_TVALUE %89, %83, 0i
  BARRIER_TABLE_FORWARD %80, R2, undef
  STORE_DOUBLE R2, 3
  STORE_TAG R2, tnumber
  %107 = GET_SLOT_NODE_ADDR %80, 11u, K3 ('c')
  CHECK_SLOT_MATCH %107, K3 ('c'), bb_fallback_13
  STORE_SPLIT_TVALUE %107, tnumber, 3, 0i
  INTERRUPT 13u
  RETURN R0, 0i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "CheckNoMetatableEliminationOnSsaValues")
{
    ensureVectorSize3();

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(t: { y: { z: number } })
    t.y[1] = t.y.z
    t.y[2] = 20
end
)",
                   false,
                   1,
                   2,
                   true
               ),
        R"(
; function foo($arg0) line 2
bb_0:
  CHECK_TAG R0, ttable, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %6 = LOAD_POINTER R0
  %7 = GET_SLOT_NODE_ADDR %6, 0u, K0 ('y')
  CHECK_SLOT_MATCH %7, K0 ('y'), bb_fallback_3
  %9 = LOAD_TVALUE %7, 0i
  STORE_TVALUE R1, %9
  JUMP bb_linear_15
bb_linear_15:
  STORE_TVALUE R2, %9
  CHECK_TAG R2, ttable, bb_fallback_7
  %84 = LOAD_POINTER R2
  %85 = GET_SLOT_NODE_ADDR %84, 4u, K1 ('z')
  CHECK_SLOT_MATCH %85, K1 ('z'), bb_fallback_7
  %87 = LOAD_TVALUE %85, 0i
  STORE_TVALUE R2, %87
  CHECK_ARRAY_SIZE %84, 0i, bb_fallback_9
  CHECK_NO_METATABLE %84, bb_fallback_9
  CHECK_READONLY %84, bb_fallback_9
  %96 = GET_ARR_ADDR %84, 0i
  STORE_TVALUE %96, %87, 0i
  BARRIER_TABLE_FORWARD %84, R2, undef
  STORE_DOUBLE R2, 20
  STORE_TAG R2, tnumber
  CHECK_ARRAY_SIZE %84, 1i, bb_fallback_13
  STORE_SPLIT_TVALUE %96, tnumber, 20, 16i
  INTERRUPT 11u
  RETURN R0, 0i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "CheckNoMetatableSsaElim")
{
    ensureVectorSize3();

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(t: { y: { z: number } })
    t.y[1] = t.y.z
    t.y[2] = 20
end
)",
                   false,
                   1,
                   2,
                   true
               ),
        R"(
; function foo($arg0) line 2
bb_0:
  CHECK_TAG R0, ttable, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %6 = LOAD_POINTER R0
  %7 = GET_SLOT_NODE_ADDR %6, 0u, K0 ('y')
  CHECK_SLOT_MATCH %7, K0 ('y'), bb_fallback_3
  %9 = LOAD_TVALUE %7, 0i
  STORE_TVALUE R1, %9
  JUMP bb_linear_15
bb_linear_15:
  STORE_TVALUE R2, %9
  CHECK_TAG R2, ttable, bb_fallback_7
  %84 = LOAD_POINTER R2
  %85 = GET_SLOT_NODE_ADDR %84, 4u, K1 ('z')
  CHECK_SLOT_MATCH %85, K1 ('z'), bb_fallback_7
  %87 = LOAD_TVALUE %85, 0i
  STORE_TVALUE R2, %87
  CHECK_ARRAY_SIZE %84, 0i, bb_fallback_9
  CHECK_NO_METATABLE %84, bb_fallback_9
  CHECK_READONLY %84, bb_fallback_9
  %96 = GET_ARR_ADDR %84, 0i
  STORE_TVALUE %96, %87, 0i
  BARRIER_TABLE_FORWARD %84, R2, undef
  STORE_DOUBLE R2, 20
  STORE_TAG R2, tnumber
  CHECK_ARRAY_SIZE %84, 1i, bb_fallback_13
  STORE_SPLIT_TVALUE %96, tnumber, 20, 16i
  INTERRUPT 11u
  RETURN R0, 0i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "TableStoreForwardUnknownTag")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(t: {}, v, w)
    t.x = v
    t.y = w
    return t.x
end
)",
                   false,
                   1,
                   2,
                   true
               ),
        R"(
; function foo($arg0, $arg1, $arg2) line 2
bb_0:
  CHECK_TAG R0, ttable, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %6 = LOAD_POINTER R0
  %7 = GET_SLOT_NODE_ADDR %6, 0u, K0 ('x')
  CHECK_SLOT_MATCH %7, K0 ('x'), bb_fallback_3
  CHECK_READONLY %6, bb_fallback_3
  %10 = LOAD_TVALUE R1
  STORE_TVALUE %7, %10, 0i
  BARRIER_TABLE_FORWARD %6, R1, undef
  JUMP bb_linear_9
bb_linear_9:
  %41 = GET_SLOT_NODE_ADDR %6, 2u, K1 ('y')
  CHECK_SLOT_MATCH %41, K1 ('y'), bb_fallback_5
  %44 = LOAD_TVALUE R2
  STORE_TVALUE %41, %44, 0i
  BARRIER_TABLE_FORWARD %6, R2, undef
  CHECK_NODE_VALUE %7, bb_fallback_7
  STORE_TVALUE R3, %10
  INTERRUPT 6u
  RETURN R3, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "TableArrayStoreForwardUnknownTag")
{
    ensureVectorSize3();

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(t: {}, v, w)
    t[1] = v
    t[2] = w
    return t[1]
end
)",
                   false,
                   1,
                   2,
                   true
               ),
        R"(
; function foo($arg0, $arg1, $arg2) line 2
bb_0:
  CHECK_TAG R0, ttable, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %6 = LOAD_POINTER R0
  CHECK_ARRAY_SIZE %6, 0i, bb_fallback_3
  CHECK_NO_METATABLE %6, bb_fallback_3
  CHECK_READONLY %6, bb_fallback_3
  %10 = GET_ARR_ADDR %6, 0i
  %11 = LOAD_TVALUE R1
  STORE_TVALUE %10, %11, 0i
  BARRIER_TABLE_FORWARD %6, R1, undef
  JUMP bb_linear_9
bb_linear_9:
  CHECK_ARRAY_SIZE %6, 1i, bb_fallback_5
  %51 = LOAD_TVALUE R2
  STORE_TVALUE %10, %51, 16i
  BARRIER_TABLE_FORWARD %6, R2, undef
  STORE_TVALUE R3, %11
  INTERRUPT 3u
  RETURN R3, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "FastcallTypeInferThroughLocal")
{
    ensureVectorFloat();
    ensureVectorSize3();

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function getsum(x, c)
    local v = vector(x, 2, 3)
    if c then
        return v.X + v.Y
    else
        return v.Z
    end
end
)",
                   /* includeIrTypes */ true
               ),
        R"(
; function getsum($arg0, $arg1) line 2
; R2: vector from 0 to 18
bb_bytecode_0:
  STORE_DOUBLE R4, 2
  STORE_TAG R4, tnumber
  STORE_DOUBLE R5, 3
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_2
  CHECK_TAG R0, tnumber, exit(4)
  %11 = LOAD_DOUBLE R0
  %14 = NUM_TO_FLOAT %11
  STORE_VECTOR R2, %14, 2, 3
  STORE_TAG R2, tvector
  JUMP bb_3
bb_3:
  JUMP_IF_FALSY R1, bb_bytecode_1, bb_6
bb_6:
  CHECK_TAG R2, tvector, exit(9)
  %35 = LOAD_FLOAT R2, 0i
  %36 = FLOAT_TO_NUM %35
  %41 = LOAD_FLOAT R2, 4i
  %42 = FLOAT_TO_NUM %41
  %51 = ADD_NUM %36, %42
  STORE_DOUBLE R3, %51
  STORE_TAG R3, tnumber
  INTERRUPT 14u
  RETURN R3, 1i
bb_bytecode_1:
  CHECK_TAG R2, tvector, exit(15)
  %58 = LOAD_FLOAT R2, 8i
  %59 = FLOAT_TO_NUM %58
  STORE_DOUBLE R3, %59
  STORE_TAG R3, tnumber
  INTERRUPT 17u
  RETURN R3, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "FastcallTypeInferThroughUpvalue")
{
    ensureVectorFloat();
    ensureVectorSize3();

    // TODO: opportunity - bb_3 and bb_bytecode_1 have only one predecessor, so they should know that the upvalue u0 is already in r2
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local v = ...

local function getsum(x, c)
    v = vector(x, 2, 3)
    if c then
        return v.X + v.Y
    else
        return v.Z
    end
end
)",
                   /* includeIrTypes */ true
               ),
        R"(
; function getsum($arg0, $arg1) line 4
; U0: vector
bb_bytecode_0:
  STORE_DOUBLE R4, 2
  STORE_TAG R4, tnumber
  STORE_DOUBLE R5, 3
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_2
  CHECK_TAG R0, tnumber, exit(4)
  %11 = LOAD_DOUBLE R0
  %14 = NUM_TO_FLOAT %11
  STORE_VECTOR R2, %14, 2, 3
  STORE_TAG R2, tvector
  JUMP bb_3
bb_3:
  %32 = LOAD_TVALUE R2
  SET_UPVALUE U0, %32, undef
  JUMP_IF_FALSY R1, bb_bytecode_1, bb_6
bb_6:
  %35 = GET_UPVALUE U0
  STORE_TVALUE R3, %35
  CHECK_TAG R3, tvector, exit(11)
  %39 = EXTRACT_VEC %35, 0i
  %40 = FLOAT_TO_NUM %39
  STORE_TVALUE R4, %35
  %47 = EXTRACT_VEC %35, 1i
  %48 = FLOAT_TO_NUM %47
  %57 = ADD_NUM %40, %48
  STORE_DOUBLE R2, %57
  STORE_TAG R2, tnumber
  INTERRUPT 17u
  RETURN R2, 1i
bb_bytecode_1:
  %62 = GET_UPVALUE U0
  STORE_TVALUE R2, %62
  CHECK_TAG R2, tvector, exit(19)
  %66 = EXTRACT_VEC %62, 2i
  %67 = FLOAT_TO_NUM %66
  STORE_DOUBLE R2, %67
  STORE_TAG R2, tnumber
  INTERRUPT 21u
  RETURN R2, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "LoadAndMoveTypePropagation")
{
    ScopedFastFlag luauBackedgeHeapCheck{FFlag::LuauBackedgeHeapCheck, true};

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function getsum(n)
    local seqsum = 0
    for i = 1,n do
        if i < 10 then
            seqsum += i
        else
            seqsum *= i
        end
    end

    return seqsum
end
)",
                   /* includeIrTypes */ true
               ),
        R"(
; function getsum($arg0) line 2
; R1: number from 0 to 13
; R4: number from 1 to 11
bb_bytecode_0:
  STORE_DOUBLE R1, 0
  STORE_TAG R1, tnumber
  STORE_DOUBLE R4, 1
  STORE_TAG R4, tnumber
  %4 = LOAD_TVALUE R0
  STORE_TVALUE R2, %4
  STORE_DOUBLE R3, 1
  STORE_TAG R3, tnumber
  CHECK_TAG R0, tnumber, exit(4)
  %12 = LOAD_DOUBLE R0
  JUMP_CMP_NUM 1, %12, not_le, bb_bytecode_4, bb_bytecode_1
bb_bytecode_1:
  INTERRUPT 5u
  STORE_DOUBLE R5, 10
  STORE_TAG R5, tnumber
  CHECK_TAG R4, tnumber, bb_fallback_6
  JUMP_CMP_NUM R4, 10, not_lt, bb_bytecode_2, bb_5
bb_5:
  CHECK_TAG R1, tnumber, exit(8)
  CHECK_TAG R4, tnumber, exit(8)
  %32 = LOAD_DOUBLE R1
  %34 = ADD_NUM %32, R4
  STORE_DOUBLE R1, %34
  JUMP bb_bytecode_3
bb_bytecode_2:
  CHECK_TAG R1, tnumber, exit(10)
  CHECK_TAG R4, tnumber, exit(10)
  %41 = LOAD_DOUBLE R1
  %43 = MUL_NUM %41, R4
  STORE_DOUBLE R1, %43
  JUMP bb_bytecode_3
bb_bytecode_3:
  CHECK_GC
  %47 = LOAD_DOUBLE R2
  %48 = LOAD_DOUBLE R4
  %49 = ADD_NUM %48, 1
  STORE_DOUBLE R4, %49
  JUMP_CMP_NUM %49, %47, le, bb_bytecode_1, bb_bytecode_4
bb_bytecode_4:
  INTERRUPT 12u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "ArgumentTypeRefinement")
{
    ensureVectorFloat();
    ensureVectorSize3();

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function getsum(x, y)
    x = vector(1, y, 3)
    return x.Y + x.Z
end
)",
                   /* includeIrTypes */ true
               ),
        R"(
; function getsum($arg0, $arg1) line 2
; R0: vector [argument]
bb_bytecode_0:
  STORE_DOUBLE R3, 1
  STORE_TAG R3, tnumber
  STORE_DOUBLE R5, 3
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_1
  CHECK_TAG R1, tnumber, exit(4)
  %12 = LOAD_DOUBLE R1
  %15 = NUM_TO_FLOAT %12
  STORE_VECTOR R2, 1, %15, 3
  STORE_TAG R2, tvector
  JUMP bb_2
bb_2:
  %32 = LOAD_TVALUE R2
  CHECK_TAG R2, tvector, bb_exit_5
   ; exit sync: R0, {%32}
  %36 = EXTRACT_VEC %32, 1i
  %37 = FLOAT_TO_NUM %36
  %42 = EXTRACT_VEC %32, 2i
  %43 = FLOAT_TO_NUM %42
  %52 = ADD_NUM %37, %43
  STORE_DOUBLE R2, %52
  STORE_TAG R2, tnumber
  INTERRUPT 14u
  RETURN R2, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "InlineFunctionType")
{
    ensureVectorFloat();

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function inl(v: vector, s: number)
    return v.Y * s
end

local function getsum(x)
    return inl(x, 3) + inl(x, 5)
end
)",
                   /* includeIrTypes */ true
               ),
        R"(
; function inl($arg0, $arg1) line 2
; R0: vector [argument]
; R1: number [argument]
bb_0:
  CHECK_TAG R0, tvector, exit(entry)
  CHECK_TAG R1, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %8 = LOAD_FLOAT R0, 4i
  %9 = FLOAT_TO_NUM %8
  %18 = MUL_NUM %9, R1
  STORE_DOUBLE R2, %18
  STORE_TAG R2, tnumber
  INTERRUPT 3u
  RETURN R2, 1i
; function getsum($arg0) line 6
; R0: vector from 0 to 3
; R0: vector from 3 to 6
bb_bytecode_0:
  CHECK_TAG R0, tvector, exit(0)
  %2 = LOAD_FLOAT R0, 4i
  %3 = FLOAT_TO_NUM %2
  %9 = MUL_NUM %3, 3
  %21 = MUL_NUM %3, 5
  %30 = ADD_NUM %9, %21
  STORE_DOUBLE R1, %30
  STORE_TAG R1, tnumber
  INTERRUPT 7u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "ResolveTablePathTypes")
{
    ensureVectorFloat();

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
type Vertex = {pos: vector, normal: vector}

local function foo(arr: {Vertex}, i)
    local v = arr[i]

    return v.pos.Y
end
)",
                   /* includeIrTypes */ true,
                   /* debugLevel */ 2
               ),
        R"(
; function foo(arr, i) line 4
; R0: table [argument 'arr']
; R2: table from 0 to 6 [local 'v']
; R3: vector from 3 to 5
bb_0:
  CHECK_TAG R0, ttable, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  CHECK_TAG R1, tnumber, bb_fallback_3
  %8 = LOAD_POINTER R0
  %9 = LOAD_DOUBLE R1
  %10 = TRY_NUM_TO_INDEX %9, bb_fallback_3
  %11 = SUB_INT %10, 1i
  CHECK_ARRAY_SIZE %8, %11, bb_fallback_3
  CHECK_NO_METATABLE %8, bb_fallback_3
  %14 = GET_ARR_ADDR %8, %11
  %15 = LOAD_TVALUE %14
  STORE_TVALUE R2, %15
  JUMP bb_4
bb_4:
  CHECK_TAG R2, ttable, exit(1)
  %23 = LOAD_POINTER R2
  %24 = GET_SLOT_NODE_ADDR %23, 1u, K0 ('pos')
  CHECK_SLOT_MATCH %24, K0 ('pos'), bb_fallback_5
  %26 = LOAD_TVALUE %24, 0i
  STORE_TVALUE R3, %26
  JUMP bb_6
bb_6:
  CHECK_TAG R3, tvector, exit(3)
  %33 = LOAD_FLOAT R3, 4i
  %34 = FLOAT_TO_NUM %33
  STORE_DOUBLE R3, %34
  STORE_TAG R3, tnumber
  INTERRUPT 5u
  RETURN R3, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "ResolvableSimpleMath")
{
    ScopedFastFlag luauCompileReuseLocalRegs{FFlag::LuauCompileReuseLocalRegs, true};

    CHECK_EQ(
        "\n" + getCodegenHeader(R"(
type Vertex = { p: vector, uv: vector, n: vector, t: vector, b: vector, h: number }
local mesh: { vertices: {Vertex}, indices: {number} } = ...

local function compute()
    for i = 1,#mesh.indices,3 do
        local a = mesh.vertices[mesh.indices[i]]
        local b = mesh.vertices[mesh.indices[i + 1]]
        local c = mesh.vertices[mesh.indices[i + 2]]

        local vba = b.p - a.p
        local vca = c.p - a.p

        local uvba = b.uv - a.uv
        local uvca = c.uv - a.uv

        local r = 1.0 / (uvba.X * uvca.Y - uvca.X * uvba.Y);

        local sdir = (uvca.Y * vba - uvba.Y * vca) * r

        a.t += sdir
    end
end
)"),
        R"(
; function compute() line 5
; U0: table ['mesh']
; R2: number from 0 to 78 [local 'i']
; R3: table from 7 to 78 [local 'a']
; R4: table from 15 to 48 [local 'b']
; R4: vector from 48 to 78 [local 'uvca']
; R5: table from 24 to 53 [local 'c']
; R5: number from 53 to 78 [local 'r']
; R6: vector from 33 to 78 [local 'vba']
; R7: vector from 37 to 38
; R7: vector from 38 to 78 [local 'vca']
; R8: vector from 37 to 38
; R8: vector from 42 to 43
; R8: vector from 43 to 78 [local 'uvba']
; R9: vector from 42 to 43
; R9: vector from 47 to 48
; R9: vector from 52 to 53
; R9: vector from 65 to 78 [local 'sdir']
; R10: vector from 47 to 48
; R10: vector from 52 to 53
; R10: vector from 72 to 73
; R10: vector from 75 to 76
; R11: vector from 71 to 72
; R12: vector from 71 to 72
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "ResolvableFunctionReturns")
{
    ScopedFastFlag luauCompileReuseLocalRegs{FFlag::LuauCompileReuseLocalRegs, true};

    CHECK_EQ(
        "\n" + getCodegenHeader(R"(
type Vertex = { p: vector, uv: vector, n: vector, t: vector, b: vector, h: number }
local mesh: { vertices: {Vertex}, indices: {number} } = ...

local function temp(b: vector, c: vector) : number
    return 1 / (b.X * c.Y - c.X * b.Y)
end

local function compute()
    for i = 1,#mesh.indices,3 do
        local a = mesh.vertices[mesh.indices[i]]
        local b = mesh.vertices[mesh.indices[i + 1]]
        local c = mesh.vertices[mesh.indices[i + 2]]

        local uvba = b.uv - a.uv
        local uvca = c.uv - a.uv

        local r = temp(uvba, uvca);

        a.t += a.p * r
    end
end
)"),
        R"(
; function compute() line 9
; U0: table ['mesh']
; R2: number from 0 to 63 [local 'i']
; R3: table from 7 to 63 [local 'a']
; R4: table from 15 to 38 [local 'b']
; R4: vector from 43 to 55 [local 'c']
; R4: vector from 38 to 63 [local 'uvca']
; R5: table from 24 to 43 [local 'c']
; R5: number from 43 to 63 [local 'r']
; R6: vector from 43 to 55 [local 'b']
; R6: vector from 33 to 63 [local 'uvba']
; R7: vector from 37 to 38
; R7: vector from 42 to 43
; R7: vector from 60 to 61
; R8: vector from 37 to 38
; R8: vector from 42 to 43
; R8: vector from 60 to 61
; R9: vector from 59 to 60
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "ResolveVectorNamecalls")
{
    ensureVectorFloat();

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
type Vertex = {pos: vector, normal: vector}

local function foo(arr: {Vertex}, i)
    return arr[i].normal:Dot(vector(0.707, 0, 0.707))
end
)",
                   /* includeIrTypes */ true
               ),
        R"(
; function foo($arg0, $arg1) line 4
; R0: table [argument]
; R2: vector from 4 to 6
bb_0:
  CHECK_TAG R0, ttable, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  CHECK_TAG R1, tnumber, bb_fallback_3
  %8 = LOAD_POINTER R0
  %9 = LOAD_DOUBLE R1
  %10 = TRY_NUM_TO_INDEX %9, bb_fallback_3
  %11 = SUB_INT %10, 1i
  CHECK_ARRAY_SIZE %8, %11, bb_fallback_3
  CHECK_NO_METATABLE %8, bb_fallback_3
  %14 = GET_ARR_ADDR %8, %11
  %15 = LOAD_TVALUE %14
  STORE_TVALUE R2, %15
  JUMP bb_4
bb_4:
  CHECK_TAG R2, ttable, bb_fallback_5
  %23 = LOAD_POINTER R2
  %24 = GET_SLOT_NODE_ADDR %23, 1u, K0 ('normal')
  CHECK_SLOT_MATCH %24, K0 ('normal'), bb_fallback_5
  %26 = LOAD_TVALUE %24, 0i
  STORE_TVALUE R2, %26
  JUMP bb_6
bb_6:
  %31 = LOAD_TVALUE K1 (0.707000017, 0, 0.707000017), 0i, tvector
  STORE_TVALUE R4, %31
  CHECK_TAG R2, tvector, exit(4)
  %37 = LOAD_FLOAT R2, 0i
  %39 = MUL_FLOAT %37, 0.7070000171661377
  %40 = LOAD_FLOAT R2, 4i
  %42 = MUL_FLOAT %40, 0
  %43 = LOAD_FLOAT R2, 8i
  %45 = MUL_FLOAT %43, 0.7070000171661377
  %46 = ADD_FLOAT %39, %42
  %47 = ADD_FLOAT %46, %45
  %48 = FLOAT_TO_NUM %47
  STORE_DOUBLE R2, %48
  STORE_TAG R2, tnumber
  ADJUST_STACK_TO_REG R2, 1i
  INTERRUPT 7u
  RETURN R2, -1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "ImmediateTypeAnnotationHelp")
{
    ensureVectorFloat();

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(arr, i)
    return (arr[i] :: vector) / 5
end
)",
                   /* includeIrTypes */ true
               ),
        R"(
; function foo($arg0, $arg1) line 2
; R3: vector from 1 to 2
bb_bytecode_0:
  CHECK_TAG R0, ttable, bb_fallback_1
  CHECK_TAG R1, tnumber, bb_fallback_1
  %4 = LOAD_POINTER R0
  %5 = LOAD_DOUBLE R1
  %6 = TRY_NUM_TO_INDEX %5, bb_fallback_1
  %7 = SUB_INT %6, 1i
  CHECK_ARRAY_SIZE %4, %7, bb_fallback_1
  CHECK_NO_METATABLE %4, bb_fallback_1
  %10 = GET_ARR_ADDR %4, %7
  %11 = LOAD_TVALUE %10
  STORE_TVALUE R3, %11
  JUMP bb_2
bb_2:
  CHECK_TAG R3, tvector, exit(1)
  %19 = LOAD_TVALUE R3, 0i, tvector
  %21 = FLOAT_TO_VEC 5
  %22 = DIV_VEC %19, %21
  %23 = TAG_VECTOR %22
  STORE_TVALUE R2, %23
  INTERRUPT 2u
  RETURN R2, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "UnaryTypeResolve")
{
    ensureVectorSize3();

    CHECK_EQ(
        "\n" + getCodegenHeader(R"(
local function foo(a, b: vector, c)
    local d = not a
    local e = -b
    local f = #c
    return (if d then e else vector(f, 2, 3)).X
end
)"),
        R"(
; function foo(a, b, c) line 2
; R1: vector [argument 'b']
; R3: boolean from 0 to 17 [local 'd']
; R4: vector from 1 to 17 [local 'e']
; R5: number from 2 to 17 [local 'f']
; R6: vector from 14 to 16
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "ForInManualAnnotation")
{
    ensureVectorFloat();

    ScopedFastFlag emitCallFb{FFlag::LuauEmitCallFeedback, true};
    ScopedFastFlag luauBackedgeHeapCheck{FFlag::LuauBackedgeHeapCheck, true};

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
type Vertex = {pos: vector, normal: vector}

local function foo(a: {Vertex})
    local sum = 0
    for k, v: Vertex in ipairs(a) do
        sum += v.pos.X
    end
    return sum
end
)",
                   /* includeIrTypes */ true,
                   /* debugLevel */ 2
               ),
        R"(
; function foo(a) line 4
; R0: table [argument 'a']
; R1: number from 0 to 15 [local 'sum']
; R5: number from 6 to 12 [local 'k']
; R6: table from 6 to 12 [local 'v']
; R7: vector from 9 to 11
bb_0:
  CHECK_TAG R0, ttable, exit(entry)
  JUMP bb_4
bb_4:
  JUMP bb_bytecode_1
bb_bytecode_1:
  STORE_DOUBLE R1, 0
  STORE_TAG R1, tnumber
  CHECK_SAFE_ENV bb_fallback_5
  GET_CACHED_IMPORT R2, K1 (nil), 1073741824u ('ipairs'), 2u
  JUMP bb_6
bb_6:
  %11 = LOAD_TVALUE R0, 0i, ttable
  STORE_TVALUE R3, %11
  INTERRUPT 4u
  SET_SAVEDPC 6u
  CALL R2, 1i, 3i
  CHECK_SAFE_ENV exit(6)
  CHECK_TAG R3, ttable, bb_fallback_7
  CHECK_TAG R4, tnumber, bb_fallback_7
  JUMP_CMP_NUM R4, 0, not_eq, bb_fallback_7, bb_8
bb_8:
  STORE_TAG R2, tnil
  STORE_POINTER R4, 0i
  STORE_EXTRA R4, 128i
  STORE_TAG R4, tlightuserdata
  JUMP bb_bytecode_3
bb_bytecode_2:
  CHECK_TAG R6, ttable, exit(7)
  %31 = LOAD_POINTER R6
  %32 = GET_SLOT_NODE_ADDR %31, 7u, K2 ('pos')
  CHECK_SLOT_MATCH %32, K2 ('pos'), bb_fallback_9
  %34 = LOAD_TVALUE %32, 0i
  STORE_TVALUE R7, %34
  JUMP bb_10
bb_10:
  CHECK_TAG R7, tvector, exit(9)
  %41 = LOAD_FLOAT R7, 0i
  %42 = FLOAT_TO_NUM %41
  STORE_DOUBLE R7, %42
  STORE_TAG R7, tnumber
  CHECK_TAG R1, tnumber, exit(11)
  %49 = LOAD_DOUBLE R1
  %51 = ADD_NUM %49, %42
  STORE_DOUBLE R1, %51
  JUMP bb_bytecode_3
bb_bytecode_3:
  INTERRUPT 12u
  CHECK_GC
  CHECK_TAG R2, tnil, bb_fallback_12
  %58 = LOAD_POINTER R3
  %59 = LOAD_INT R4
  %60 = GET_ARR_ADDR %58, %59
  CHECK_ARRAY_SIZE %58, %59, bb_11
  %62 = LOAD_TAG %60
  JUMP_EQ_TAG %62, tnil, bb_11, bb_13
bb_13:
  %64 = ADD_INT %59, 1i
  STORE_INT R4, %64
  %66 = INT_TO_NUM %64
  STORE_DOUBLE R5, %66
  STORE_TAG R5, tnumber
  %69 = LOAD_TVALUE %60
  STORE_TVALUE R6, %69
  JUMP bb_bytecode_2
bb_11:
  INTERRUPT 14u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "ForInAutoAnnotationIpairs")
{
    ScopedFastFlag emitCallFb{FFlag::LuauEmitCallFeedback, true};

    CHECK_EQ(
        "\n" + getCodegenHeader(R"(
type Vertex = {pos: vector, normal: vector}

local function foo(a: {Vertex})
    local sum = 0
    for k, v in ipairs(a) do
        local n = v.pos.X
        sum += n
    end
    return sum
end
)"),
        R"(
; function foo(a) line 4
; R0: table [argument 'a']
; R1: number from 0 to 15 [local 'sum']
; R5: number from 6 to 12 [local 'k']
; R6: table from 6 to 12 [local 'v']
; R7: vector from 9 to 11
; R7: number from 7 to 12 [local 'n']
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "ForInAutoAnnotationPairs")
{
    ScopedFastFlag emitCallFb{FFlag::LuauEmitCallFeedback, true};

    CHECK_EQ(
        "\n" + getCodegenHeader(R"(
type Vertex = {pos: vector, normal: vector}

local function foo(a: {[string]: Vertex})
    local sum = 0
    for k, v in pairs(a) do
        local n = v.pos.X
        sum += n
    end
    return sum
end
)"),
        R"(
; function foo(a) line 4
; R0: table [argument 'a']
; R1: number from 0 to 15 [local 'sum']
; R5: string from 6 to 12 [local 'k']
; R6: table from 6 to 12 [local 'v']
; R7: vector from 9 to 11
; R7: number from 7 to 12 [local 'n']
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "ForInAutoAnnotationGeneric")
{
    CHECK_EQ(
        "\n" + getCodegenHeader(R"(
type Vertex = {pos: vector, normal: vector}

local function foo(a: {Vertex})
    local sum = 0
    for k, v in a do
        local n = v.pos.X
        sum += n
    end
    return sum
end
)"),
        R"(
; function foo(a) line 4
; R0: table [argument 'a']
; R1: number from 0 to 13 [local 'sum']
; R5: number from 4 to 10 [local 'k']
; R6: table from 4 to 10 [local 'v']
; R7: vector from 7 to 9
; R7: number from 5 to 10 [local 'n']
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "CustomUserdataTypes")
{
    // This test requires runtime component to be present
    if (!Luau::CodeGen::isSupported())
        return;

    CHECK_EQ(
        "\n" + getCodegenHeader(R"(
local function foo(v: vec2, x: mat3)
    return v.X * x
end
)"),
        R"(
; function foo(v, x) line 2
; R0: vec2 [argument 'v']
; R1: mat3 [argument 'x']
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "CustomUserdataPropertyAccess")
{
    // This test requires runtime component to be present
    if (!Luau::CodeGen::isSupported())
        return;

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(v: vec2)
    return v.X + v.Y
end
)",
                   /* includeIrTypes */ true
               ),
        R"(
; function foo($arg0) line 2
; R0: vec2 [argument]
bb_0:
  CHECK_TAG R0, tuserdata, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %6 = LOAD_POINTER R0
  CHECK_USERDATA_TAG %6, 12i, exit(0)
  %8 = BUFFER_READF32 %6, 0i, tuserdata
  %9 = FLOAT_TO_NUM %8
  %16 = BUFFER_READF32 %6, 4i, tuserdata
  %17 = FLOAT_TO_NUM %16
  %26 = ADD_NUM %9, %17
  STORE_DOUBLE R1, %26
  STORE_TAG R1, tnumber
  INTERRUPT 5u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "CustomUserdataPropertyAccess2")
{
    ensureVectorFloat();

    // This test requires runtime component to be present
    if (!Luau::CodeGen::isSupported())
        return;

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(a: mat3)
    return a.Row1 * a.Row2
end
)",
                   /* includeIrTypes */ true
               ),
        R"(
; function foo($arg0) line 2
; R0: mat3 [argument]
bb_0:
  CHECK_TAG R0, tuserdata, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  FALLBACK_GETTABLEKS 0u, R2, R0, K0 ('Row1')
  FALLBACK_GETTABLEKS 2u, R3, R0, K1 ('Row2')
  CHECK_TAG R2, tvector, exit(4)
  CHECK_TAG R3, tvector, exit(4)
  %14 = LOAD_TVALUE R2, 0i, tvector
  %15 = LOAD_TVALUE R3, 0i, tvector
  %16 = MUL_VEC %14, %15
  %17 = TAG_VECTOR %16
  STORE_TVALUE R1, %17
  INTERRUPT 5u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "CustomUserdataNamecall1")
{
    // This test requires runtime component to be present
    if (!Luau::CodeGen::isSupported())
        return;

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(a: vec2, b: vec2)
    return a:Dot(b)
end
)",
                   /* includeIrTypes */ true
               ),
        R"(
; function foo($arg0, $arg1) line 2
; R0: vec2 [argument]
; R1: vec2 [argument]
bb_0:
  CHECK_TAG R0, tuserdata, exit(entry)
  CHECK_TAG R1, tuserdata, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %6 = LOAD_TVALUE R1, 0i, tuserdata
  STORE_TVALUE R4, %6
  %10 = LOAD_POINTER R0
  CHECK_USERDATA_TAG %10, 12i, exit(1)
  %14 = LOAD_POINTER R1
  CHECK_USERDATA_TAG %14, 12i, exit(1)
  %16 = BUFFER_READF32 %10, 0i, tuserdata
  %17 = BUFFER_READF32 %14, 0i, tuserdata
  %18 = FLOAT_TO_NUM %16
  %19 = FLOAT_TO_NUM %17
  %20 = MUL_NUM %18, %19
  %21 = BUFFER_READF32 %10, 4i, tuserdata
  %22 = BUFFER_READF32 %14, 4i, tuserdata
  %23 = FLOAT_TO_NUM %21
  %24 = FLOAT_TO_NUM %22
  %25 = MUL_NUM %23, %24
  %26 = ADD_NUM %20, %25
  STORE_DOUBLE R2, %26
  STORE_TAG R2, tnumber
  ADJUST_STACK_TO_REG R2, 1i
  INTERRUPT 4u
  RETURN R2, -1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "CustomUserdataNamecall2")
{
    // This test requires runtime component to be present
    if (!Luau::CodeGen::isSupported())
        return;

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(a: vec2, b: vec2)
    return a:Min(b)
end
)",
                   /* includeIrTypes */ true
               ),
        R"(
; function foo($arg0, $arg1) line 2
; R0: vec2 [argument]
; R1: vec2 [argument]
bb_0:
  CHECK_TAG R0, tuserdata, exit(entry)
  CHECK_TAG R1, tuserdata, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %6 = LOAD_TVALUE R1, 0i, tuserdata
  STORE_TVALUE R4, %6
  %10 = LOAD_POINTER R0
  CHECK_USERDATA_TAG %10, 12i, exit(1)
  %14 = LOAD_POINTER R1
  CHECK_USERDATA_TAG %14, 12i, exit(1)
  %16 = BUFFER_READF32 %10, 0i, tuserdata
  %17 = BUFFER_READF32 %14, 0i, tuserdata
  %18 = FLOAT_TO_NUM %16
  %19 = FLOAT_TO_NUM %17
  %20 = MIN_NUM %18, %19
  %21 = BUFFER_READF32 %10, 4i, tuserdata
  %22 = BUFFER_READF32 %14, 4i, tuserdata
  %23 = FLOAT_TO_NUM %21
  %24 = FLOAT_TO_NUM %22
  %25 = MIN_NUM %23, %24
  %26 = NUM_TO_FLOAT %20
  %27 = NUM_TO_FLOAT %25
  %28 = NEW_USERDATA 8i, 12i
  BUFFER_WRITEF32 %28, 0i, %26, tuserdata
  BUFFER_WRITEF32 %28, 4i, %27, tuserdata
  STORE_POINTER R2, %28
  STORE_TAG R2, tuserdata
  ADJUST_STACK_TO_REG R2, 1i
  INTERRUPT 4u
  RETURN R2, -1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "CustomUserdataMetamethodDirectFlow")
{
    // This test requires runtime component to be present
    if (!Luau::CodeGen::isSupported())
        return;

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(a: mat3, b: mat3)
    return a * b
end
)",
                   /* includeIrTypes */ true
               ),
        R"(
; function foo($arg0, $arg1) line 2
; R0: mat3 [argument]
; R1: mat3 [argument]
bb_0:
  CHECK_TAG R0, tuserdata, exit(entry)
  CHECK_TAG R1, tuserdata, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  SET_SAVEDPC 1u
  DO_ARITH R2, R0, R1, 10i
  INTERRUPT 1u
  RETURN R2, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "CustomUserdataMetamethodDirectFlow2")
{
    // This test requires runtime component to be present
    if (!Luau::CodeGen::isSupported())
        return;

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(a: mat3)
    return -a
end
)",
                   /* includeIrTypes */ true
               ),
        R"(
; function foo($arg0) line 2
; R0: mat3 [argument]
bb_0:
  CHECK_TAG R0, tuserdata, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  SET_SAVEDPC 1u
  DO_ARITH R1, R0, R0, 15i
  INTERRUPT 1u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "CustomUserdataMetamethodDirectFlow3")
{
    // This test requires runtime component to be present
    if (!Luau::CodeGen::isSupported())
        return;

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(a: sequence)
    return #a
end
)",
                   /* includeIrTypes */ true
               ),
        R"(
; function foo($arg0) line 2
; R0: userdata [argument]
bb_0:
  CHECK_TAG R0, tuserdata, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  SET_SAVEDPC 1u
  DO_LEN R1, R0
  INTERRUPT 1u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "CustomUserdataMetamethod")
{
    // This test requires runtime component to be present
    if (!Luau::CodeGen::isSupported())
        return;

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(a: vec2, b: vec2, c: vec2)
    return -c + a * b
end
)",
                   /* includeIrTypes */ true
               ),
        R"(
; function foo($arg0, $arg1, $arg2) line 2
; R0: vec2 [argument]
; R1: vec2 [argument]
; R2: vec2 [argument]
bb_0:
  CHECK_TAG R0, tuserdata, exit(entry)
  CHECK_TAG R1, tuserdata, exit(entry)
  CHECK_TAG R2, tuserdata, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %10 = LOAD_POINTER R2
  CHECK_USERDATA_TAG %10, 12i, exit(0)
  %12 = BUFFER_READF32 %10, 0i, tuserdata
  %13 = BUFFER_READF32 %10, 4i, tuserdata
  %14 = UNM_FLOAT %12
  %15 = UNM_FLOAT %13
  %16 = NEW_USERDATA 8i, 12i
  BUFFER_WRITEF32 %16, 0i, %14, tuserdata
  BUFFER_WRITEF32 %16, 4i, %15, tuserdata
  %25 = LOAD_POINTER R0
  CHECK_USERDATA_TAG %25, 12i, bb_exit_3
   ; exit sync: R4, {%16}
  %27 = LOAD_POINTER R1
  CHECK_USERDATA_TAG %27, 12i, bb_exit_4
   ; exit sync: R4, {%16}
  %29 = BUFFER_READF32 %25, 0i, tuserdata
  %30 = BUFFER_READF32 %27, 0i, tuserdata
  %31 = MUL_FLOAT %29, %30
  %32 = BUFFER_READF32 %25, 4i, tuserdata
  %33 = BUFFER_READF32 %27, 4i, tuserdata
  %34 = MUL_FLOAT %32, %33
  %50 = ADD_FLOAT %14, %31
  %53 = ADD_FLOAT %15, %34
  %54 = NEW_USERDATA 8i, 12i
  BUFFER_WRITEF32 %54, 0i, %50, tuserdata
  BUFFER_WRITEF32 %54, 4i, %53, tuserdata
  STORE_POINTER R3, %54
  STORE_TAG R3, tuserdata
  INTERRUPT 3u
  RETURN R3, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "CustomUserdataMapping")
{
    ScopedFastFlag emitCallFb{FFlag::LuauEmitCallFeedback, true};

    // This test requires runtime component to be present
    if (!Luau::CodeGen::isSupported())
        return;

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(a: mat3)
    print(a, vec2.create(0, 0))
end
)",
                   /* includeIrTypes */ true
               ),
        R"(
; function foo($arg0) line 2
; R0: mat3 [argument]
bb_0:
  CHECK_TAG R0, tuserdata, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  CHECK_SAFE_ENV bb_fallback_3
  GET_CACHED_IMPORT R1, K1 (nil), 1073741824u ('print'), 1u
  JUMP bb_4
bb_4:
  %9 = LOAD_TVALUE R0, 0i, tuserdata
  STORE_TVALUE R2, %9
  CHECK_SAFE_ENV bb_fallback_5
  GET_CACHED_IMPORT R3, K4 (nil), 2149583872u ('vec2'.'create'), 4u
  JUMP bb_6
bb_6:
  STORE_DOUBLE R4, 0
  STORE_TAG R4, tnumber
  STORE_DOUBLE R5, 0
  STORE_TAG R5, tnumber
  INTERRUPT 7u
  SET_SAVEDPC 8u
  CALL R3, 2i, -1i
  INTERRUPT 8u
  SET_SAVEDPC 9u
  CALL R1, -1i, 0i
  INTERRUPT 9u
  RETURN R0, 0i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "LibraryFieldTypesAndConstants")
{
    ensureVectorFloat();

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(a: vector)
    return Vector3.xAxis * a + Vector3.yAxis
end
)",
                   /* includeIrTypes */ true
               ),
        R"(
; function foo($arg0) line 2
; R0: vector [argument]
; R2: vector from 3 to 4
; R3: vector from 1 to 2
; R3: vector from 3 to 4
bb_0:
  CHECK_TAG R0, tvector, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %4 = LOAD_TVALUE K0 (1, 0, 0), 0i, tvector
  %11 = LOAD_TVALUE R0, 0i, tvector
  %12 = MUL_VEC %4, %11
  %15 = LOAD_TVALUE K1 (0, 1, 0), 0i, tvector
  %23 = ADD_VEC %12, %15
  %24 = TAG_VECTOR %23
  STORE_TVALUE R1, %24
  INTERRUPT 4u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "LibraryFieldTypesAndConstants")
{
    ensureVectorFloat();

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(a: vector)
    local x = vector.zero
    x += a
    return x
end
)",
                   /* includeIrTypes */ true
               ),
        R"(
; function foo($arg0) line 2
; R0: vector [argument]
; R1: vector from 0 to 3
bb_0:
  CHECK_TAG R0, tvector, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %4 = LOAD_TVALUE K0 (0, 0, 0), 0i, tvector
  %11 = LOAD_TVALUE R0, 0i, tvector
  %12 = ADD_VEC %4, %11
  %13 = TAG_VECTOR %12
  STORE_TVALUE R1, %13
  INTERRUPT 2u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "LibraryFieldTypesAndConstantsCApi")
{
    ensureVectorSize3();

    CHECK_EQ(
        "\n" + getCodegenAssemblyUsingCApi(
                   R"(
local function foo()
    return test.some_nil, test.some_boolean, test.some_number, test.some_vector, test.some_string
end
)",
                   /* includeIrTypes */ true
               ),
        R"(
; function foo() line 2
bb_bytecode_0:
  STORE_TAG R0, tnil
  STORE_INT R1, 1i
  STORE_TAG R1, tboolean
  STORE_DOUBLE R2, 4.75
  STORE_TAG R2, tnumber
  %5 = LOAD_TVALUE K1 (1, 2, 4), 0i, tvector
  STORE_TVALUE R3, %5
  %7 = LOAD_TVALUE K2 ('test'), 0i, tstring
  STORE_TVALUE R4, %7
  INTERRUPT 5u
  RETURN R0, 5i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "MathIsNan")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(a: number)
    return math.isnan(a)
end
)"),
        R"(
; function foo($arg0) line 2
bb_0:
  CHECK_TAG R0, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  CHECK_SAFE_ENV bb_fallback_3
  %7 = LOAD_DOUBLE R0
  %8 = CMP_SPLIT_TVALUE tnumber, tnumber, %7, %7, not_eq
  STORE_INT R1, %8
  STORE_TAG R1, tboolean
  JUMP bb_4
bb_4:
  INTERRUPT 5u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "Bit32BtestDirect")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(a: number)
    return bit32.btest(a, 0x1f)
end
)"),
        R"(
; function foo($arg0) line 2
bb_0:
  CHECK_TAG R0, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  CHECK_SAFE_ENV bb_fallback_3
  %7 = LOAD_DOUBLE R0
  %8 = NUM_TO_UINT %7
  %10 = BITAND_UINT %8, 31i
  %11 = CMP_INT %10, 0i, not_eq
  STORE_INT R1, %11
  STORE_TAG R1, tboolean
  JUMP bb_4
bb_4:
  INTERRUPT 7u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "Bit32ReplaceDirect")
{
    ensureVectorFloat();
    ensureVectorSize3();

    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(a: number, b: number)
    local x = bit32.band(a, 0x003FFFFF)
    local y = bit32.band(b, 0x003FFFFF)
    local z = bit32.replace(bit32.rshift(a, 22), bit32.rshift(b, 22), 10, 10)

    local v = vector.create(x, y, z)
    return v, v.x + v.y -- tests UINT_TO_FLOAT propagation as well
end
)"),
        R"(
; function foo($arg0, $arg1) line 2
bb_0:
  CHECK_TAG R0, tnumber, exit(entry)
  CHECK_TAG R1, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  CHECK_SAFE_ENV bb_fallback_3
  %9 = LOAD_DOUBLE R0
  %10 = NUM_TO_UINT %9
  %12 = BITAND_UINT %10, 4194303i
  JUMP bb_linear_27
bb_linear_27:
  %210 = LOAD_DOUBLE R1
  %211 = NUM_TO_UINT %210
  %212 = BITAND_UINT %211, 4194303i
  %220 = BITRSHIFT_UINT %10, 22i
  %228 = BITRSHIFT_UINT %211, 22i
  %246 = BITAND_UINT %220, -1047553i
  %247 = BITAND_UINT %228, 1023i
  %248 = BITLSHIFT_UINT %247, 10i
  %249 = BITOR_UINT %246, %248
  %264 = UINT_TO_FLOAT %12
  %265 = UINT_TO_FLOAT %212
  %266 = UINT_TO_FLOAT %249
  STORE_VECTOR R5, %264, %265, %266, tvector
  %270 = LOAD_TVALUE R5, 0i, tvector
  STORE_TVALUE R6, %270
  %274 = EXTRACT_VEC %270, 0i
  %275 = FLOAT_TO_NUM %274
  %278 = EXTRACT_VEC %270, 1i
  %279 = FLOAT_TO_NUM %278
  %282 = ADD_NUM %275, %279
  STORE_SPLIT_TVALUE R7, tnumber, %282
  INTERRUPT 48u
  RETURN R6, 2i
bb_4:
  CHECK_SAFE_ENV bb_fallback_7
  %34 = LOAD_DOUBLE R1
  %35 = NUM_TO_UINT %34
  %37 = BITAND_UINT %35, 4194303i
  %38 = UINT_TO_NUM %37
  STORE_DOUBLE R3, %38
  STORE_TAG R3, tnumber
  JUMP bb_8
bb_8:
  CHECK_SAFE_ENV bb_fallback_11
  %59 = LOAD_DOUBLE R0
  %60 = NUM_TO_UINT %59
  %61 = BITRSHIFT_UINT %60, 22i
  %62 = UINT_TO_NUM %61
  STORE_DOUBLE R5, %62
  STORE_TAG R5, tnumber
  JUMP bb_12
bb_12:
  CHECK_SAFE_ENV bb_fallback_15
  %83 = LOAD_DOUBLE R1
  %84 = NUM_TO_UINT %83
  %85 = BITRSHIFT_UINT %84, 22i
  %86 = UINT_TO_NUM %85
  STORE_DOUBLE R6, %86
  STORE_TAG R6, tnumber
  JUMP bb_16
bb_16:
  STORE_DOUBLE R7, 10
  STORE_TAG R7, tnumber
  STORE_DOUBLE R8, 10
  STORE_TAG R8, tnumber
  CHECK_SAFE_ENV bb_fallback_19
  CHECK_TAG R5, tnumber, exit(31)
  CHECK_TAG R6, tnumber, exit(31)
  %115 = LOAD_DOUBLE R5
  %116 = LOAD_DOUBLE R6
  %118 = NUM_TO_UINT %115
  %119 = NUM_TO_UINT %116
  %134 = BITAND_UINT %118, -1047553i
  %135 = BITAND_UINT %119, 1023i
  %136 = BITLSHIFT_UINT %135, 10i
  %137 = BITOR_UINT %134, %136
  %138 = UINT_TO_NUM %137
  STORE_DOUBLE R4, %138
  STORE_TAG R4, tnumber
  JUMP bb_20
bb_20:
  CHECK_SAFE_ENV bb_fallback_23
  CHECK_TAG R2, tnumber, exit(36)
  CHECK_TAG R3, tnumber, exit(36)
  CHECK_TAG R4, tnumber, exit(36)
  %159 = LOAD_DOUBLE R2
  %160 = LOAD_DOUBLE R3
  %161 = LOAD_DOUBLE R4
  %162 = NUM_TO_FLOAT %159
  %163 = NUM_TO_FLOAT %160
  %164 = NUM_TO_FLOAT %161
  STORE_VECTOR R5, %162, %163, %164
  STORE_TAG R5, tvector
  JUMP bb_24
bb_24:
  %184 = LOAD_TVALUE R5
  STORE_TVALUE R6, %184
  CHECK_TAG R5, tvector, exit(43)
  %188 = EXTRACT_VEC %184, 0i
  %189 = FLOAT_TO_NUM %188
  %194 = EXTRACT_VEC %184, 1i
  %195 = FLOAT_TO_NUM %194
  %204 = ADD_NUM %189, %195
  STORE_DOUBLE R7, %204
  STORE_TAG R7, tnumber
  INTERRUPT 48u
  RETURN R6, 2i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "Bit32ExtractDirect")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(a: number, b: number)
    return bit32.extract(a, b, 4)
end
)"),
        R"(
; function foo($arg0, $arg1) line 2
bb_0:
  CHECK_TAG R0, tnumber, exit(entry)
  CHECK_TAG R1, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  STORE_DOUBLE R5, 4
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_3
  %13 = LOAD_DOUBLE R0
  %14 = LOAD_DOUBLE R1
  %15 = NUM_TO_UINT %13
  %16 = NUM_TO_INT %14
  %21 = ADD_INT %16, 4i
  CHECK_CMP_INT %16, 0i, ge, exit(3)
  CHECK_CMP_INT %21, 32i, le, exit(3)
  %28 = BITRSHIFT_UINT %15, %16
  %29 = BITAND_UINT %28, 15i
  %30 = UINT_TO_NUM %29
  STORE_DOUBLE R2, %30
  STORE_TAG R2, tnumber
  JUMP bb_4
bb_4:
  INTERRUPT 8u
  RETURN R2, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "Bit32SingleArg")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(a: number, b: number, c: number)
    return bit32.band(a) + bit32.bor(b) + bit32.bxor(c)
end
)"),
        R"(
; function foo($arg0, $arg1, $arg2) line 2
bb_0:
  CHECK_TAG R0, tnumber, exit(entry)
  CHECK_TAG R1, tnumber, exit(entry)
  CHECK_TAG R2, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  CHECK_SAFE_ENV bb_fallback_3
  %11 = LOAD_DOUBLE R0
  %12 = NUM_TO_UINT %11
  %13 = UINT_TO_NUM %12
  JUMP bb_linear_15
bb_linear_15:
  %92 = LOAD_DOUBLE R1
  %93 = NUM_TO_UINT %92
  %94 = UINT_TO_NUM %93
  %104 = ADD_NUM %13, %94
  %108 = LOAD_DOUBLE R2
  %109 = NUM_TO_UINT %108
  %110 = UINT_TO_NUM %109
  %119 = ADD_NUM %104, %110
  STORE_DOUBLE R3, %119
  STORE_TAG R3, tnumber
  INTERRUPT 17u
  RETURN R3, 1i
bb_4:
  CHECK_SAFE_ENV bb_fallback_7
  %32 = LOAD_DOUBLE R1
  %33 = NUM_TO_UINT %32
  %34 = UINT_TO_NUM %33
  STORE_DOUBLE R6, %34
  STORE_TAG R6, tnumber
  JUMP bb_8
bb_8:
  CHECK_TAG R5, tnumber, exit(10)
  CHECK_TAG R6, tnumber, exit(10)
  %54 = LOAD_DOUBLE R5
  %56 = ADD_NUM %54, R6
  STORE_DOUBLE R4, %56
  STORE_TAG R4, tnumber
  CHECK_SAFE_ENV bb_fallback_11
  %62 = LOAD_DOUBLE R2
  %63 = NUM_TO_UINT %62
  %64 = UINT_TO_NUM %63
  STORE_DOUBLE R5, %64
  JUMP bb_12
bb_12:
  CHECK_TAG R4, tnumber, exit(16)
  CHECK_TAG R5, tnumber, exit(16)
  %84 = LOAD_DOUBLE R4
  %86 = ADD_NUM %84, R5
  STORE_DOUBLE R3, %86
  STORE_TAG R3, tnumber
  INTERRUPT 17u
  RETURN R3, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "Bit32SingleArgBtest")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(a: number)
    return bit32.btest(a)
end
)"),
        R"(
; function foo($arg0) line 2
bb_0:
  CHECK_TAG R0, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  CHECK_SAFE_ENV bb_fallback_3
  %7 = LOAD_DOUBLE R0
  %8 = NUM_TO_UINT %7
  %9 = CMP_INT %8, 0i, not_eq
  STORE_INT R1, %9
  STORE_TAG R1, tboolean
  JUMP bb_4
bb_4:
  INTERRUPT 5u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "VectorLoadReuse")
{
    ensureVectorFloat();

    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function shuffle(v: vector)
    return v.x * v.x + v.y * v.y
end
)"),
        R"(
; function shuffle($arg0) line 2
bb_0:
  CHECK_TAG R0, tvector, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %6 = LOAD_FLOAT R0, 0i
  %7 = FLOAT_TO_NUM %6
  %22 = MUL_NUM %7, %7
  %27 = LOAD_FLOAT R0, 4i
  %28 = FLOAT_TO_NUM %27
  %43 = MUL_NUM %28, %28
  %52 = ADD_NUM %22, %43
  STORE_DOUBLE R1, %52
  STORE_TAG R1, tnumber
  INTERRUPT 11u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "VectorShuffle1")
{
    ensureVectorFloat();
    ensureVectorSize3();

    // TODO: opportunity - if we introduce a separate vector shuffle instruction, this can be done in a single shuffle (+/- load and store)
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function shuffle(v: vector)
    return vector.create(v.z, v.x, v.y)
end
)"),
        R"(
; function shuffle($arg0) line 2
bb_0:
  CHECK_TAG R0, tvector, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %6 = LOAD_FLOAT R0, 8i
  %7 = FLOAT_TO_NUM %6
  STORE_DOUBLE R2, %7
  STORE_TAG R2, tnumber
  %12 = LOAD_FLOAT R0, 0i
  %13 = FLOAT_TO_NUM %12
  STORE_DOUBLE R3, %13
  STORE_TAG R3, tnumber
  %18 = LOAD_FLOAT R0, 4i
  %19 = FLOAT_TO_NUM %18
  STORE_DOUBLE R4, %19
  STORE_TAG R4, tnumber
  CHECK_SAFE_ENV bb_fallback_3
  STORE_VECTOR R1, %6, %12, %18
  STORE_TAG R1, tvector
  JUMP bb_4
bb_4:
  INTERRUPT 10u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "VectorShuffle2")
{
    ensureVectorFloat();
    ensureVectorSize3();

    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function crossshuffle(v: vector, t: vector)
    local tmp1 = vector.create(v.x, v.x, v.z)
    local tmp2 = vector.create(t.y, t.z, t.x)
    return vector.create(tmp1.z, tmp2.x, tmp1.y)
end
)"),
        R"(
; function crossshuffle($arg0, $arg1) line 2
bb_0:
  CHECK_TAG R0, tvector, exit(entry)
  CHECK_TAG R1, tvector, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %8 = LOAD_FLOAT R0, 0i
  %9 = FLOAT_TO_NUM %8
  STORE_DOUBLE R3, %9
  STORE_TAG R3, tnumber
  STORE_DOUBLE R4, %9
  STORE_TAG R4, tnumber
  %20 = LOAD_FLOAT R0, 8i
  %21 = FLOAT_TO_NUM %20
  STORE_DOUBLE R5, %21
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_3
  JUMP bb_linear_15
bb_linear_15:
  %140 = LOAD_FLOAT R1, 4i
  STORE_VECTOR R4, %20, %140, %8
  STORE_TAG R4, tvector
  INTERRUPT 30u
  RETURN R4, 1i
bb_4:
  %52 = LOAD_FLOAT R1, 4i
  %53 = FLOAT_TO_NUM %52
  STORE_DOUBLE R4, %53
  STORE_TAG R4, tnumber
  %58 = LOAD_FLOAT R1, 8i
  %59 = FLOAT_TO_NUM %58
  STORE_DOUBLE R5, %59
  STORE_TAG R5, tnumber
  %64 = LOAD_FLOAT R1, 0i
  %65 = FLOAT_TO_NUM %64
  STORE_DOUBLE R6, %65
  STORE_TAG R6, tnumber
  CHECK_SAFE_ENV bb_fallback_7
  STORE_VECTOR R3, %52, %58, %64
  STORE_TAG R3, tvector
  JUMP bb_8
bb_8:
  CHECK_TAG R2, tvector, exit(20)
  %96 = LOAD_FLOAT R2, 8i
  %97 = FLOAT_TO_NUM %96
  STORE_DOUBLE R5, %97
  STORE_TAG R5, tnumber
  CHECK_TAG R3, tvector, exit(22)
  %102 = LOAD_FLOAT R3, 0i
  %103 = FLOAT_TO_NUM %102
  STORE_DOUBLE R6, %103
  STORE_TAG R6, tnumber
  %108 = LOAD_FLOAT R2, 4i
  %109 = FLOAT_TO_NUM %108
  STORE_DOUBLE R7, %109
  STORE_TAG R7, tnumber
  CHECK_SAFE_ENV bb_fallback_11
  STORE_VECTOR R4, %96, %102, %108
  STORE_TAG R4, tvector
  JUMP bb_12
bb_12:
  INTERRUPT 30u
  RETURN R4, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "VectorShuffleFromComposite1")
{
    ensureVectorFloat();

    // This test requires runtime component to be present
    if (!Luau::CodeGen::isSupported())
        return;

    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function test(v: vertex)
    return v.normal.X * v.normal.X + v.normal.Y * v.normal.Y
end
)"),
        R"(
; function test($arg0) line 2
bb_0:
  CHECK_TAG R0, tuserdata, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %6 = LOAD_POINTER R0
  CHECK_USERDATA_TAG %6, 13i, exit(0)
  %8 = BUFFER_READF32 %6, 12i, tuserdata
  %9 = BUFFER_READF32 %6, 16i, tuserdata
  %16 = FLOAT_TO_NUM %8
  %40 = MUL_NUM %16, %16
  %55 = FLOAT_TO_NUM %9
  %79 = MUL_NUM %55, %55
  %88 = ADD_NUM %40, %79
  STORE_DOUBLE R1, %88
  STORE_TAG R1, tnumber
  INTERRUPT 19u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "VectorShuffleFromComposite2")
{
    // This test requires runtime component to be present
    if (!Luau::CodeGen::isSupported())
        return;

    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function test(v: vertex)
    return v.uv.X * v.uv.Y
end
)"),
        R"(
; function test($arg0) line 2
bb_0:
  CHECK_TAG R0, tuserdata, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %6 = LOAD_POINTER R0
  CHECK_USERDATA_TAG %6, 13i, exit(0)
  %8 = BUFFER_READF32 %6, 24i, tuserdata
  %9 = BUFFER_READF32 %6, 28i, tuserdata
  %20 = FLOAT_TO_NUM %8
  %39 = FLOAT_TO_NUM %9
  %48 = MUL_NUM %20, %39
  STORE_DOUBLE R1, %48
  STORE_TAG R1, tnumber
  INTERRUPT 9u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "VectorCreateXY")
{
    ensureVectorFloat();
    ensureVectorSize3();

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(a: number): vector
    return vector.create(a, a * 2.0)
end
)"
               ),
        R"(
; function foo($arg0) line 2
bb_0:
  CHECK_TAG R0, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %6 = LOAD_DOUBLE R0
  %7 = ADD_NUM %6, %6
  STORE_DOUBLE R3, %7
  STORE_TAG R3, tnumber
  CHECK_SAFE_ENV bb_fallback_3
  %17 = NUM_TO_FLOAT %6
  %18 = NUM_TO_FLOAT %7
  STORE_VECTOR R1, %17, %18, 0
  STORE_TAG R1, tvector
  JUMP bb_4
bb_4:
  INTERRUPT 7u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "VectorComparison1")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(a: vector, b: vector)
    return a == b
end
)"),
        R"(
; function foo($arg0, $arg1) line 2
bb_0:
  CHECK_TAG R0, tvector, exit(entry)
  CHECK_TAG R1, tvector, exit(entry)
  JUMP bb_4
bb_4:
  JUMP bb_bytecode_1
bb_bytecode_1:
  SET_SAVEDPC 1u
  %7 = CMP_ANY R0, R1, eq
  STORE_INT R2, %7
  STORE_TAG R2, tboolean
  JUMP bb_bytecode_3
bb_bytecode_3:
  INTERRUPT 4u
  RETURN R2, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "VectorComparison2")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(a: vector, b)
    return a == b
end
)"),
        R"(
; function foo($arg0, $arg1) line 2
bb_0:
  CHECK_TAG R0, tvector, exit(entry)
  JUMP bb_4
bb_4:
  JUMP bb_bytecode_1
bb_bytecode_1:
  SET_SAVEDPC 1u
  %5 = CMP_ANY R0, R1, eq
  STORE_INT R2, %5
  STORE_TAG R2, tboolean
  JUMP bb_bytecode_3
bb_bytecode_3:
  INTERRUPT 4u
  RETURN R2, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "ComparisonPropagationWall")
{
    // After CMP_ANY 'z' cannot reuse any SSA registers before
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(a, b)
    local x = type(b)
    local y = (not a) ~= b
    local z = type(b)
    return x, y, z
end
)"),
        R"(
; function foo($arg0, $arg1) line 2
bb_bytecode_0:
  CHECK_SAFE_ENV bb_fallback_3
  %1 = LOAD_TAG R1
  %2 = GET_TYPE %1
  STORE_POINTER R2, %2
  STORE_TAG R2, tstring
  JUMP bb_4
bb_4:
  %18 = LOAD_TAG R0
  %19 = LOAD_INT R0
  %20 = NOT_ANY %18, %19
  STORE_INT R4, %20
  STORE_TAG R4, tboolean
  SET_SAVEDPC 7u
  %24 = CMP_ANY R4, R1, eq
  %25 = SUB_INT 1i, %24
  STORE_INT R3, %25
  STORE_TAG R3, tboolean
  JUMP bb_bytecode_2
bb_bytecode_2:
  CHECK_SAFE_ENV bb_fallback_7
  %33 = LOAD_TAG R1
  %34 = GET_TYPE %33
  STORE_POINTER R4, %34
  STORE_TAG R4, tstring
  JUMP bb_8
bb_8:
  INTERRUPT 15u
  RETURN R2, 3i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "VectorLoadStoreOnlySamePrecision")
{
    ensureVectorFloat();
    ensureVectorSize3();

    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function test(x: number, y: number)
    local vec = vector.create(x, y, 0)
    return vec.X + vec.Y + vec.Z
end
)"),
        R"(
; function test($arg0, $arg1) line 2
bb_0:
  CHECK_TAG R0, tnumber, exit(entry)
  CHECK_TAG R1, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  STORE_DOUBLE R5, 0
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_3
  %15 = LOAD_DOUBLE R0
  %16 = LOAD_DOUBLE R1
  %18 = NUM_TO_FLOAT %15
  %19 = NUM_TO_FLOAT %16
  STORE_VECTOR R2, %18, %19, 0
  STORE_TAG R2, tvector
  JUMP bb_4
bb_4:
  CHECK_TAG R2, tvector, exit(8)
  %40 = LOAD_FLOAT R2, 0i
  %41 = FLOAT_TO_NUM %40
  %46 = LOAD_FLOAT R2, 4i
  %47 = FLOAT_TO_NUM %46
  %56 = ADD_NUM %41, %47
  %61 = LOAD_FLOAT R2, 8i
  %62 = FLOAT_TO_NUM %61
  %71 = ADD_NUM %56, %62
  STORE_DOUBLE R3, %71
  STORE_TAG R3, tnumber
  INTERRUPT 16u
  RETURN R3, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "NonNumericalComparison1")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(a: string, b: string, c: {}, d: {})
    return a == b and c == d
end
)"),
        R"(
; function foo($arg0, $arg1, $arg2, $arg3) line 2
bb_0:
  CHECK_TAG R0, tstring, exit(entry)
  CHECK_TAG R1, tstring, exit(entry)
  CHECK_TAG R2, ttable, exit(entry)
  CHECK_TAG R3, ttable, exit(entry)
  JUMP bb_4
bb_4:
  JUMP bb_bytecode_1
bb_bytecode_1:
  STORE_INT R4, 0i
  STORE_TAG R4, tboolean
  SET_SAVEDPC 2u
  %13 = CMP_ANY R0, R1, eq
  JUMP_CMP_INT %13, 0i, eq, bb_bytecode_3, bb_5
bb_5:
  SET_SAVEDPC 4u
  %16 = CMP_ANY R2, R3, eq
  STORE_INT R4, %16
  STORE_TAG R4, tboolean
  JUMP bb_bytecode_3
bb_bytecode_3:
  INTERRUPT 7u
  RETURN R4, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "NonNumericalComparison2")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(a: string, b: string, c: {}, d: {})
    return a > b and c > d
end
)"),
        R"(
; function foo($arg0, $arg1, $arg2, $arg3) line 2
bb_0:
  CHECK_TAG R0, tstring, exit(entry)
  CHECK_TAG R1, tstring, exit(entry)
  CHECK_TAG R2, ttable, exit(entry)
  CHECK_TAG R3, ttable, exit(entry)
  JUMP bb_4
bb_4:
  JUMP bb_bytecode_1
bb_bytecode_1:
  STORE_INT R4, 0i
  STORE_TAG R4, tboolean
  SET_SAVEDPC 2u
  %13 = CMP_ANY R1, R0, lt
  JUMP_CMP_INT %13, 0i, eq, bb_bytecode_3, bb_5
bb_5:
  SET_SAVEDPC 4u
  %16 = CMP_ANY R3, R2, lt
  JUMP_CMP_INT %16, 0i, eq, bb_6, bb_bytecode_2
bb_6:
  STORE_INT R4, 0i
  STORE_TAG R4, tboolean
  JUMP bb_bytecode_3
bb_bytecode_2:
  STORE_INT R4, 1i
  STORE_TAG R4, tboolean
  JUMP bb_bytecode_3
bb_bytecode_3:
  INTERRUPT 7u
  RETURN R4, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "BufferRelatedIndicesPositiveBase")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(buf: buffer, a: number)
    return buffer.readi32(buf, a) + buffer.readi32(buf, a + 4) + buffer.readi32(buf, a + 8)
end
)"),
        R"(
; function foo($arg0, $arg1) line 2
bb_0:
  CHECK_TAG R0, tbuffer, exit(entry)
  CHECK_TAG R1, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  CHECK_SAFE_ENV bb_fallback_3
  %11 = LOAD_POINTER R0
  %12 = LOAD_DOUBLE R1
  %13 = NUM_TO_INT %12
  CHECK_BUFFER_LEN %11, %13, 0i, 12i, %12, exit(2)
  %15 = BUFFER_READI32 %11, %13, tbuffer
  %16 = INT_TO_NUM %15
  JUMP bb_linear_15
bb_linear_15:
  %124 = ADD_INT %13, 4i
  %126 = BUFFER_READI32 %11, %124, tbuffer
  %127 = INT_TO_NUM %126
  %137 = ADD_NUM %16, %127
  %146 = ADD_INT %13, 8i
  %148 = BUFFER_READI32 %11, %146, tbuffer
  %149 = INT_TO_NUM %148
  %158 = ADD_NUM %137, %149
  STORE_DOUBLE R2, %158
  STORE_TAG R2, tnumber
  INTERRUPT 23u
  RETURN R2, 1i
bb_4:
  %36 = LOAD_DOUBLE R1
  %37 = ADD_NUM %36, 4
  STORE_DOUBLE R7, %37
  STORE_TAG R7, tnumber
  CHECK_SAFE_ENV bb_fallback_7
  %45 = LOAD_POINTER R0
  %47 = NUM_TO_INT %37
  CHECK_BUFFER_LEN %45, %47, 0i, 4i, undef, exit(10)
  %49 = BUFFER_READI32 %45, %47, tbuffer
  %50 = INT_TO_NUM %49
  STORE_DOUBLE R5, %50
  STORE_TAG R5, tnumber
  JUMP bb_8
bb_8:
  CHECK_TAG R4, tnumber, exit(14)
  CHECK_TAG R5, tnumber, exit(14)
  %70 = LOAD_DOUBLE R4
  %72 = ADD_NUM %70, R5
  STORE_DOUBLE R3, %72
  STORE_TAG R3, tnumber
  %77 = LOAD_DOUBLE R1
  %78 = ADD_NUM %77, 8
  STORE_DOUBLE R6, %78
  STORE_TAG R6, tnumber
  CHECK_SAFE_ENV bb_fallback_11
  %86 = LOAD_POINTER R0
  %88 = NUM_TO_INT %78
  CHECK_BUFFER_LEN %86, %88, 0i, 4i, undef, exit(18)
  %90 = BUFFER_READI32 %86, %88, tbuffer
  %91 = INT_TO_NUM %90
  STORE_DOUBLE R4, %91
  JUMP bb_12
bb_12:
  CHECK_TAG R3, tnumber, exit(22)
  CHECK_TAG R4, tnumber, exit(22)
  %111 = LOAD_DOUBLE R3
  %113 = ADD_NUM %111, R4
  STORE_DOUBLE R2, %113
  STORE_TAG R2, tnumber
  INTERRUPT 23u
  RETURN R2, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "BufferRelatedIndicesPositiveBaseInverted")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(buf: buffer, a: number)
    return buffer.readi32(buf, a + 8) + buffer.readi32(buf, a + 4) + buffer.readi32(buf, a + 0)
end
)"),
        R"(
; function foo($arg0, $arg1) line 2
bb_0:
  CHECK_TAG R0, tbuffer, exit(entry)
  CHECK_TAG R1, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %8 = LOAD_DOUBLE R1
  %9 = ADD_NUM %8, 8
  STORE_DOUBLE R6, %9
  STORE_TAG R6, tnumber
  CHECK_SAFE_ENV bb_fallback_3
  %17 = LOAD_POINTER R0
  %19 = NUM_TO_INT %9
  CHECK_BUFFER_LEN %17, %19, -8i, 4i, %9, exit(3)
  %21 = BUFFER_READI32 %17, %19, tbuffer
  %22 = INT_TO_NUM %21
  JUMP bb_linear_15
bb_linear_15:
  %128 = ADD_INT %19, -4i
  %130 = BUFFER_READI32 %17, %128, tbuffer
  %131 = INT_TO_NUM %130
  %141 = ADD_NUM %22, %131
  %150 = ADD_INT %19, -8i
  %152 = BUFFER_READI32 %17, %150, tbuffer
  %153 = INT_TO_NUM %152
  %162 = ADD_NUM %141, %153
  STORE_DOUBLE R2, %162
  STORE_TAG R2, tnumber
  INTERRUPT 23u
  RETURN R2, 1i
bb_4:
  %40 = LOAD_DOUBLE R1
  %41 = ADD_NUM %40, 4
  STORE_DOUBLE R7, %41
  STORE_TAG R7, tnumber
  CHECK_SAFE_ENV bb_fallback_7
  %49 = LOAD_POINTER R0
  %51 = NUM_TO_INT %41
  CHECK_BUFFER_LEN %49, %51, 0i, 4i, undef, exit(10)
  %53 = BUFFER_READI32 %49, %51, tbuffer
  %54 = INT_TO_NUM %53
  STORE_DOUBLE R5, %54
  STORE_TAG R5, tnumber
  JUMP bb_8
bb_8:
  CHECK_TAG R4, tnumber, exit(14)
  CHECK_TAG R5, tnumber, exit(14)
  %74 = LOAD_DOUBLE R4
  %76 = ADD_NUM %74, R5
  STORE_DOUBLE R3, %76
  STORE_TAG R3, tnumber
  %81 = LOAD_DOUBLE R1
  %82 = ADD_NUM %81, 0
  STORE_DOUBLE R6, %82
  STORE_TAG R6, tnumber
  CHECK_SAFE_ENV bb_fallback_11
  %90 = LOAD_POINTER R0
  %92 = NUM_TO_INT %81
  CHECK_BUFFER_LEN %90, %92, 0i, 4i, undef, exit(18)
  %94 = BUFFER_READI32 %90, %92, tbuffer
  %95 = INT_TO_NUM %94
  STORE_DOUBLE R4, %95
  JUMP bb_12
bb_12:
  CHECK_TAG R3, tnumber, exit(22)
  CHECK_TAG R4, tnumber, exit(22)
  %115 = LOAD_DOUBLE R3
  %117 = ADD_NUM %115, R4
  STORE_DOUBLE R2, %117
  STORE_TAG R2, tnumber
  INTERRUPT 23u
  RETURN R2, 1i
)"
    );
}
TEST_CASE_FIXTURE(LoweringFixture, "BufferRelatedIndicesPositiveDynamicBase")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(index: buffer, data: buffer, a: number)
    local i = buffer.readi32(index, a)
    return buffer.readf32(data, i + 0) * buffer.readf32(data, i + 4) * buffer.readf32(data, i + 8)
end
)"),
        R"(
; function foo($arg0, $arg1, $arg2) line 2
bb_0:
  CHECK_TAG R0, tbuffer, exit(entry)
  CHECK_TAG R1, tbuffer, exit(entry)
  CHECK_TAG R2, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  CHECK_SAFE_ENV bb_fallback_3
  %13 = LOAD_POINTER R0
  %14 = LOAD_DOUBLE R2
  %15 = NUM_TO_INT %14
  CHECK_BUFFER_LEN %13, %15, 0i, 4i, undef, exit(2)
  %17 = BUFFER_READI32 %13, %15, tbuffer
  %18 = INT_TO_NUM %17
  JUMP bb_linear_19
bb_linear_19:
  %159 = LOAD_POINTER R1
  CHECK_BUFFER_LEN %159, %17, 0i, 4i, undef, bb_exit_20
   ; exit sync: R8, R3, {%18}
  %162 = BUFFER_READF32 %159, %17, tbuffer
  %163 = FLOAT_TO_NUM %162
  %170 = ADD_NUM %18, 4
  %175 = NUM_TO_INT %170
  CHECK_BUFFER_LEN %159, %175, 0i, 8i, %170, bb_exit_21
   ; exit sync: R9, R8, R6, R3, {%170, %163, %18}
  %177 = BUFFER_READF32 %159, %175, tbuffer
  %178 = FLOAT_TO_NUM %177
  %188 = MUL_NUM %163, %178
  %199 = ADD_INT %175, 4i
  %201 = BUFFER_READF32 %159, %199, tbuffer
  %202 = FLOAT_TO_NUM %201
  %211 = MUL_NUM %188, %202
  STORE_DOUBLE R4, %211
  STORE_TAG R4, tnumber
  INTERRUPT 30u
  RETURN R4, 1i
bb_4:
  CHECK_TAG R3, tnumber, exit(7)
  %38 = LOAD_DOUBLE R3
  %39 = ADD_NUM %38, 0
  STORE_DOUBLE R8, %39
  STORE_TAG R8, tnumber
  CHECK_SAFE_ENV bb_fallback_7
  %47 = LOAD_POINTER R1
  %49 = NUM_TO_INT %38
  CHECK_BUFFER_LEN %47, %49, 0i, 4i, undef, exit(10)
  %51 = BUFFER_READF32 %47, %49, tbuffer
  %52 = FLOAT_TO_NUM %51
  STORE_DOUBLE R6, %52
  STORE_TAG R6, tnumber
  JUMP bb_8
bb_8:
  CHECK_TAG R3, tnumber, exit(14)
  %70 = LOAD_DOUBLE R3
  %71 = ADD_NUM %70, 4
  STORE_DOUBLE R9, %71
  STORE_TAG R9, tnumber
  CHECK_SAFE_ENV bb_fallback_11
  %79 = LOAD_POINTER R1
  %81 = NUM_TO_INT %71
  CHECK_BUFFER_LEN %79, %81, 0i, 4i, undef, exit(17)
  %83 = BUFFER_READF32 %79, %81, tbuffer
  %84 = FLOAT_TO_NUM %83
  STORE_DOUBLE R7, %84
  STORE_TAG R7, tnumber
  JUMP bb_12
bb_12:
  CHECK_TAG R6, tnumber, exit(21)
  CHECK_TAG R7, tnumber, exit(21)
  %104 = LOAD_DOUBLE R6
  %106 = MUL_NUM %104, R7
  STORE_DOUBLE R5, %106
  STORE_TAG R5, tnumber
  CHECK_TAG R3, tnumber, exit(22)
  %111 = LOAD_DOUBLE R3
  %112 = ADD_NUM %111, 8
  STORE_DOUBLE R8, %112
  STORE_TAG R8, tnumber
  CHECK_SAFE_ENV bb_fallback_15
  %120 = LOAD_POINTER R1
  %122 = NUM_TO_INT %112
  CHECK_BUFFER_LEN %120, %122, 0i, 4i, undef, exit(25)
  %124 = BUFFER_READF32 %120, %122, tbuffer
  %125 = FLOAT_TO_NUM %124
  STORE_DOUBLE R6, %125
  JUMP bb_16
bb_16:
  CHECK_TAG R5, tnumber, exit(29)
  CHECK_TAG R6, tnumber, exit(29)
  %145 = LOAD_DOUBLE R5
  %147 = MUL_NUM %145, R6
  STORE_DOUBLE R4, %147
  STORE_TAG R4, tnumber
  INTERRUPT 30u
  RETURN R4, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "BufferRelatedIndicesPositiveLoopRangeBase")
{
    ScopedFastFlag emitCallFb{FFlag::LuauEmitCallFeedback, true};
    ScopedFastFlag luauBackedgeHeapCheck{FFlag::LuauBackedgeHeapCheck, true};
    ScopedFastFlag luauCodegenPropagateFallbackTags{FFlag::LuauCodegenPropagateFallbackTags, true};

    // TODO: opportunity 1 - buffer.len is not a fastcall, but under safe env we can treat it like one and read buffer len field
    // TODO: opportunity 2 - range of 'i' is known, we can check it in loop header
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(buf: buffer, a: number)
    local s = 0
    for i = 0, buffer.len(buf) - 1, 12 do
        s += buffer.readf32(buf, i) * buffer.readf32(buf, i + 4) * buffer.readf32(buf, i + 8)
    end
    return s
end
)"),
        R"(
; function foo($arg0, $arg1) line 2
bb_0:
  CHECK_TAG R0, tbuffer, exit(entry)
  CHECK_TAG R1, tnumber, exit(entry)
  JUMP bb_4
bb_4:
  JUMP bb_bytecode_1
bb_bytecode_1:
  STORE_DOUBLE R2, 0
  STORE_TAG R2, tnumber
  STORE_DOUBLE R5, 0
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_5
  GET_CACHED_IMPORT R6, K3 (nil), 2148534272u ('buffer'.'len'), 3u
  JUMP bb_6
bb_6:
  %15 = LOAD_TVALUE R0, 0i, tbuffer
  STORE_TVALUE R7, %15
  INTERRUPT 5u
  SET_SAVEDPC 7u
  CALL R6, 1i, 1i
  CHECK_TAG R6, tnumber, bb_fallback_7
  %22 = LOAD_DOUBLE R6
  %23 = SUB_NUM %22, 1
  STORE_DOUBLE R3, %23
  STORE_TAG R3, tnumber
  JUMP bb_8
bb_8:
  STORE_DOUBLE R4, 12
  STORE_TAG R4, tnumber
  CHECK_TAG R3, tnumber, exit(9)
  %36 = LOAD_DOUBLE R3
  JUMP_CMP_NUM R5, %36, not_le, bb_bytecode_3, bb_bytecode_2
bb_bytecode_2:
  INTERRUPT 10u
  CHECK_SAFE_ENV bb_fallback_9
  CHECK_TAG R5, tnumber, exit(12)
  %45 = LOAD_POINTER R0
  %46 = LOAD_DOUBLE R5
  %47 = NUM_TO_INT %46
  CHECK_BUFFER_LEN %45, %47, 0i, 12i, %46, exit(12)
  %49 = BUFFER_READF32 %45, %47, tbuffer
  %50 = FLOAT_TO_NUM %49
  JUMP bb_linear_21
bb_linear_21:
  %175 = ADD_INT %47, 4i
  %177 = BUFFER_READF32 %45, %175, tbuffer
  %178 = FLOAT_TO_NUM %177
  %188 = MUL_NUM %50, %178
  %199 = ADD_INT %47, 8i
  %201 = BUFFER_READF32 %45, %199, tbuffer
  %202 = FLOAT_TO_NUM %201
  %211 = MUL_NUM %188, %202
  %216 = LOAD_DOUBLE R2
  %217 = ADD_NUM %216, %211
  STORE_DOUBLE R2, %217
  CHECK_GC
  %220 = LOAD_DOUBLE R3
  %222 = ADD_NUM %46, 12
  STORE_DOUBLE R5, %222
  JUMP_CMP_NUM %222, %220, le, bb_bytecode_2, bb_bytecode_3
bb_10:
  CHECK_TAG R5, tnumber, exit(17)
  %70 = LOAD_DOUBLE R5
  %71 = ADD_NUM %70, 4
  STORE_DOUBLE R11, %71
  STORE_TAG R11, tnumber
  CHECK_SAFE_ENV bb_fallback_13
  %79 = LOAD_POINTER R0
  %81 = NUM_TO_INT %71
  CHECK_BUFFER_LEN %79, %81, 0i, 4i, undef, exit(20)
  %83 = BUFFER_READF32 %79, %81, tbuffer
  %84 = FLOAT_TO_NUM %83
  STORE_DOUBLE R9, %84
  STORE_TAG R9, tnumber
  JUMP bb_14
bb_14:
  CHECK_TAG R8, tnumber, exit(24)
  CHECK_TAG R9, tnumber, exit(24)
  %104 = LOAD_DOUBLE R8
  %106 = MUL_NUM %104, R9
  STORE_DOUBLE R7, %106
  STORE_TAG R7, tnumber
  CHECK_TAG R5, tnumber, exit(25)
  %111 = LOAD_DOUBLE R5
  %112 = ADD_NUM %111, 8
  STORE_DOUBLE R10, %112
  STORE_TAG R10, tnumber
  CHECK_SAFE_ENV bb_fallback_17
  %120 = LOAD_POINTER R0
  %122 = NUM_TO_INT %112
  CHECK_BUFFER_LEN %120, %122, 0i, 4i, undef, exit(28)
  %124 = BUFFER_READF32 %120, %122, tbuffer
  %125 = FLOAT_TO_NUM %124
  STORE_DOUBLE R8, %125
  JUMP bb_18
bb_18:
  CHECK_TAG R7, tnumber, exit(32)
  CHECK_TAG R8, tnumber, exit(32)
  %145 = LOAD_DOUBLE R7
  %147 = MUL_NUM %145, R8
  CHECK_TAG R2, tnumber, bb_exit_22
   ; exit sync: R6, {%147}
  %154 = LOAD_DOUBLE R2
  %156 = ADD_NUM %154, %147
  STORE_DOUBLE R2, %156
  CHECK_GC
  %159 = LOAD_DOUBLE R3
  %160 = LOAD_DOUBLE R5
  %161 = ADD_NUM %160, 12
  STORE_DOUBLE R5, %161
  JUMP_CMP_NUM %161, %159, le, bb_bytecode_2, bb_bytecode_3
bb_bytecode_3:
  INTERRUPT 35u
  RETURN R2, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "BufferRelatedIndicesPositiveAdvancingBase")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(buf: buffer, pos: number, a: number, b: number, c: number)
    buffer.writei32(buf, pos, a)
    pos += 4
    buffer.writei32(buf, pos, b)
    pos += 4
    buffer.writei32(buf, pos, c)
    pos += 4

    return pos
end
)"),
        R"(
; function foo($arg0, $arg1, $arg2, $arg3, $arg4) line 2
bb_0:
  CHECK_TAG R0, tbuffer, exit(entry)
  CHECK_TAG R1, tnumber, exit(entry)
  CHECK_TAG R2, tnumber, exit(entry)
  CHECK_TAG R3, tnumber, exit(entry)
  CHECK_TAG R4, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  CHECK_SAFE_ENV bb_fallback_3
  %19 = LOAD_POINTER R0
  %20 = LOAD_DOUBLE R1
  %21 = NUM_TO_INT %20
  CHECK_BUFFER_LEN %19, %21, 0i, 12i, %20, exit(2)
  %23 = LOAD_DOUBLE R2
  %24 = NUM_TO_UINT %23
  BUFFER_WRITEI32 %19, %21, %24, tbuffer
  JUMP bb_linear_15
bb_linear_15:
  %125 = ADD_NUM %20, 4
  %129 = ADD_INT %21, 4i
  %131 = LOAD_DOUBLE R3
  %132 = NUM_TO_UINT %131
  BUFFER_WRITEI32 %19, %129, %132, tbuffer
  %138 = ADD_NUM %125, 4
  %142 = ADD_INT %21, 8i
  %144 = LOAD_DOUBLE R4
  %145 = NUM_TO_UINT %144
  BUFFER_WRITEI32 %19, %142, %145, tbuffer
  %151 = ADD_NUM %138, 4
  STORE_DOUBLE R1, %151
  INTERRUPT 27u
  RETURN R1, 1i
bb_4:
  CHECK_TAG R1, tnumber, exit(8)
  %45 = LOAD_DOUBLE R1
  %46 = ADD_NUM %45, 4
  STORE_DOUBLE R1, %46
  CHECK_SAFE_ENV bb_fallback_7
  %55 = LOAD_POINTER R0
  %57 = NUM_TO_INT %46
  CHECK_BUFFER_LEN %55, %57, 0i, 4i, undef, exit(11)
  %59 = LOAD_DOUBLE R3
  %60 = NUM_TO_UINT %59
  BUFFER_WRITEI32 %55, %57, %60, tbuffer
  JUMP bb_8
bb_8:
  CHECK_TAG R1, tnumber, exit(17)
  %81 = LOAD_DOUBLE R1
  %82 = ADD_NUM %81, 4
  STORE_DOUBLE R1, %82
  CHECK_SAFE_ENV bb_fallback_11
  %91 = LOAD_POINTER R0
  %93 = NUM_TO_INT %82
  CHECK_BUFFER_LEN %91, %93, 0i, 4i, undef, exit(20)
  %95 = LOAD_DOUBLE R4
  %96 = NUM_TO_UINT %95
  BUFFER_WRITEI32 %91, %93, %96, tbuffer
  JUMP bb_12
bb_12:
  CHECK_TAG R1, tnumber, exit(26)
  %117 = LOAD_DOUBLE R1
  %118 = ADD_NUM %117, 4
  STORE_DOUBLE R1, %118
  INTERRUPT 27u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "BufferRelatedIndicesNegativeBase")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(buf: buffer, a: number)
    return buffer.readi32(buf, a - 8) + buffer.readi32(buf, a - 4) + buffer.readi32(buf, a - 0)
end
)"),
        R"(
; function foo($arg0, $arg1) line 2
bb_0:
  CHECK_TAG R0, tbuffer, exit(entry)
  CHECK_TAG R1, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %8 = LOAD_DOUBLE R1
  %9 = SUB_NUM %8, 8
  STORE_DOUBLE R6, %9
  STORE_TAG R6, tnumber
  CHECK_SAFE_ENV bb_fallback_3
  %17 = LOAD_POINTER R0
  %19 = NUM_TO_INT %9
  CHECK_BUFFER_LEN %17, %19, 0i, 12i, %9, exit(3)
  %21 = BUFFER_READI32 %17, %19, tbuffer
  %22 = INT_TO_NUM %21
  JUMP bb_linear_15
bb_linear_15:
  %128 = ADD_INT %19, 4i
  %130 = BUFFER_READI32 %17, %128, tbuffer
  %131 = INT_TO_NUM %130
  %141 = ADD_NUM %22, %131
  %149 = ADD_INT %19, 8i
  %151 = BUFFER_READI32 %17, %149, tbuffer
  %152 = INT_TO_NUM %151
  %161 = ADD_NUM %141, %152
  STORE_DOUBLE R2, %161
  STORE_TAG R2, tnumber
  INTERRUPT 23u
  RETURN R2, 1i
bb_4:
  %40 = LOAD_DOUBLE R1
  %41 = SUB_NUM %40, 4
  STORE_DOUBLE R7, %41
  STORE_TAG R7, tnumber
  CHECK_SAFE_ENV bb_fallback_7
  %49 = LOAD_POINTER R0
  %51 = NUM_TO_INT %41
  CHECK_BUFFER_LEN %49, %51, 0i, 4i, undef, exit(10)
  %53 = BUFFER_READI32 %49, %51, tbuffer
  %54 = INT_TO_NUM %53
  STORE_DOUBLE R5, %54
  STORE_TAG R5, tnumber
  JUMP bb_8
bb_8:
  CHECK_TAG R4, tnumber, exit(14)
  CHECK_TAG R5, tnumber, exit(14)
  %74 = LOAD_DOUBLE R4
  %76 = ADD_NUM %74, R5
  STORE_DOUBLE R3, %76
  STORE_TAG R3, tnumber
  %81 = LOAD_DOUBLE R1
  STORE_DOUBLE R6, %81
  STORE_TAG R6, tnumber
  CHECK_SAFE_ENV bb_fallback_11
  %90 = LOAD_POINTER R0
  %92 = NUM_TO_INT %81
  CHECK_BUFFER_LEN %90, %92, 0i, 4i, undef, exit(18)
  %94 = BUFFER_READI32 %90, %92, tbuffer
  %95 = INT_TO_NUM %94
  STORE_DOUBLE R4, %95
  JUMP bb_12
bb_12:
  CHECK_TAG R3, tnumber, exit(22)
  CHECK_TAG R4, tnumber, exit(22)
  %115 = LOAD_DOUBLE R3
  %117 = ADD_NUM %115, R4
  STORE_DOUBLE R2, %117
  STORE_TAG R2, tnumber
  INTERRUPT 23u
  RETURN R2, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "BufferRelatedIndicesMixedBase")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(buf: buffer, a: number)
    return buffer.readi32(buf, a) + buffer.readi32(buf, a - 4) + buffer.readi32(buf, a + 4)
end
)"),
        R"(
; function foo($arg0, $arg1) line 2
bb_0:
  CHECK_TAG R0, tbuffer, exit(entry)
  CHECK_TAG R1, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  CHECK_SAFE_ENV bb_fallback_3
  %11 = LOAD_POINTER R0
  %12 = LOAD_DOUBLE R1
  %13 = NUM_TO_INT %12
  CHECK_BUFFER_LEN %11, %13, -4i, 8i, %12, exit(2)
  %15 = BUFFER_READI32 %11, %13, tbuffer
  %16 = INT_TO_NUM %15
  JUMP bb_linear_15
bb_linear_15:
  %124 = ADD_INT %13, -4i
  %126 = BUFFER_READI32 %11, %124, tbuffer
  %127 = INT_TO_NUM %126
  %137 = ADD_NUM %16, %127
  %146 = ADD_INT %13, 4i
  %148 = BUFFER_READI32 %11, %146, tbuffer
  %149 = INT_TO_NUM %148
  %158 = ADD_NUM %137, %149
  STORE_DOUBLE R2, %158
  STORE_TAG R2, tnumber
  INTERRUPT 23u
  RETURN R2, 1i
bb_4:
  %36 = LOAD_DOUBLE R1
  %37 = SUB_NUM %36, 4
  STORE_DOUBLE R7, %37
  STORE_TAG R7, tnumber
  CHECK_SAFE_ENV bb_fallback_7
  %45 = LOAD_POINTER R0
  %47 = NUM_TO_INT %37
  CHECK_BUFFER_LEN %45, %47, 0i, 4i, undef, exit(10)
  %49 = BUFFER_READI32 %45, %47, tbuffer
  %50 = INT_TO_NUM %49
  STORE_DOUBLE R5, %50
  STORE_TAG R5, tnumber
  JUMP bb_8
bb_8:
  CHECK_TAG R4, tnumber, exit(14)
  CHECK_TAG R5, tnumber, exit(14)
  %70 = LOAD_DOUBLE R4
  %72 = ADD_NUM %70, R5
  STORE_DOUBLE R3, %72
  STORE_TAG R3, tnumber
  %77 = LOAD_DOUBLE R1
  %78 = ADD_NUM %77, 4
  STORE_DOUBLE R6, %78
  STORE_TAG R6, tnumber
  CHECK_SAFE_ENV bb_fallback_11
  %86 = LOAD_POINTER R0
  %88 = NUM_TO_INT %78
  CHECK_BUFFER_LEN %86, %88, 0i, 4i, undef, exit(18)
  %90 = BUFFER_READI32 %86, %88, tbuffer
  %91 = INT_TO_NUM %90
  STORE_DOUBLE R4, %91
  JUMP bb_12
bb_12:
  CHECK_TAG R3, tnumber, exit(22)
  CHECK_TAG R4, tnumber, exit(22)
  %111 = LOAD_DOUBLE R3
  %113 = ADD_NUM %111, R4
  STORE_DOUBLE R2, %113
  STORE_TAG R2, tnumber
  INTERRUPT 23u
  RETURN R2, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "BufferSanityPositive")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(zero: number, b1: buffer, b2: buffer)
    buffer.writei8(b1, zero + 0, buffer.readi8(b1, zero + 0))
    buffer.writeu8(b1, zero + 0, buffer.readu8(b1, zero + 0))

    buffer.writei8(b2, zero + 0, buffer.readi8(b2, zero + 0))
    buffer.writeu8(b2, zero + 0, buffer.readu8(b2, zero + 0))
    buffer.writei8(b2, zero + 1, buffer.readi8(b2, zero + 1))
    buffer.writeu8(b2, zero + 1, buffer.readu8(b2, zero + 1))
    buffer.writei16(b2, zero + 0, buffer.readi16(b2, zero + 0))
    buffer.writeu16(b2, zero + 0, buffer.readu16(b2, zero + 0))
end
)"),
        R"(
; function foo($arg0, $arg1, $arg2) line 2
bb_0:
  CHECK_TAG R0, tnumber, exit(entry)
  CHECK_TAG R1, tbuffer, exit(entry)
  CHECK_TAG R2, tbuffer, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %10 = LOAD_DOUBLE R0
  %11 = ADD_NUM %10, 0
  STORE_DOUBLE R5, %11
  STORE_TAG R5, tnumber
  STORE_DOUBLE R8, %11
  STORE_TAG R8, tnumber
  CHECK_SAFE_ENV bb_fallback_3
  %25 = LOAD_POINTER R1
  %27 = NUM_TO_INT %10
  CHECK_BUFFER_LEN %25, %27, 0i, 1i, undef, exit(4)
  %29 = BUFFER_READI8 %25, %27, tbuffer
  JUMP bb_linear_67
bb_linear_67:
  BUFFER_WRITEI8 %25, %27, %29, tbuffer
  %553 = BUFFER_READU8 %25, %27, tbuffer
  BUFFER_WRITEI8 %25, %27, %553, tbuffer
  %578 = LOAD_POINTER R2
  CHECK_BUFFER_LEN %578, %27, 0i, 2i, %10, bb_exit_68
   ; exit sync: R6, {%553}
  %581 = BUFFER_READI8 %578, %27, tbuffer
  BUFFER_WRITEI8 %578, %27, %581, tbuffer
  %609 = BUFFER_READU8 %578, %27, tbuffer
  BUFFER_WRITEI8 %578, %27, %609, tbuffer
  %635 = ADD_INT %27, 1i
  %637 = BUFFER_READI8 %578, %635, tbuffer
  BUFFER_WRITEI8 %578, %635, %637, tbuffer
  %665 = BUFFER_READU8 %578, %635, tbuffer
  BUFFER_WRITEI8 %578, %635, %665, tbuffer
  %693 = BUFFER_READI16 %578, %27, tbuffer
  BUFFER_WRITEI16 %578, %27, %693, tbuffer
  %721 = BUFFER_READU16 %578, %27, tbuffer
  BUFFER_WRITEI16 %578, %27, %721, tbuffer
  INTERRUPT 112u
  RETURN R0, 0i
bb_4:
  CHECK_SAFE_ENV bb_fallback_7
  CHECK_TAG R5, tnumber, exit(10)
  CHECK_TAG R6, tnumber, exit(10)
  %53 = LOAD_POINTER R1
  %54 = LOAD_DOUBLE R5
  %55 = NUM_TO_INT %54
  CHECK_BUFFER_LEN %53, %55, 0i, 1i, undef, exit(10)
  %57 = LOAD_DOUBLE R6
  %58 = NUM_TO_UINT %57
  BUFFER_WRITEI8 %53, %55, %58, tbuffer
  JUMP bb_8
bb_8:
  %75 = LOAD_DOUBLE R0
  %76 = ADD_NUM %75, 0
  STORE_DOUBLE R5, %76
  STORE_TAG R5, tnumber
  STORE_DOUBLE R8, %76
  STORE_TAG R8, tnumber
  CHECK_SAFE_ENV bb_fallback_11
  %90 = LOAD_POINTER R1
  %92 = NUM_TO_INT %75
  CHECK_BUFFER_LEN %90, %92, 0i, 1i, undef, exit(18)
  %94 = BUFFER_READU8 %90, %92, tbuffer
  %95 = INT_TO_NUM %94
  STORE_DOUBLE R6, %95
  STORE_TAG R6, tnumber
  JUMP bb_12
bb_12:
  CHECK_SAFE_ENV bb_fallback_15
  CHECK_TAG R5, tnumber, exit(24)
  CHECK_TAG R6, tnumber, exit(24)
  %118 = LOAD_POINTER R1
  %119 = LOAD_DOUBLE R5
  %120 = NUM_TO_INT %119
  CHECK_BUFFER_LEN %118, %120, 0i, 1i, undef, exit(24)
  %122 = LOAD_DOUBLE R6
  %123 = NUM_TO_UINT %122
  BUFFER_WRITEI8 %118, %120, %123, tbuffer
  JUMP bb_16
bb_16:
  %140 = LOAD_DOUBLE R0
  %141 = ADD_NUM %140, 0
  STORE_DOUBLE R5, %141
  STORE_TAG R5, tnumber
  STORE_DOUBLE R8, %141
  STORE_TAG R8, tnumber
  CHECK_SAFE_ENV bb_fallback_19
  %155 = LOAD_POINTER R2
  %157 = NUM_TO_INT %140
  CHECK_BUFFER_LEN %155, %157, 0i, 1i, undef, exit(32)
  %159 = BUFFER_READI8 %155, %157, tbuffer
  %160 = INT_TO_NUM %159
  STORE_DOUBLE R6, %160
  STORE_TAG R6, tnumber
  JUMP bb_20
bb_20:
  CHECK_SAFE_ENV bb_fallback_23
  CHECK_TAG R5, tnumber, exit(38)
  CHECK_TAG R6, tnumber, exit(38)
  %183 = LOAD_POINTER R2
  %184 = LOAD_DOUBLE R5
  %185 = NUM_TO_INT %184
  CHECK_BUFFER_LEN %183, %185, 0i, 1i, undef, exit(38)
  %187 = LOAD_DOUBLE R6
  %188 = NUM_TO_UINT %187
  BUFFER_WRITEI8 %183, %185, %188, tbuffer
  JUMP bb_24
bb_24:
  %205 = LOAD_DOUBLE R0
  %206 = ADD_NUM %205, 0
  STORE_DOUBLE R5, %206
  STORE_TAG R5, tnumber
  STORE_DOUBLE R8, %206
  STORE_TAG R8, tnumber
  CHECK_SAFE_ENV bb_fallback_27
  %220 = LOAD_POINTER R2
  %222 = NUM_TO_INT %205
  CHECK_BUFFER_LEN %220, %222, 0i, 1i, undef, exit(46)
  %224 = BUFFER_READU8 %220, %222, tbuffer
  %225 = INT_TO_NUM %224
  STORE_DOUBLE R6, %225
  STORE_TAG R6, tnumber
  JUMP bb_28
bb_28:
  CHECK_SAFE_ENV bb_fallback_31
  CHECK_TAG R5, tnumber, exit(52)
  CHECK_TAG R6, tnumber, exit(52)
  %248 = LOAD_POINTER R2
  %249 = LOAD_DOUBLE R5
  %250 = NUM_TO_INT %249
  CHECK_BUFFER_LEN %248, %250, 0i, 1i, undef, exit(52)
  %252 = LOAD_DOUBLE R6
  %253 = NUM_TO_UINT %252
  BUFFER_WRITEI8 %248, %250, %253, tbuffer
  JUMP bb_32
bb_32:
  %270 = LOAD_DOUBLE R0
  %271 = ADD_NUM %270, 1
  STORE_DOUBLE R5, %271
  STORE_TAG R5, tnumber
  STORE_DOUBLE R8, %271
  STORE_TAG R8, tnumber
  CHECK_SAFE_ENV bb_fallback_35
  %285 = LOAD_POINTER R2
  %287 = NUM_TO_INT %271
  CHECK_BUFFER_LEN %285, %287, 0i, 1i, undef, exit(60)
  %289 = BUFFER_READI8 %285, %287, tbuffer
  %290 = INT_TO_NUM %289
  STORE_DOUBLE R6, %290
  STORE_TAG R6, tnumber
  JUMP bb_36
bb_36:
  CHECK_SAFE_ENV bb_fallback_39
  CHECK_TAG R5, tnumber, exit(66)
  CHECK_TAG R6, tnumber, exit(66)
  %313 = LOAD_POINTER R2
  %314 = LOAD_DOUBLE R5
  %315 = NUM_TO_INT %314
  CHECK_BUFFER_LEN %313, %315, 0i, 1i, undef, exit(66)
  %317 = LOAD_DOUBLE R6
  %318 = NUM_TO_UINT %317
  BUFFER_WRITEI8 %313, %315, %318, tbuffer
  JUMP bb_40
bb_40:
  %335 = LOAD_DOUBLE R0
  %336 = ADD_NUM %335, 1
  STORE_DOUBLE R5, %336
  STORE_TAG R5, tnumber
  STORE_DOUBLE R8, %336
  STORE_TAG R8, tnumber
  CHECK_SAFE_ENV bb_fallback_43
  %350 = LOAD_POINTER R2
  %352 = NUM_TO_INT %336
  CHECK_BUFFER_LEN %350, %352, 0i, 1i, undef, exit(74)
  %354 = BUFFER_READU8 %350, %352, tbuffer
  %355 = INT_TO_NUM %354
  STORE_DOUBLE R6, %355
  STORE_TAG R6, tnumber
  JUMP bb_44
bb_44:
  CHECK_SAFE_ENV bb_fallback_47
  CHECK_TAG R5, tnumber, exit(80)
  CHECK_TAG R6, tnumber, exit(80)
  %378 = LOAD_POINTER R2
  %379 = LOAD_DOUBLE R5
  %380 = NUM_TO_INT %379
  CHECK_BUFFER_LEN %378, %380, 0i, 1i, undef, exit(80)
  %382 = LOAD_DOUBLE R6
  %383 = NUM_TO_UINT %382
  BUFFER_WRITEI8 %378, %380, %383, tbuffer
  JUMP bb_48
bb_48:
  %400 = LOAD_DOUBLE R0
  %401 = ADD_NUM %400, 0
  STORE_DOUBLE R5, %401
  STORE_TAG R5, tnumber
  STORE_DOUBLE R8, %401
  STORE_TAG R8, tnumber
  CHECK_SAFE_ENV bb_fallback_51
  %415 = LOAD_POINTER R2
  %417 = NUM_TO_INT %400
  CHECK_BUFFER_LEN %415, %417, 0i, 2i, undef, exit(88)
  %419 = BUFFER_READI16 %415, %417, tbuffer
  %420 = INT_TO_NUM %419
  STORE_DOUBLE R6, %420
  STORE_TAG R6, tnumber
  JUMP bb_52
bb_52:
  CHECK_SAFE_ENV bb_fallback_55
  CHECK_TAG R5, tnumber, exit(94)
  CHECK_TAG R6, tnumber, exit(94)
  %443 = LOAD_POINTER R2
  %444 = LOAD_DOUBLE R5
  %445 = NUM_TO_INT %444
  CHECK_BUFFER_LEN %443, %445, 0i, 2i, undef, exit(94)
  %447 = LOAD_DOUBLE R6
  %448 = NUM_TO_UINT %447
  BUFFER_WRITEI16 %443, %445, %448, tbuffer
  JUMP bb_56
bb_56:
  %465 = LOAD_DOUBLE R0
  %466 = ADD_NUM %465, 0
  STORE_DOUBLE R5, %466
  STORE_TAG R5, tnumber
  STORE_DOUBLE R8, %466
  STORE_TAG R8, tnumber
  CHECK_SAFE_ENV bb_fallback_59
  %480 = LOAD_POINTER R2
  %482 = NUM_TO_INT %465
  CHECK_BUFFER_LEN %480, %482, 0i, 2i, undef, exit(102)
  %484 = BUFFER_READU16 %480, %482, tbuffer
  %485 = INT_TO_NUM %484
  STORE_DOUBLE R6, %485
  STORE_TAG R6, tnumber
  JUMP bb_60
bb_60:
  CHECK_SAFE_ENV bb_fallback_63
  CHECK_TAG R5, tnumber, exit(108)
  CHECK_TAG R6, tnumber, exit(108)
  %508 = LOAD_POINTER R2
  %509 = LOAD_DOUBLE R5
  %510 = NUM_TO_INT %509
  CHECK_BUFFER_LEN %508, %510, 0i, 2i, undef, exit(108)
  %512 = LOAD_DOUBLE R6
  %513 = NUM_TO_UINT %512
  BUFFER_WRITEI16 %508, %510, %513, tbuffer
  JUMP bb_64
bb_64:
  INTERRUPT 112u
  RETURN R0, 0i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "BufferSanityNegative")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(one: number, b1: buffer, b2: buffer)
    buffer.writei8(b1, one - 1, buffer.readi8(b1, one - 1))
    buffer.writeu8(b1, one - 1, buffer.readu8(b1, one - 1))

    buffer.writei8(b2, one - 1, buffer.readi8(b2, one - 1))
    buffer.writeu8(b2, one - 1, buffer.readu8(b2, one - 1))
    buffer.writei8(b2, one - 0, buffer.readi8(b2, one - 0))
    buffer.writeu8(b2, one - 0, buffer.readu8(b2, one - 0))
    buffer.writei16(b2, one - 1, buffer.readi16(b2, one - 1))
    buffer.writeu16(b2, one - 1, buffer.readu16(b2, one - 1))
end
)"),
        R"(
; function foo($arg0, $arg1, $arg2) line 2
bb_0:
  CHECK_TAG R0, tnumber, exit(entry)
  CHECK_TAG R1, tbuffer, exit(entry)
  CHECK_TAG R2, tbuffer, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %10 = LOAD_DOUBLE R0
  %11 = SUB_NUM %10, 1
  STORE_DOUBLE R5, %11
  STORE_TAG R5, tnumber
  STORE_DOUBLE R8, %11
  STORE_TAG R8, tnumber
  CHECK_SAFE_ENV bb_fallback_3
  %25 = LOAD_POINTER R1
  %27 = NUM_TO_INT %11
  CHECK_BUFFER_LEN %25, %27, 0i, 1i, undef, exit(4)
  %29 = BUFFER_READI8 %25, %27, tbuffer
  JUMP bb_linear_67
bb_linear_67:
  BUFFER_WRITEI8 %25, %27, %29, tbuffer
  %553 = BUFFER_READU8 %25, %27, tbuffer
  BUFFER_WRITEI8 %25, %27, %553, tbuffer
  %578 = LOAD_POINTER R2
  CHECK_BUFFER_LEN %578, %27, 0i, 2i, %11, bb_exit_68
   ; exit sync: R6, {%553}
  %581 = BUFFER_READI8 %578, %27, tbuffer
  BUFFER_WRITEI8 %578, %27, %581, tbuffer
  %609 = BUFFER_READU8 %578, %27, tbuffer
  BUFFER_WRITEI8 %578, %27, %609, tbuffer
  %634 = ADD_INT %27, 1i
  %636 = BUFFER_READI8 %578, %634, tbuffer
  BUFFER_WRITEI8 %578, %634, %636, tbuffer
  %663 = BUFFER_READU8 %578, %634, tbuffer
  BUFFER_WRITEI8 %578, %634, %663, tbuffer
  %691 = BUFFER_READI16 %578, %27, tbuffer
  BUFFER_WRITEI16 %578, %27, %691, tbuffer
  %719 = BUFFER_READU16 %578, %27, tbuffer
  BUFFER_WRITEI16 %578, %27, %719, tbuffer
  INTERRUPT 112u
  RETURN R0, 0i
bb_4:
  CHECK_SAFE_ENV bb_fallback_7
  CHECK_TAG R5, tnumber, exit(10)
  CHECK_TAG R6, tnumber, exit(10)
  %53 = LOAD_POINTER R1
  %54 = LOAD_DOUBLE R5
  %55 = NUM_TO_INT %54
  CHECK_BUFFER_LEN %53, %55, 0i, 1i, undef, exit(10)
  %57 = LOAD_DOUBLE R6
  %58 = NUM_TO_UINT %57
  BUFFER_WRITEI8 %53, %55, %58, tbuffer
  JUMP bb_8
bb_8:
  %75 = LOAD_DOUBLE R0
  %76 = SUB_NUM %75, 1
  STORE_DOUBLE R5, %76
  STORE_TAG R5, tnumber
  STORE_DOUBLE R8, %76
  STORE_TAG R8, tnumber
  CHECK_SAFE_ENV bb_fallback_11
  %90 = LOAD_POINTER R1
  %92 = NUM_TO_INT %76
  CHECK_BUFFER_LEN %90, %92, 0i, 1i, undef, exit(18)
  %94 = BUFFER_READU8 %90, %92, tbuffer
  %95 = INT_TO_NUM %94
  STORE_DOUBLE R6, %95
  STORE_TAG R6, tnumber
  JUMP bb_12
bb_12:
  CHECK_SAFE_ENV bb_fallback_15
  CHECK_TAG R5, tnumber, exit(24)
  CHECK_TAG R6, tnumber, exit(24)
  %118 = LOAD_POINTER R1
  %119 = LOAD_DOUBLE R5
  %120 = NUM_TO_INT %119
  CHECK_BUFFER_LEN %118, %120, 0i, 1i, undef, exit(24)
  %122 = LOAD_DOUBLE R6
  %123 = NUM_TO_UINT %122
  BUFFER_WRITEI8 %118, %120, %123, tbuffer
  JUMP bb_16
bb_16:
  %140 = LOAD_DOUBLE R0
  %141 = SUB_NUM %140, 1
  STORE_DOUBLE R5, %141
  STORE_TAG R5, tnumber
  STORE_DOUBLE R8, %141
  STORE_TAG R8, tnumber
  CHECK_SAFE_ENV bb_fallback_19
  %155 = LOAD_POINTER R2
  %157 = NUM_TO_INT %141
  CHECK_BUFFER_LEN %155, %157, 0i, 1i, undef, exit(32)
  %159 = BUFFER_READI8 %155, %157, tbuffer
  %160 = INT_TO_NUM %159
  STORE_DOUBLE R6, %160
  STORE_TAG R6, tnumber
  JUMP bb_20
bb_20:
  CHECK_SAFE_ENV bb_fallback_23
  CHECK_TAG R5, tnumber, exit(38)
  CHECK_TAG R6, tnumber, exit(38)
  %183 = LOAD_POINTER R2
  %184 = LOAD_DOUBLE R5
  %185 = NUM_TO_INT %184
  CHECK_BUFFER_LEN %183, %185, 0i, 1i, undef, exit(38)
  %187 = LOAD_DOUBLE R6
  %188 = NUM_TO_UINT %187
  BUFFER_WRITEI8 %183, %185, %188, tbuffer
  JUMP bb_24
bb_24:
  %205 = LOAD_DOUBLE R0
  %206 = SUB_NUM %205, 1
  STORE_DOUBLE R5, %206
  STORE_TAG R5, tnumber
  STORE_DOUBLE R8, %206
  STORE_TAG R8, tnumber
  CHECK_SAFE_ENV bb_fallback_27
  %220 = LOAD_POINTER R2
  %222 = NUM_TO_INT %206
  CHECK_BUFFER_LEN %220, %222, 0i, 1i, undef, exit(46)
  %224 = BUFFER_READU8 %220, %222, tbuffer
  %225 = INT_TO_NUM %224
  STORE_DOUBLE R6, %225
  STORE_TAG R6, tnumber
  JUMP bb_28
bb_28:
  CHECK_SAFE_ENV bb_fallback_31
  CHECK_TAG R5, tnumber, exit(52)
  CHECK_TAG R6, tnumber, exit(52)
  %248 = LOAD_POINTER R2
  %249 = LOAD_DOUBLE R5
  %250 = NUM_TO_INT %249
  CHECK_BUFFER_LEN %248, %250, 0i, 1i, undef, exit(52)
  %252 = LOAD_DOUBLE R6
  %253 = NUM_TO_UINT %252
  BUFFER_WRITEI8 %248, %250, %253, tbuffer
  JUMP bb_32
bb_32:
  %270 = LOAD_DOUBLE R0
  STORE_DOUBLE R5, %270
  STORE_TAG R5, tnumber
  STORE_DOUBLE R8, %270
  STORE_TAG R8, tnumber
  CHECK_SAFE_ENV bb_fallback_35
  %285 = LOAD_POINTER R2
  %287 = NUM_TO_INT %270
  CHECK_BUFFER_LEN %285, %287, 0i, 1i, undef, exit(60)
  %289 = BUFFER_READI8 %285, %287, tbuffer
  %290 = INT_TO_NUM %289
  STORE_DOUBLE R6, %290
  STORE_TAG R6, tnumber
  JUMP bb_36
bb_36:
  CHECK_SAFE_ENV bb_fallback_39
  CHECK_TAG R5, tnumber, exit(66)
  CHECK_TAG R6, tnumber, exit(66)
  %313 = LOAD_POINTER R2
  %314 = LOAD_DOUBLE R5
  %315 = NUM_TO_INT %314
  CHECK_BUFFER_LEN %313, %315, 0i, 1i, undef, exit(66)
  %317 = LOAD_DOUBLE R6
  %318 = NUM_TO_UINT %317
  BUFFER_WRITEI8 %313, %315, %318, tbuffer
  JUMP bb_40
bb_40:
  %335 = LOAD_DOUBLE R0
  STORE_DOUBLE R5, %335
  STORE_TAG R5, tnumber
  STORE_DOUBLE R8, %335
  STORE_TAG R8, tnumber
  CHECK_SAFE_ENV bb_fallback_43
  %350 = LOAD_POINTER R2
  %352 = NUM_TO_INT %335
  CHECK_BUFFER_LEN %350, %352, 0i, 1i, undef, exit(74)
  %354 = BUFFER_READU8 %350, %352, tbuffer
  %355 = INT_TO_NUM %354
  STORE_DOUBLE R6, %355
  STORE_TAG R6, tnumber
  JUMP bb_44
bb_44:
  CHECK_SAFE_ENV bb_fallback_47
  CHECK_TAG R5, tnumber, exit(80)
  CHECK_TAG R6, tnumber, exit(80)
  %378 = LOAD_POINTER R2
  %379 = LOAD_DOUBLE R5
  %380 = NUM_TO_INT %379
  CHECK_BUFFER_LEN %378, %380, 0i, 1i, undef, exit(80)
  %382 = LOAD_DOUBLE R6
  %383 = NUM_TO_UINT %382
  BUFFER_WRITEI8 %378, %380, %383, tbuffer
  JUMP bb_48
bb_48:
  %400 = LOAD_DOUBLE R0
  %401 = SUB_NUM %400, 1
  STORE_DOUBLE R5, %401
  STORE_TAG R5, tnumber
  STORE_DOUBLE R8, %401
  STORE_TAG R8, tnumber
  CHECK_SAFE_ENV bb_fallback_51
  %415 = LOAD_POINTER R2
  %417 = NUM_TO_INT %401
  CHECK_BUFFER_LEN %415, %417, 0i, 2i, undef, exit(88)
  %419 = BUFFER_READI16 %415, %417, tbuffer
  %420 = INT_TO_NUM %419
  STORE_DOUBLE R6, %420
  STORE_TAG R6, tnumber
  JUMP bb_52
bb_52:
  CHECK_SAFE_ENV bb_fallback_55
  CHECK_TAG R5, tnumber, exit(94)
  CHECK_TAG R6, tnumber, exit(94)
  %443 = LOAD_POINTER R2
  %444 = LOAD_DOUBLE R5
  %445 = NUM_TO_INT %444
  CHECK_BUFFER_LEN %443, %445, 0i, 2i, undef, exit(94)
  %447 = LOAD_DOUBLE R6
  %448 = NUM_TO_UINT %447
  BUFFER_WRITEI16 %443, %445, %448, tbuffer
  JUMP bb_56
bb_56:
  %465 = LOAD_DOUBLE R0
  %466 = SUB_NUM %465, 1
  STORE_DOUBLE R5, %466
  STORE_TAG R5, tnumber
  STORE_DOUBLE R8, %466
  STORE_TAG R8, tnumber
  CHECK_SAFE_ENV bb_fallback_59
  %480 = LOAD_POINTER R2
  %482 = NUM_TO_INT %466
  CHECK_BUFFER_LEN %480, %482, 0i, 2i, undef, exit(102)
  %484 = BUFFER_READU16 %480, %482, tbuffer
  %485 = INT_TO_NUM %484
  STORE_DOUBLE R6, %485
  STORE_TAG R6, tnumber
  JUMP bb_60
bb_60:
  CHECK_SAFE_ENV bb_fallback_63
  CHECK_TAG R5, tnumber, exit(108)
  CHECK_TAG R6, tnumber, exit(108)
  %508 = LOAD_POINTER R2
  %509 = LOAD_DOUBLE R5
  %510 = NUM_TO_INT %509
  CHECK_BUFFER_LEN %508, %510, 0i, 2i, undef, exit(108)
  %512 = LOAD_DOUBLE R6
  %513 = NUM_TO_UINT %512
  BUFFER_WRITEI16 %508, %510, %513, tbuffer
  JUMP bb_64
bb_64:
  INTERRUPT 112u
  RETURN R0, 0i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "NumericConversionReplacementCheck")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(buf: buffer, a: number)
    math.ldexp(a, a) -- generate NUM_TO_INT early

    -- range checks cannot make NUM_TO_INT exit to VM at a later location
    return buffer.readi32(buf, a) + buffer.readi32(buf, a + 4)
end
)"),
        R"(
; function foo($arg0, $arg1) line 2
bb_0:
  CHECK_TAG R0, tbuffer, exit(entry)
  CHECK_TAG R1, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  CHECK_SAFE_ENV bb_fallback_3
  %11 = LOAD_DOUBLE R1
  %13 = NUM_TO_INT %11
  JUMP bb_linear_15
bb_linear_15:
  %104 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %104, %13, 0i, 8i, %11, bb_exit_16
   ; exit sync: R2, {%11, %13}
  %108 = BUFFER_READI32 %104, %13, tbuffer
  %109 = INT_TO_NUM %108
  %119 = ADD_INT %13, 4i
  %121 = BUFFER_READI32 %104, %119, tbuffer
  %122 = INT_TO_NUM %121
  %132 = ADD_NUM %109, %122
  STORE_SPLIT_TVALUE R2, tnumber, %132
  INTERRUPT 22u
  RETURN R2, 1i
bb_4:
  CHECK_SAFE_ENV bb_fallback_7
  %37 = LOAD_POINTER R0
  %38 = LOAD_DOUBLE R1
  %39 = NUM_TO_INT %38
  CHECK_BUFFER_LEN %37, %39, 0i, 4i, undef, exit(9)
  %41 = BUFFER_READI32 %37, %39, tbuffer
  %42 = INT_TO_NUM %41
  STORE_DOUBLE R3, %42
  STORE_TAG R3, tnumber
  JUMP bb_8
bb_8:
  %62 = LOAD_DOUBLE R1
  %63 = ADD_NUM %62, 4
  STORE_DOUBLE R6, %63
  STORE_TAG R6, tnumber
  CHECK_SAFE_ENV bb_fallback_11
  %71 = LOAD_POINTER R0
  %73 = NUM_TO_INT %63
  CHECK_BUFFER_LEN %71, %73, 0i, 4i, undef, exit(17)
  %75 = BUFFER_READI32 %71, %73, tbuffer
  %76 = INT_TO_NUM %75
  STORE_DOUBLE R4, %76
  STORE_TAG R4, tnumber
  JUMP bb_12
bb_12:
  CHECK_TAG R3, tnumber, exit(21)
  CHECK_TAG R4, tnumber, exit(21)
  %96 = LOAD_DOUBLE R3
  %98 = ADD_NUM %96, R4
  STORE_DOUBLE R2, %98
  STORE_TAG R2, tnumber
  INTERRUPT 22u
  RETURN R2, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "BufferRelatedIndicesPositiveMultBase")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(buf: buffer, a: number)
    return buffer.readi32(buf, a * 4) + buffer.readi32(buf, (a + 1) * 4) + buffer.readi32(buf, (a + 2) * 4)
end
)"),
        R"(
; function foo($arg0, $arg1) line 2
bb_0:
  CHECK_TAG R0, tbuffer, exit(entry)
  CHECK_TAG R1, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %8 = LOAD_DOUBLE R1
  %9 = MUL_NUM %8, 4
  STORE_DOUBLE R6, %9
  STORE_TAG R6, tnumber
  CHECK_SAFE_ENV bb_fallback_3
  %17 = LOAD_POINTER R0
  %19 = NUM_TO_INT %9
  CHECK_BUFFER_LEN %17, %19, 0i, 12i, %9, exit(3)
  %21 = BUFFER_READI32 %17, %19, tbuffer
  %22 = INT_TO_NUM %21
  JUMP bb_linear_15
bb_linear_15:
  %143 = ADD_INT %19, 4i
  %145 = BUFFER_READI32 %17, %143, tbuffer
  %146 = INT_TO_NUM %145
  %156 = ADD_NUM %22, %146
  %168 = ADD_INT %19, 8i
  %170 = BUFFER_READI32 %17, %168, tbuffer
  %171 = INT_TO_NUM %170
  %180 = ADD_NUM %156, %171
  STORE_DOUBLE R2, %180
  STORE_TAG R2, tnumber
  INTERRUPT 25u
  RETURN R2, 1i
bb_4:
  %40 = LOAD_DOUBLE R1
  %41 = ADD_NUM %40, 1
  STORE_DOUBLE R8, %41
  STORE_TAG R8, tnumber
  %47 = MUL_NUM %41, 4
  STORE_DOUBLE R7, %47
  STORE_TAG R7, tnumber
  CHECK_SAFE_ENV bb_fallback_7
  %55 = LOAD_POINTER R0
  %57 = NUM_TO_INT %47
  CHECK_BUFFER_LEN %55, %57, 0i, 4i, undef, exit(11)
  %59 = BUFFER_READI32 %55, %57, tbuffer
  %60 = INT_TO_NUM %59
  STORE_DOUBLE R5, %60
  STORE_TAG R5, tnumber
  JUMP bb_8
bb_8:
  CHECK_TAG R4, tnumber, exit(15)
  CHECK_TAG R5, tnumber, exit(15)
  %80 = LOAD_DOUBLE R4
  %82 = ADD_NUM %80, R5
  STORE_DOUBLE R3, %82
  STORE_TAG R3, tnumber
  %87 = LOAD_DOUBLE R1
  %88 = ADD_NUM %87, 2
  STORE_DOUBLE R7, %88
  STORE_TAG R7, tnumber
  %94 = MUL_NUM %88, 4
  STORE_DOUBLE R6, %94
  STORE_TAG R6, tnumber
  CHECK_SAFE_ENV bb_fallback_11
  %102 = LOAD_POINTER R0
  %104 = NUM_TO_INT %94
  CHECK_BUFFER_LEN %102, %104, 0i, 4i, undef, exit(20)
  %106 = BUFFER_READI32 %102, %104, tbuffer
  %107 = INT_TO_NUM %106
  STORE_DOUBLE R4, %107
  JUMP bb_12
bb_12:
  CHECK_TAG R3, tnumber, exit(24)
  CHECK_TAG R4, tnumber, exit(24)
  %127 = LOAD_DOUBLE R3
  %129 = ADD_NUM %127, R4
  STORE_DOUBLE R2, %129
  STORE_TAG R2, tnumber
  INTERRUPT 25u
  RETURN R2, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "BufferRelatedIndicesPositiveMultBase2")
{
    // Different index multipliers are not merged
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(buf: buffer, a: number)
    return buffer.readi32(buf, a * 4) + buffer.readi32(buf, (a + 1) * 8)
end
)"),
        R"(
; function foo($arg0, $arg1) line 2
bb_0:
  CHECK_TAG R0, tbuffer, exit(entry)
  CHECK_TAG R1, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %8 = LOAD_DOUBLE R1
  %9 = MUL_NUM %8, 4
  STORE_DOUBLE R5, %9
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_3
  %17 = LOAD_POINTER R0
  %19 = NUM_TO_INT %9
  CHECK_BUFFER_LEN %17, %19, 0i, 4i, undef, exit(3)
  %21 = BUFFER_READI32 %17, %19, tbuffer
  %22 = INT_TO_NUM %21
  STORE_DOUBLE R3, %22
  STORE_TAG R3, tnumber
  JUMP bb_4
bb_4:
  %40 = LOAD_DOUBLE R1
  %41 = ADD_NUM %40, 1
  STORE_DOUBLE R7, %41
  STORE_TAG R7, tnumber
  %47 = MUL_NUM %41, 8
  STORE_DOUBLE R6, %47
  STORE_TAG R6, tnumber
  CHECK_SAFE_ENV bb_fallback_7
  %55 = LOAD_POINTER R0
  %57 = NUM_TO_INT %47
  CHECK_BUFFER_LEN %55, %57, 0i, 4i, undef, exit(11)
  %59 = BUFFER_READI32 %55, %57, tbuffer
  %60 = INT_TO_NUM %59
  STORE_DOUBLE R4, %60
  STORE_TAG R4, tnumber
  JUMP bb_8
bb_8:
  CHECK_TAG R3, tnumber, exit(15)
  CHECK_TAG R4, tnumber, exit(15)
  %80 = LOAD_DOUBLE R3
  %82 = ADD_NUM %80, R4
  STORE_DOUBLE R2, %82
  STORE_TAG R2, tnumber
  INTERRUPT 16u
  RETURN R2, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "BufferRelatedIndicesPositiveMultBaseInt")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(buf: buffer, a: number)
    -- trying to be helpful
    local t1 = bit32.bor(a, 0)
    local t2 = bit32.bor(t1 + 8, 0)
    local t3 = bit32.bor(t1 + 16, 0)
    return buffer.readf64(buf, t1) + buffer.readf64(buf, t2) + buffer.readf64(buf, t3)
end
)"),
        R"(
; function foo($arg0, $arg1) line 2
bb_0:
  CHECK_TAG R0, tbuffer, exit(entry)
  CHECK_TAG R1, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  CHECK_SAFE_ENV bb_fallback_3
  %9 = LOAD_DOUBLE R1
  %10 = NUM_TO_UINT %9
  JUMP bb_linear_27
bb_linear_27:
  %197 = ADD_INT %10, 8i
  %209 = ADD_INT %10, 16i
  %217 = LOAD_POINTER R0
  %219 = TRUNCATE_UINT %10
  CHECK_BUFFER_LEN %217, %219, 0i, 24i, undef, bb_exit_28
   ; exit sync: R5, R4, R3, R2, {%209, %197, %10}
  %221 = BUFFER_READF64 %217, %219, tbuffer
  %232 = BUFFER_READF64 %217, %197, tbuffer
  %242 = ADD_NUM %221, %232
  %252 = BUFFER_READF64 %217, %209, tbuffer
  %261 = ADD_NUM %242, %252
  STORE_SPLIT_TVALUE R5, tnumber, %261
  INTERRUPT 44u
  RETURN R5, 1i
bb_4:
  CHECK_TAG R2, tnumber, exit(7)
  %33 = LOAD_DOUBLE R2
  %34 = ADD_NUM %33, 8
  STORE_DOUBLE R4, %34
  STORE_TAG R4, tnumber
  CHECK_SAFE_ENV bb_fallback_7
  %41 = NUM_TO_UINT %34
  %44 = UINT_TO_NUM %41
  STORE_DOUBLE R3, %44
  STORE_TAG R3, tnumber
  JUMP bb_8
bb_8:
  CHECK_TAG R2, tnumber, exit(14)
  %62 = LOAD_DOUBLE R2
  %63 = ADD_NUM %62, 16
  STORE_DOUBLE R5, %63
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_11
  %70 = NUM_TO_UINT %63
  %73 = UINT_TO_NUM %70
  STORE_DOUBLE R4, %73
  STORE_TAG R4, tnumber
  JUMP bb_12
bb_12:
  CHECK_SAFE_ENV bb_fallback_15
  CHECK_TAG R2, tnumber, exit(23)
  %94 = LOAD_POINTER R0
  %95 = LOAD_DOUBLE R2
  %96 = NUM_TO_INT %95
  CHECK_BUFFER_LEN %94, %96, 0i, 8i, undef, exit(23)
  %98 = BUFFER_READF64 %94, %96, tbuffer
  STORE_DOUBLE R7, %98
  STORE_TAG R7, tnumber
  JUMP bb_16
bb_16:
  CHECK_SAFE_ENV bb_fallback_19
  CHECK_TAG R3, tnumber, exit(30)
  %121 = LOAD_POINTER R0
  %122 = LOAD_DOUBLE R3
  %123 = NUM_TO_INT %122
  CHECK_BUFFER_LEN %121, %123, 0i, 8i, undef, exit(30)
  %125 = BUFFER_READF64 %121, %123, tbuffer
  STORE_DOUBLE R8, %125
  STORE_TAG R8, tnumber
  JUMP bb_20
bb_20:
  CHECK_TAG R7, tnumber, exit(35)
  CHECK_TAG R8, tnumber, exit(35)
  %147 = LOAD_DOUBLE R7
  %149 = ADD_NUM %147, R8
  STORE_DOUBLE R6, %149
  STORE_TAG R6, tnumber
  CHECK_SAFE_ENV bb_fallback_23
  CHECK_TAG R4, tnumber, exit(38)
  %157 = LOAD_POINTER R0
  %158 = LOAD_DOUBLE R4
  %159 = NUM_TO_INT %158
  CHECK_BUFFER_LEN %157, %159, 0i, 8i, undef, exit(38)
  %161 = BUFFER_READF64 %157, %159, tbuffer
  STORE_DOUBLE R7, %161
  JUMP bb_24
bb_24:
  CHECK_TAG R6, tnumber, exit(43)
  CHECK_TAG R7, tnumber, exit(43)
  %183 = LOAD_DOUBLE R6
  %185 = ADD_NUM %183, R7
  STORE_DOUBLE R5, %185
  STORE_TAG R5, tnumber
  INTERRUPT 44u
  RETURN R5, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "BufferRelatedIndicesMixedSizes")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(buf: buffer, a: number)
    return buffer.readi8(buf, a) + buffer.readi8(buf, a + 4) + buffer.readf64(buf, a - 1)
end
)"),
        R"(
; function foo($arg0, $arg1) line 2
bb_0:
  CHECK_TAG R0, tbuffer, exit(entry)
  CHECK_TAG R1, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  CHECK_SAFE_ENV bb_fallback_3
  %11 = LOAD_POINTER R0
  %12 = LOAD_DOUBLE R1
  %13 = NUM_TO_INT %12
  CHECK_BUFFER_LEN %11, %13, -1i, 7i, %12, exit(2)
  %15 = BUFFER_READI8 %11, %13, tbuffer
  %16 = INT_TO_NUM %15
  JUMP bb_linear_15
bb_linear_15:
  %123 = ADD_INT %13, 4i
  %125 = BUFFER_READI8 %11, %123, tbuffer
  %126 = INT_TO_NUM %125
  %136 = ADD_NUM %16, %126
  %145 = ADD_INT %13, -1i
  %147 = BUFFER_READF64 %11, %145, tbuffer
  %156 = ADD_NUM %136, %147
  STORE_DOUBLE R2, %156
  STORE_TAG R2, tnumber
  INTERRUPT 23u
  RETURN R2, 1i
bb_4:
  %36 = LOAD_DOUBLE R1
  %37 = ADD_NUM %36, 4
  STORE_DOUBLE R7, %37
  STORE_TAG R7, tnumber
  CHECK_SAFE_ENV bb_fallback_7
  %45 = LOAD_POINTER R0
  %47 = NUM_TO_INT %37
  CHECK_BUFFER_LEN %45, %47, 0i, 1i, undef, exit(10)
  %49 = BUFFER_READI8 %45, %47, tbuffer
  %50 = INT_TO_NUM %49
  STORE_DOUBLE R5, %50
  STORE_TAG R5, tnumber
  JUMP bb_8
bb_8:
  CHECK_TAG R4, tnumber, exit(14)
  CHECK_TAG R5, tnumber, exit(14)
  %70 = LOAD_DOUBLE R4
  %72 = ADD_NUM %70, R5
  STORE_DOUBLE R3, %72
  STORE_TAG R3, tnumber
  %77 = LOAD_DOUBLE R1
  %78 = SUB_NUM %77, 1
  STORE_DOUBLE R6, %78
  STORE_TAG R6, tnumber
  CHECK_SAFE_ENV bb_fallback_11
  %86 = LOAD_POINTER R0
  %88 = NUM_TO_INT %78
  CHECK_BUFFER_LEN %86, %88, 0i, 8i, undef, exit(18)
  %90 = BUFFER_READF64 %86, %88, tbuffer
  STORE_DOUBLE R4, %90
  JUMP bb_12
bb_12:
  CHECK_TAG R3, tnumber, exit(22)
  CHECK_TAG R4, tnumber, exit(22)
  %110 = LOAD_DOUBLE R3
  %112 = ADD_NUM %110, R4
  STORE_DOUBLE R2, %112
  STORE_TAG R2, tnumber
  INTERRUPT 23u
  RETURN R2, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "BufferRelatedIndicesConstantRange")
{
    ScopedFastFlag luauCodegenConstPropMinOffset{FFlag::LuauCodegenConstPropMinOffset, true};
    ScopedFastFlag luauCodegenPropagateFallbackTags{FFlag::LuauCodegenPropagateFallbackTags, true};

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(buf: buffer, n: number, x, y, z, w)
    n = 0

    buffer.writeu8(buf, n, x + y + z + w)
    buffer.writeu8(buf, n - 4, 0)
end
)",
                   false,
                   1,
                   2,
                   true
               ),
        R"(
; function foo($arg0, $arg1, $arg2, $arg3, $arg4, $arg5) line 2
bb_0:
  CHECK_TAG R0, tbuffer, exit(entry)
  CHECK_TAG R1, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  STORE_DOUBLE R1, 0
  CHECK_TAG R2, tnumber, bb_fallback_3
  CHECK_TAG R3, tnumber, bb_fallback_3
  %12 = LOAD_DOUBLE R2
  %14 = ADD_NUM %12, R3
  STORE_DOUBLE R11, %14
  STORE_TAG R11, tnumber
  JUMP bb_linear_17
bb_linear_17:
  CHECK_TAG R4, tnumber, bb_fallback_5
  %119 = ADD_NUM %14, R4
  STORE_DOUBLE R10, %119
  STORE_TAG R10, tnumber
  CHECK_TAG R5, tnumber, bb_fallback_7
  %129 = ADD_NUM %119, R5
  STORE_DOUBLE R9, %129
  STORE_TAG R9, tnumber
  CHECK_SAFE_ENV bb_fallback_9
  %136 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %136, 0i, 0i, 1i, undef, exit(6)
  %141 = NUM_TO_UINT %129
  BUFFER_WRITEI8 %136, 0i, %141, tbuffer
  STORE_DOUBLE R8, -4
  STORE_TAG R8, tnumber
  STORE_DOUBLE R9, 0
  JUMP exit(15)
bb_4:
  CHECK_TAG R11, tnumber, bb_fallback_5
  CHECK_TAG R4, tnumber, bb_fallback_5
  %25 = LOAD_DOUBLE R11
  %27 = ADD_NUM %25, R4
  STORE_DOUBLE R10, %27
  STORE_TAG R10, tnumber
  JUMP bb_6
bb_6:
  CHECK_TAG R10, tnumber, bb_fallback_7
  CHECK_TAG R5, tnumber, bb_fallback_7
  %38 = LOAD_DOUBLE R10
  %40 = ADD_NUM %38, R5
  STORE_DOUBLE R9, %40
  STORE_TAG R9, tnumber
  JUMP bb_8
bb_8:
  CHECK_SAFE_ENV bb_fallback_9
  CHECK_TAG R9, tnumber, exit(6)
  %54 = LOAD_POINTER R0
  %55 = LOAD_DOUBLE R1
  %56 = NUM_TO_INT %55
  CHECK_BUFFER_LEN %54, %56, 0i, 1i, undef, exit(6)
  %58 = LOAD_DOUBLE R9
  %59 = NUM_TO_UINT %58
  BUFFER_WRITEI8 %54, %56, %59, tbuffer
  JUMP bb_10
bb_10:
  CHECK_TAG R1, tnumber, exit(11)
  %78 = LOAD_DOUBLE R1
  %79 = SUB_NUM %78, 4
  STORE_DOUBLE R8, %79
  STORE_TAG R8, tnumber
  STORE_DOUBLE R9, 0
  STORE_TAG R9, tnumber
  CHECK_SAFE_ENV bb_fallback_13
  %91 = LOAD_POINTER R0
  %93 = NUM_TO_INT %79
  CHECK_BUFFER_LEN %91, %93, 0i, 1i, undef, exit(15)
  BUFFER_WRITEI8 %91, %93, 0i, tbuffer
  JUMP bb_14
bb_14:
  INTERRUPT 19u
  RETURN R0, 0i
)"
    );
}
TEST_CASE_FIXTURE(LoweringFixture, "BufferVmExitSync")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(buf: buffer, a: number, b: number, c: number)
    local x = buffer.readu8(buf, a * b)
    local y = buffer.readu8(buf, a * b + c)
    return x, y
end
)"),
        R"(
; function foo($arg0, $arg1, $arg2, $arg3) line 2
bb_0:
  CHECK_TAG R0, tbuffer, exit(entry)
  CHECK_TAG R1, tnumber, exit(entry)
  CHECK_TAG R2, tnumber, exit(entry)
  CHECK_TAG R3, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %14 = LOAD_DOUBLE R1
  %16 = MUL_NUM %14, R2
  STORE_DOUBLE R6, %16
  STORE_TAG R6, tnumber
  CHECK_SAFE_ENV bb_fallback_3
  %24 = LOAD_POINTER R0
  %26 = NUM_TO_INT %16
  CHECK_BUFFER_LEN %24, %26, 0i, 1i, undef, exit(3)
  %28 = BUFFER_READU8 %24, %26, tbuffer
  %29 = INT_TO_NUM %28
  STORE_DOUBLE R4, %29
  STORE_TAG R4, tnumber
  JUMP bb_4
bb_4:
  %49 = LOAD_DOUBLE R1
  %51 = MUL_NUM %49, R2
  STORE_DOUBLE R8, %51
  STORE_TAG R8, tnumber
  %60 = ADD_NUM %51, R3
  STORE_DOUBLE R7, %60
  STORE_TAG R7, tnumber
  CHECK_SAFE_ENV bb_fallback_7
  %68 = LOAD_POINTER R0
  %70 = NUM_TO_INT %60
  CHECK_BUFFER_LEN %68, %70, 0i, 1i, undef, exit(11)
  %72 = BUFFER_READU8 %68, %70, tbuffer
  %73 = INT_TO_NUM %72
  STORE_DOUBLE R5, %73
  STORE_TAG R5, tnumber
  JUMP bb_8
bb_8:
  INTERRUPT 15u
  RETURN R4, 2i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "BufferVmExitSyncMultiUseSink")
{
    // This test captures that 'a * b' is only needed for the VM exit to display the 'expected' value of 's' if buffer read throws an exception
    // If the compiler output makes this test outdated, it can be removed as we have IR builder tests covering this as well
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(buf: buffer, a: number, b: number, p: number, q: number)
    local s = a * b
    local v1 = buffer.readi32(buf, p)
    local v2 = buffer.readi32(buf, q)
    s = v1 + v2
    return s
end
)"),
        R"(
; function foo($arg0, $arg1, $arg2, $arg3, $arg4) line 2
bb_0:
  CHECK_TAG R0, tbuffer, exit(entry)
  CHECK_TAG R1, tnumber, exit(entry)
  CHECK_TAG R2, tnumber, exit(entry)
  CHECK_TAG R3, tnumber, exit(entry)
  CHECK_TAG R4, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %16 = LOAD_DOUBLE R1
  %18 = MUL_NUM %16, R2
  STORE_DOUBLE R5, %18
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_3
  %26 = LOAD_POINTER R0
  %27 = LOAD_DOUBLE R3
  %28 = NUM_TO_INT %27
  CHECK_BUFFER_LEN %26, %28, 0i, 4i, undef, exit(3)
  %30 = BUFFER_READI32 %26, %28, tbuffer
  %31 = INT_TO_NUM %30
  STORE_DOUBLE R6, %31
  STORE_TAG R6, tnumber
  JUMP bb_4
bb_4:
  CHECK_SAFE_ENV bb_fallback_7
  %54 = LOAD_POINTER R0
  %55 = LOAD_DOUBLE R4
  %56 = NUM_TO_INT %55
  CHECK_BUFFER_LEN %54, %56, 0i, 4i, undef, exit(10)
  %58 = BUFFER_READI32 %54, %56, tbuffer
  %59 = INT_TO_NUM %58
  STORE_DOUBLE R7, %59
  STORE_TAG R7, tnumber
  JUMP bb_8
bb_8:
  CHECK_TAG R6, tnumber, exit(15)
  CHECK_TAG R7, tnumber, exit(15)
  %81 = LOAD_DOUBLE R6
  %83 = ADD_NUM %81, R7
  STORE_DOUBLE R5, %83
  STORE_TAG R5, tnumber
  INTERRUPT 16u
  RETURN R5, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "BufferEffects")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(buf: buffer)
    buffer.writef64(buf, 0, 3.14)
    local u1 = buffer.writeu8(buf, 4, 170)
    local u2 = buffer.writeu8(buf, 5, 187)
    local u3 = buffer.writeu8(buf, 0, 255)
    return buffer.readf64(buf, 0), u1, u2, u3
end
)",
                   false,
                   1,
                   2,
                   true
               ),
        R"(
; function foo($arg0) line 2
bb_0:
  CHECK_TAG R0, tbuffer, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  STORE_DOUBLE R3, 0
  STORE_TAG R3, tnumber
  STORE_DOUBLE R4, 3.1400000000000001
  STORE_TAG R4, tnumber
  CHECK_SAFE_ENV bb_fallback_3
  %15 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %15, 0i, 0i, 8i, undef, exit(4)
  BUFFER_WRITEF64 %15, 0i, 3.1400000000000001, tbuffer
  JUMP bb_linear_23
bb_linear_23:
  STORE_DOUBLE R3, 4
  STORE_DOUBLE R4, 170
  SET_SAVEDPC 12u
  %132 = INVOKE_FASTCALL 67u, R1, R0, R3, R4, 3i, 1i
  CHECK_FASTCALL_RES %132, bb_fallback_7
  STORE_DOUBLE R4, 5
  STORE_TAG R4, tnumber
  STORE_DOUBLE R5, 187
  STORE_TAG R5, tnumber
  SET_SAVEDPC 20u
  %141 = INVOKE_FASTCALL 67u, R2, R0, R4, R5, 3i, 1i
  CHECK_FASTCALL_RES %141, bb_fallback_11
  STORE_DOUBLE R5, 0
  STORE_TAG R5, tnumber
  STORE_DOUBLE R6, 255
  STORE_TAG R6, tnumber
  SET_SAVEDPC 28u
  %150 = INVOKE_FASTCALL 67u, R3, R0, R5, R6, 3i, 1i
  CHECK_FASTCALL_RES %150, bb_fallback_15
  CHECK_BUFFER_LEN %15, 0i, 0i, 8i, undef, exit(34)
  %156 = BUFFER_READF64 %15, 0i, tbuffer
  STORE_DOUBLE R4, %156
  STORE_TAG R4, tnumber
  %160 = LOAD_TVALUE R1
  STORE_TVALUE R5, %160
  %162 = LOAD_TVALUE R2
  STORE_TVALUE R6, %162
  %164 = LOAD_TVALUE R3
  STORE_TVALUE R7, %164
  INTERRUPT 42u
  RETURN R4, 4i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "Bit32NoDoubleTemporariesAdd")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(a: number, b: number)
    local a = bit32.band(bit32.bor(a, 0) + bit32.bor(b, 0), 0xffff)
    local b = bit32.band(bit32.bor(a, 0) + 127, 0xffff)
    local c = bit32.band(254 + bit32.bor(a, 1), 0xffff)
    return a, b, c
end
)"),
        R"(
; function foo($arg0, $arg1) line 2
bb_0:
  CHECK_TAG R0, tnumber, exit(entry)
  CHECK_TAG R1, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  CHECK_SAFE_ENV bb_fallback_3
  %9 = LOAD_DOUBLE R0
  %10 = NUM_TO_UINT %9
  JUMP bb_linear_31
bb_linear_31:
  %199 = LOAD_DOUBLE R1
  %200 = NUM_TO_UINT %199
  %215 = ADD_INT %10, %200
  %216 = BITAND_UINT %215, 65535i
  %217 = UINT_TO_NUM %216
  STORE_DOUBLE R2, %217
  STORE_TAG R2, tnumber
  %237 = ADD_INT %216, 127i
  %238 = BITAND_UINT %237, 65535i
  %239 = UINT_TO_NUM %238
  STORE_SPLIT_TVALUE R3, tnumber, %239
  %248 = BITOR_UINT %216, 1i
  %260 = ADD_INT %248, 254i
  %261 = BITAND_UINT %260, 65535i
  %262 = UINT_TO_NUM %261
  STORE_SPLIT_TVALUE R4, tnumber, %262
  INTERRUPT 49u
  RETURN R2, 3i
bb_4:
  CHECK_SAFE_ENV bb_fallback_7
  %34 = LOAD_DOUBLE R1
  %35 = NUM_TO_UINT %34
  %38 = UINT_TO_NUM %35
  STORE_DOUBLE R5, %38
  STORE_TAG R5, tnumber
  JUMP bb_8
bb_8:
  CHECK_TAG R4, tnumber, exit(14)
  CHECK_TAG R5, tnumber, exit(14)
  %60 = LOAD_DOUBLE R4
  %62 = ADD_NUM %60, R5
  STORE_DOUBLE R3, %62
  STORE_TAG R3, tnumber
  CHECK_SAFE_ENV bb_fallback_11
  %69 = NUM_TO_UINT %62
  %71 = BITAND_UINT %69, 65535i
  %72 = UINT_TO_NUM %71
  STORE_DOUBLE R2, %72
  STORE_TAG R2, tnumber
  JUMP bb_12
bb_12:
  CHECK_SAFE_ENV bb_fallback_15
  CHECK_TAG R2, tnumber, exit(23)
  %91 = LOAD_DOUBLE R2
  %92 = NUM_TO_UINT %91
  %95 = UINT_TO_NUM %92
  STORE_DOUBLE R5, %95
  STORE_TAG R5, tnumber
  JUMP bb_16
bb_16:
  CHECK_TAG R5, tnumber, exit(28)
  %115 = LOAD_DOUBLE R5
  %116 = ADD_NUM %115, 127
  STORE_DOUBLE R4, %116
  STORE_TAG R4, tnumber
  CHECK_SAFE_ENV bb_fallback_19
  %123 = NUM_TO_UINT %116
  %125 = BITAND_UINT %123, 65535i
  %126 = UINT_TO_NUM %125
  STORE_DOUBLE R3, %126
  STORE_TAG R3, tnumber
  JUMP bb_20
bb_20:
  CHECK_SAFE_ENV bb_fallback_23
  CHECK_TAG R2, tnumber, exit(37)
  %145 = LOAD_DOUBLE R2
  %146 = NUM_TO_UINT %145
  %148 = BITOR_UINT %146, 1i
  %149 = UINT_TO_NUM %148
  STORE_DOUBLE R6, %149
  STORE_TAG R6, tnumber
  JUMP bb_24
bb_24:
  CHECK_TAG R6, tnumber, exit(42)
  %169 = LOAD_DOUBLE R6
  %170 = ADD_NUM %169, 254
  STORE_DOUBLE R5, %170
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_27
  %177 = NUM_TO_UINT %170
  %179 = BITAND_UINT %177, 65535i
  %180 = UINT_TO_NUM %179
  STORE_DOUBLE R4, %180
  STORE_TAG R4, tnumber
  JUMP bb_28
bb_28:
  INTERRUPT 49u
  RETURN R2, 3i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "Bit32HasToUseDoubleTemporariesAdd")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(a: number, b: number)
    local a = bit32.band(bit32.bor(a, 0) + 0.75, 0xffff)
    local b = bit32.band(bit32.bor(a, 0) + 1e30, 0xffff)
    local c = bit32.band(1e30 + bit32.bor(a, 1), 0xffff)
    return a, b, c
end
)"),
        R"(
; function foo($arg0, $arg1) line 2
bb_0:
  CHECK_TAG R0, tnumber, exit(entry)
  CHECK_TAG R1, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  CHECK_SAFE_ENV bb_fallback_3
  %9 = LOAD_DOUBLE R0
  %10 = NUM_TO_UINT %9
  %13 = UINT_TO_NUM %10
  JUMP bb_linear_27
bb_linear_27:
  %173 = ADD_NUM %13, 0.75
  %177 = NUM_TO_UINT %173
  %178 = BITAND_UINT %177, 65535i
  %179 = UINT_TO_NUM %178
  STORE_DOUBLE R2, %179
  STORE_TAG R2, tnumber
  %195 = ADD_NUM %179, 1e+30
  %199 = NUM_TO_UINT %195
  %200 = BITAND_UINT %199, 65535i
  %201 = UINT_TO_NUM %200
  STORE_SPLIT_TVALUE R3, tnumber, %201
  %210 = BITOR_UINT %178, 1i
  %211 = UINT_TO_NUM %210
  %218 = ADD_NUM %211, 1e+30
  %222 = NUM_TO_UINT %218
  %223 = BITAND_UINT %222, 65535i
  %224 = UINT_TO_NUM %223
  STORE_SPLIT_TVALUE R4, tnumber, %224
  INTERRUPT 42u
  RETURN R2, 3i
bb_4:
  CHECK_TAG R4, tnumber, exit(7)
  %33 = LOAD_DOUBLE R4
  %34 = ADD_NUM %33, 0.75
  STORE_DOUBLE R3, %34
  STORE_TAG R3, tnumber
  CHECK_SAFE_ENV bb_fallback_7
  %41 = NUM_TO_UINT %34
  %43 = BITAND_UINT %41, 65535i
  %44 = UINT_TO_NUM %43
  STORE_DOUBLE R2, %44
  STORE_TAG R2, tnumber
  JUMP bb_8
bb_8:
  CHECK_SAFE_ENV bb_fallback_11
  CHECK_TAG R2, tnumber, exit(16)
  %63 = LOAD_DOUBLE R2
  %64 = NUM_TO_UINT %63
  %67 = UINT_TO_NUM %64
  STORE_DOUBLE R5, %67
  STORE_TAG R5, tnumber
  JUMP bb_12
bb_12:
  CHECK_TAG R5, tnumber, exit(21)
  %87 = LOAD_DOUBLE R5
  %88 = ADD_NUM %87, 1e+30
  STORE_DOUBLE R4, %88
  STORE_TAG R4, tnumber
  CHECK_SAFE_ENV bb_fallback_15
  %95 = NUM_TO_UINT %88
  %97 = BITAND_UINT %95, 65535i
  %98 = UINT_TO_NUM %97
  STORE_DOUBLE R3, %98
  STORE_TAG R3, tnumber
  JUMP bb_16
bb_16:
  CHECK_SAFE_ENV bb_fallback_19
  CHECK_TAG R2, tnumber, exit(30)
  %117 = LOAD_DOUBLE R2
  %118 = NUM_TO_UINT %117
  %120 = BITOR_UINT %118, 1i
  %121 = UINT_TO_NUM %120
  STORE_DOUBLE R6, %121
  STORE_TAG R6, tnumber
  JUMP bb_20
bb_20:
  CHECK_TAG R6, tnumber, exit(35)
  %141 = LOAD_DOUBLE R6
  %142 = ADD_NUM %141, 1e+30
  STORE_DOUBLE R5, %142
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_23
  %149 = NUM_TO_UINT %142
  %151 = BITAND_UINT %149, 65535i
  %152 = UINT_TO_NUM %151
  STORE_DOUBLE R4, %152
  STORE_TAG R4, tnumber
  JUMP bb_24
bb_24:
  INTERRUPT 42u
  RETURN R2, 3i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "Bit32NoDoubleTemporariesSub")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(a: number, b: number)
    local a = bit32.band(bit32.bor(a, 0) - bit32.bor(b, 0), 0xffff)
    local b = bit32.band(bit32.bor(a, 0) - 127, 0xffff)
    local c = bit32.band(254 - bit32.bor(a, 1), 0xffff)
    return a, b, c
end
)"),
        R"(
; function foo($arg0, $arg1) line 2
bb_0:
  CHECK_TAG R0, tnumber, exit(entry)
  CHECK_TAG R1, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  CHECK_SAFE_ENV bb_fallback_3
  %9 = LOAD_DOUBLE R0
  %10 = NUM_TO_UINT %9
  JUMP bb_linear_31
bb_linear_31:
  %199 = LOAD_DOUBLE R1
  %200 = NUM_TO_UINT %199
  %215 = SUB_INT %10, %200
  %216 = BITAND_UINT %215, 65535i
  %217 = UINT_TO_NUM %216
  STORE_DOUBLE R2, %217
  STORE_TAG R2, tnumber
  %237 = SUB_INT %216, 127i
  %238 = BITAND_UINT %237, 65535i
  %239 = UINT_TO_NUM %238
  STORE_SPLIT_TVALUE R3, tnumber, %239
  %248 = BITOR_UINT %216, 1i
  %260 = SUB_INT 254i, %248
  %261 = BITAND_UINT %260, 65535i
  %262 = UINT_TO_NUM %261
  STORE_SPLIT_TVALUE R4, tnumber, %262
  INTERRUPT 49u
  RETURN R2, 3i
bb_4:
  CHECK_SAFE_ENV bb_fallback_7
  %34 = LOAD_DOUBLE R1
  %35 = NUM_TO_UINT %34
  %38 = UINT_TO_NUM %35
  STORE_DOUBLE R5, %38
  STORE_TAG R5, tnumber
  JUMP bb_8
bb_8:
  CHECK_TAG R4, tnumber, exit(14)
  CHECK_TAG R5, tnumber, exit(14)
  %60 = LOAD_DOUBLE R4
  %62 = SUB_NUM %60, R5
  STORE_DOUBLE R3, %62
  STORE_TAG R3, tnumber
  CHECK_SAFE_ENV bb_fallback_11
  %69 = NUM_TO_UINT %62
  %71 = BITAND_UINT %69, 65535i
  %72 = UINT_TO_NUM %71
  STORE_DOUBLE R2, %72
  STORE_TAG R2, tnumber
  JUMP bb_12
bb_12:
  CHECK_SAFE_ENV bb_fallback_15
  CHECK_TAG R2, tnumber, exit(23)
  %91 = LOAD_DOUBLE R2
  %92 = NUM_TO_UINT %91
  %95 = UINT_TO_NUM %92
  STORE_DOUBLE R5, %95
  STORE_TAG R5, tnumber
  JUMP bb_16
bb_16:
  CHECK_TAG R5, tnumber, exit(28)
  %115 = LOAD_DOUBLE R5
  %116 = SUB_NUM %115, 127
  STORE_DOUBLE R4, %116
  STORE_TAG R4, tnumber
  CHECK_SAFE_ENV bb_fallback_19
  %123 = NUM_TO_UINT %116
  %125 = BITAND_UINT %123, 65535i
  %126 = UINT_TO_NUM %125
  STORE_DOUBLE R3, %126
  STORE_TAG R3, tnumber
  JUMP bb_20
bb_20:
  CHECK_SAFE_ENV bb_fallback_23
  CHECK_TAG R2, tnumber, exit(37)
  %145 = LOAD_DOUBLE R2
  %146 = NUM_TO_UINT %145
  %148 = BITOR_UINT %146, 1i
  %149 = UINT_TO_NUM %148
  STORE_DOUBLE R6, %149
  STORE_TAG R6, tnumber
  JUMP bb_24
bb_24:
  CHECK_TAG R6, tnumber, exit(42)
  %170 = SUB_NUM 254, R6
  STORE_DOUBLE R5, %170
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_27
  %177 = NUM_TO_UINT %170
  %179 = BITAND_UINT %177, 65535i
  %180 = UINT_TO_NUM %179
  STORE_DOUBLE R4, %180
  STORE_TAG R4, tnumber
  JUMP bb_28
bb_28:
  INTERRUPT 49u
  RETURN R2, 3i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "Bit32HasToUseDoubleTemporariesSub")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(a: number, b: number)
    local a = bit32.band(bit32.bor(a, 0) - 0.75, 0xffff)
    local b = bit32.band(bit32.bor(a, 0) - 1e30, 0xffff)
    local c = bit32.band(1e30 - bit32.bor(a, 1), 0xffff)
    return a, b, c
end
)"),
        R"(
; function foo($arg0, $arg1) line 2
bb_0:
  CHECK_TAG R0, tnumber, exit(entry)
  CHECK_TAG R1, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  CHECK_SAFE_ENV bb_fallback_3
  %9 = LOAD_DOUBLE R0
  %10 = NUM_TO_UINT %9
  %13 = UINT_TO_NUM %10
  JUMP bb_linear_27
bb_linear_27:
  %173 = SUB_NUM %13, 0.75
  %177 = NUM_TO_UINT %173
  %178 = BITAND_UINT %177, 65535i
  %179 = UINT_TO_NUM %178
  STORE_DOUBLE R2, %179
  STORE_TAG R2, tnumber
  %195 = SUB_NUM %179, 1e+30
  %199 = NUM_TO_UINT %195
  %200 = BITAND_UINT %199, 65535i
  %201 = UINT_TO_NUM %200
  STORE_SPLIT_TVALUE R3, tnumber, %201
  %210 = BITOR_UINT %178, 1i
  %211 = UINT_TO_NUM %210
  %218 = SUB_NUM 1e+30, %211
  %222 = NUM_TO_UINT %218
  %223 = BITAND_UINT %222, 65535i
  %224 = UINT_TO_NUM %223
  STORE_SPLIT_TVALUE R4, tnumber, %224
  INTERRUPT 42u
  RETURN R2, 3i
bb_4:
  CHECK_TAG R4, tnumber, exit(7)
  %33 = LOAD_DOUBLE R4
  %34 = SUB_NUM %33, 0.75
  STORE_DOUBLE R3, %34
  STORE_TAG R3, tnumber
  CHECK_SAFE_ENV bb_fallback_7
  %41 = NUM_TO_UINT %34
  %43 = BITAND_UINT %41, 65535i
  %44 = UINT_TO_NUM %43
  STORE_DOUBLE R2, %44
  STORE_TAG R2, tnumber
  JUMP bb_8
bb_8:
  CHECK_SAFE_ENV bb_fallback_11
  CHECK_TAG R2, tnumber, exit(16)
  %63 = LOAD_DOUBLE R2
  %64 = NUM_TO_UINT %63
  %67 = UINT_TO_NUM %64
  STORE_DOUBLE R5, %67
  STORE_TAG R5, tnumber
  JUMP bb_12
bb_12:
  CHECK_TAG R5, tnumber, exit(21)
  %87 = LOAD_DOUBLE R5
  %88 = SUB_NUM %87, 1e+30
  STORE_DOUBLE R4, %88
  STORE_TAG R4, tnumber
  CHECK_SAFE_ENV bb_fallback_15
  %95 = NUM_TO_UINT %88
  %97 = BITAND_UINT %95, 65535i
  %98 = UINT_TO_NUM %97
  STORE_DOUBLE R3, %98
  STORE_TAG R3, tnumber
  JUMP bb_16
bb_16:
  CHECK_SAFE_ENV bb_fallback_19
  CHECK_TAG R2, tnumber, exit(30)
  %117 = LOAD_DOUBLE R2
  %118 = NUM_TO_UINT %117
  %120 = BITOR_UINT %118, 1i
  %121 = UINT_TO_NUM %120
  STORE_DOUBLE R6, %121
  STORE_TAG R6, tnumber
  JUMP bb_20
bb_20:
  CHECK_TAG R6, tnumber, exit(35)
  %142 = SUB_NUM 1e+30, R6
  STORE_DOUBLE R5, %142
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_23
  %149 = NUM_TO_UINT %142
  %151 = BITAND_UINT %149, 65535i
  %152 = UINT_TO_NUM %151
  STORE_DOUBLE R4, %152
  STORE_TAG R4, tnumber
  JUMP bb_24
bb_24:
  INTERRUPT 42u
  RETURN R2, 3i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "OptionalOr")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(a, b)
    a = a or 0
    b = b or 0
    return a + b
end
-- when a function like 'foo' is inlined, those 'default values' collapse
local function bar()
    return foo(3, 4)
end
)"),
        R"(
; function foo($arg0, $arg1) line 2
bb_bytecode_0:
  %0 = LOAD_TVALUE R0
  %1 = LOAD_TVALUE K0 (0)
  %2 = SELECT_IF_TRUTHY %0, %0, %1
  STORE_TVALUE R0, %2
  %4 = LOAD_TVALUE R1
  %5 = LOAD_TVALUE K0 (0)
  %6 = SELECT_IF_TRUTHY %4, %4, %5
  STORE_TVALUE R1, %6
  CHECK_TAG R0, tnumber, bb_fallback_1
  CHECK_TAG R1, tnumber, bb_fallback_1
  %12 = LOAD_DOUBLE R0
  %14 = ADD_NUM %12, R1
  STORE_DOUBLE R2, %14
  STORE_TAG R2, tnumber
  JUMP bb_2
bb_2:
  INTERRUPT 3u
  RETURN R2, 1i
; function bar() line 8
bb_bytecode_0:
  STORE_DOUBLE R0, 7
  STORE_TAG R0, tnumber
  INTERRUPT 5u
  RETURN R0, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "LinearAndOr")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(a, b)
    return a and b, a or b
end
local function bar()
    local a, b = foo(3, 4)
    return a, b
end
)"),
        R"(
; function foo($arg0, $arg1) line 2
bb_bytecode_0:
  %0 = LOAD_TVALUE R0
  %1 = LOAD_TVALUE R1
  %2 = SELECT_IF_TRUTHY %0, %1, %0
  STORE_TVALUE R2, %2
  %6 = SELECT_IF_TRUTHY %0, %0, %1
  STORE_TVALUE R3, %6
  INTERRUPT 2u
  RETURN R2, 2i
; function bar() line 5
bb_bytecode_0:
  STORE_DOUBLE R0, 4
  STORE_TAG R0, tnumber
  STORE_DOUBLE R1, 3
  STORE_TAG R1, tnumber
  INTERRUPT 2u
  RETURN R0, 2i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "OldStyleConditional")
{
    // TODO: opportunity - this can be done in two SELECT_IF_TRUTHY, but we cannot match such complex sequences right now
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(a: boolean, b: number, c: number)
    local x = a and b or c
    return x + 1
end
)"),
        R"(
; function foo($arg0, $arg1, $arg2) line 2
bb_0:
  CHECK_TAG R0, tboolean, exit(entry)
  CHECK_TAG R1, tnumber, exit(entry)
  CHECK_TAG R2, tnumber, exit(entry)
  JUMP bb_4
bb_4:
  JUMP bb_bytecode_1
bb_bytecode_1:
  JUMP_IF_FALSY R0, bb_bytecode_2, bb_5
bb_5:
  %9 = LOAD_TVALUE R1, 0i, tnumber
  STORE_TVALUE R3, %9
  JUMP bb_bytecode_3
bb_bytecode_2:
  %12 = LOAD_TVALUE R2, 0i, tnumber
  STORE_TVALUE R3, %12
  JUMP bb_bytecode_3
bb_bytecode_3:
  CHECK_TAG R3, tnumber, exit(4)
  %17 = LOAD_DOUBLE R3
  %18 = ADD_NUM %17, 1
  STORE_DOUBLE R4, %18
  STORE_TAG R4, tnumber
  INTERRUPT 5u
  RETURN R4, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "NewStyleConditional")
{
    // TODO: opportunity - this can be done in one SELECT_IF_TRUTHY, but this is also hard to detect in current system
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function foo(a: boolean, b: number, c: number)
    local x = if a then b else c
    return x + 1
end
)"),
        R"(
; function foo($arg0, $arg1, $arg2) line 2
bb_0:
  CHECK_TAG R0, tboolean, exit(entry)
  CHECK_TAG R1, tnumber, exit(entry)
  CHECK_TAG R2, tnumber, exit(entry)
  JUMP bb_4
bb_4:
  JUMP bb_bytecode_1
bb_bytecode_1:
  JUMP_IF_FALSY R0, bb_bytecode_2, bb_5
bb_5:
  %9 = LOAD_TVALUE R1, 0i, tnumber
  STORE_TVALUE R3, %9
  JUMP bb_bytecode_3
bb_bytecode_2:
  %12 = LOAD_TVALUE R2, 0i, tnumber
  STORE_TVALUE R3, %12
  JUMP bb_bytecode_3
bb_bytecode_3:
  CHECK_TAG R3, tnumber, exit(4)
  %17 = LOAD_DOUBLE R3
  %18 = ADD_NUM %17, 1
  STORE_DOUBLE R4, %18
  STORE_TAG R4, tnumber
  INTERRUPT 5u
  RETURN R4, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "RecursiveRemoval1")
{
    // Check that this compiles with no assertions
    CHECK(
        getCodegenAssembly(R"(
repeat
local _ = # {} < # {} < _ < _ < _ ^ _ ^ ""
until ""
)")
            .size() > 0
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "RecursiveRemoval2")
{
    // Check that this compiles with no assertions
    CHECK(
        getCodegenAssembly(R"(
local _

for l0={[1]=(_),},_ do
    _ += _
    for l0={[1]=(_),},_ do
        break
    end
end
)")
            .size() > 0
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "RecursiveRemoval3")
{
    // Check that this compiles with no assertions
    CHECK(
        getCodegenAssembly(R"(
while {_,} do
    local _
    repeat
        _ = "x",_ or {}
    until "x"
end
return l0
)")
            .size() > 0
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "RecursiveRemoval4")
{
    // Check that this compiles with no assertions
    CHECK(
        getCodegenAssembly(R"(
local _ = 5633,5633
while "" do
    _ += bit32.replace(_,function() end,0)
    for l0=_,{_,} do
        do
            _ += bit32.replace(_,nil,0)
            local _
        end
    end
end
)")
            .size() > 0
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "FuzzTagsAcrossChains")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function f(...)
    if bit32.btest(538976288,4,4,4,262144) then
    elseif bit32.btest(538976288,4,_,4,67108864) then
    end
end
)",
                   false,
                   1,
                   1
               ),
        R"(
; function f() line 2
bb_bytecode_0:
  FALLBACK_PREPVARARGS 0u, 0i
  STORE_DOUBLE R1, 538976288
  STORE_TAG R1, tnumber
  STORE_DOUBLE R2, 4
  STORE_TAG R2, tnumber
  STORE_DOUBLE R3, 4
  STORE_TAG R3, tnumber
  STORE_DOUBLE R4, 4
  STORE_TAG R4, tnumber
  STORE_DOUBLE R5, 262144
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_3
  STORE_INT R0, 0i
  STORE_TAG R0, tboolean
  JUMP bb_4
bb_4:
  JUMP_IF_FALSY R0, bb_bytecode_1, bb_7
bb_7:
  INTERRUPT 11u
  RETURN R0, 0i
bb_bytecode_1:
  STORE_DOUBLE R1, 538976288
  STORE_TAG R1, tnumber
  STORE_DOUBLE R2, 4
  STORE_TAG R2, tnumber
  CHECK_SAFE_ENV bb_fallback_8
  GET_CACHED_IMPORT R3, K6 (nil), 1078984704u ('_'), 15u
  JUMP bb_9
bb_9:
  STORE_DOUBLE R4, 4
  STORE_TAG R4, tnumber
  STORE_DOUBLE R5, 67108864
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_10
  CHECK_TAG R1, tnumber, exit(19)
  CHECK_TAG R2, tnumber, exit(19)
  CHECK_TAG R3, tnumber, exit(19)
  %77 = LOAD_DOUBLE R1
  %78 = NUM_TO_UINT %77
  %79 = LOAD_DOUBLE R2
  %80 = NUM_TO_UINT %79
  %81 = BITAND_UINT %78, %80
  %82 = LOAD_DOUBLE R3
  %83 = NUM_TO_UINT %82
  %84 = BITAND_UINT %81, %83
  %87 = BITAND_UINT %84, 4i
  %90 = BITAND_UINT %87, 67108864i
  %91 = CMP_INT %90, 0i, not_eq
  STORE_INT R0, %91
  STORE_TAG R0, tboolean
  JUMP bb_11
bb_11:
  JUMP_IF_FALSY R0, bb_bytecode_2, bb_bytecode_2
bb_bytecode_2:
  INTERRUPT 23u
  RETURN R0, 0i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "FuzzTest1")
{
    // Check that this compiles with no assertions
    CHECK(
        getCodegenAssembly(R"(
local _ = 5633,5633
while _ do
    _ ^= _
    for l0=_,_,{[# {}]=_,} do
        repeat
        until _
    end
end
)")
            .size() > 0
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "FuzzTest2")
{
    // Check that this compiles with no assertions
    CHECK(
        getCodegenAssembly(R"(
local _
while true ~= _ do
    _ = nil
end
_ = _,{},16711935 ~= _,{["" ~= _]=16711935,},_ ~= _
while {} ~= _ do
    _ = nil
end
)")
            .size() > 0
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "FuzzTest3")
{
    // Check that this compiles with no assertions
    CHECK(
        getCodegenAssembly(R"(
local function f(...)
    local _ = ``,_
    _ ..= _(_()(_(_ and (...),_ .. _),_(_)),_(_(_(_(_ .. _,- _),_(_)),_()(_)),_(_,_._)))
    _(_)
end
)")
            .size() > 0
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "FuzzTest4")
{
    // Check that this compiles with no assertions
    CHECK(
        getCodegenAssembly(R"(
local _ = math.exp,_(),_
local _ = math._,_(_(_),_(_ and _),_(_(_),_,_,_()),`{nil}`),_
for l41=_,_ do
end
l0 -= _(0,_(# _,_(_),_(_(_),_(_),_,_()),`{nil}`))
)")
            .size() > 0
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "FuzzTest5")
{
    ensureVectorSize3();

    // Check that this compiles with no assertions
    CHECK(
        getCodegenAssembly(R"(
local _ = 32768
while "" do
    n0 ..= 0
    for l0=`{-2013233152}`,65535 do
        local l0 = vector.create(`{-2013233152}`,-2147450880,_,_)
        _ = _,_ // 0,_ // _
    end
end
)")
            .size() > 0
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "FuzzTest6")
{
    // Check that this compiles with no assertions
    CHECK(
        getCodegenAssembly(R"(
local l0:any = _(393216),# 0,n0
while vector.sign(_ and true) do
_ ..= nil
do end
for l0=_,_,vector.sign(_ + - _) do
for l0=_,_,vector.sign() do
do end
end
end
end
)")
            .size() > 0
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "FuzzTest7")
{
    ensureVectorSize3();

    // Check that this compiles with no assertions
    CHECK(
        getCodegenAssembly(R"(
for l0=0,32768 do end

local _ = vector.create(14941,14941,14848)

local function l0() end

_,l0,_,_,_G,_ = vector.clamp(_,_ / _,_),- - _
)")
            .size() > 0
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "FuzzTest8")
{
    // Check that this compiles with no assertions
    CHECK(
        getCodegenAssembly(R"(
local function f(...)
    local _ = bit32.arshift
    local _ = (_),_(_(# # true,0),30),_(l158(true,_),0),_(_(l9,8258560),_)(_),_ + _,_
end
)")
            .size() > 0
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "FuzzTest9")
{
    // Check that this compiles with no assertions
    CHECK(
        getCodegenAssembly(R"(
local function f(...)
    local _ = bit32.lshift
    local _ = nil,bit32(l0(_(_(8200202,0,_),nil),_),_),_(_,1752395619),{_=_(_,0),},_(_(_(8200202,0,_),0,""),0),_[_]
end
)")
            .size() > 0
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "FuzzTest10")
{
    // Check that this compiles with no assertions
    CHECK(
        getCodegenAssembly(R"(
local function f(...)
    local b, t = ...
    t[buffer.readi8(b, -52436992 * -52436992)] /= buffer.readi8(b, -52436992 * 52436991)
end
)")
            .size() > 0
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "FuzzTest11")
{
    // Check that this compiles with no assertions
    CHECK(
        getCodegenAssembly(R"(
local function f(...)
    local _ = 1024,l0[_],...
    function _(l1, l118, l32, ...)
        for l0,l0,l0 in nil,__index,_ do
        end
        _ = _,vector.min((_),nil),_,nil
        _ = _ + _[l0]
        n0 = nil
    end
    _(_,_,_,_)
end
)")
            .size() > 0
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "FuzzTest12")
{
    // Check that this compiles with no assertions
    CHECK(
        getCodegenAssembly(R"(
local function f(...)
    if buffer.readf64(_, bit32.bxor(0,_,0), function() _ += _ end) then
    elseif ... then
    end
end
)")
            .size() > 0
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "FuzzTest13")
{
    // Check that this compiles with no assertions
    CHECK(
        getCodegenAssembly(R"(
local function f(...)
    local l0 = require(module0)
    buffer.writeu8(l0,1697972224 * 4,function(l0,...)end)
    buffer.writef32(l0,1697972224 * 4,function(l0,...)end)
end
)")
            .size() > 0
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "FuzzTest14")
{
    // Check that this compiles with no assertions
    CHECK(
        getCodegenAssembly(R"(
local function f(...)
    local _ = true
    if tanh then
        l242,_,_,_._ = _,tanh,_,_
        _(...)
        _ = {}
    elseif _ then
        l242,_,_,_._ = _,{_=_,_=_,},_
        _(...)
        _ = {}
    elseif _ then
    end
end
)")
            .size() > 0
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "FuzzTest15")
{
    // Check that this compiles with no assertions
    CHECK(
        getCodegenAssembly(R"(
repeat
for _ in {next=_,},_ do
end
_ = _,_
do end
for l32 in _,{} do
end
until _()
repeat
for _ in next,{_=_,},l0(),_ do
end
_,n0 = l0[_],_,131072 ^ _
for l32 in next,{} do
end
for l32 in next,{sort=_,} do
end
until l0()
)")
            .size() > 0
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "FuzzTest16")
{
    // Check that this compiles with no assertions
    CHECK(
        getCodegenAssembly(R"(
local function f(...)
    local _
    table.insert(_,insert)
    repeat
    table.insert(_,insert)
    local l0 = "",{_=_,_=_,n0=_,n0=_,n0=_,n1=_,_=_,n0=_,}
    _ = nil
    until ...
end
)")
            .size() > 0
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "FuzzTest17")
{
    // Check that this compiles with no assertions
    CHECK(
        getCodegenAssembly(R"(
local function f(...)
    local _ = vector.sign,l0
    _({_,},_,_,_,true,_,_({(if _ then _ else n0._),}),_)
    _(true,vector,_,nil,true,_(- _,l0),n0.sign)
end
)")
            .size() > 0
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "FuzzTest18")
{
    assemblyOptions.compilationOptions.flags = Luau::CodeGen::CodeGen_ColdFunctions;
    compilationOptions.typeInfoLevel = 0;

    // Check that this compiles with no assertions
    CHECK(
        getCodegenAssembly(
            R"(
_[_](_)
local _ = 538976256,_()()
do end
_ = 28672,false,_ ~= _ - _ - _ / _ >= _ - _ - _ / _ - _ - _ - "" - _ - _ - _,not _ - "",not _ - _ - _,_
)",
            false,
            1,
            1
        )
            .size() > 0
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "FuzzTest19")
{
    ensureVectorSize3();

    assemblyOptions.compilationOptions.flags = Luau::CodeGen::CodeGen_ColdFunctions;
    compilationOptions.typeInfoLevel = 0;

    // Check that this compiles with no assertions
    CHECK(
        getCodegenAssembly(
            R"(
local _ = tonumber(159)
_ += _
while 128 do
_,_ = vector.create(_,2304)
do end
while {_,[rawequal(_,_)]=l115,} do
end
end
while {1048576,[rawequal(_,_,_,_ + 128)]=l255,} do
_ = -5
end
_ += _
l0,_ = false,_
do end
)",
            false,
            1,
            1
        )
            .size() > 0
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "FuzzTest20")
{
    ensureVectorSize3();

    // Check that this compiles with no assertions
    CHECK(
        getCodegenAssembly(R"(
local function f(...)
    vector.sign(vector.create(3080192,vector.dot(_,_)))
    vector.sign(vector.create(3080192,vector.dot(_,_)))
end
)")
            .size() > 0
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "FuzzTest21")
{
    // Check that this compiles with no assertions
    CHECK(
        getCodegenAssembly(R"(
local function f(...)
    local _ = (_)._,math.abs(...)._,_._
    local _ = `{string.byte("",0,_)}`,math.abs(...,...).n8,_._
end
)")
            .size() > 0
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "FuzzTest22")
{
    // Check that this compiles with no assertions
    CHECK(
        getCodegenAssembly(
            R"(
local function f(...)
    local _ = _
    for l0=-1,22 do
        for l0=512,187 do
            for l8=16,0 do
                repeat
                until _
                l0 ^= _
                buffer.readi16(_,_)
            end
            integer.min(_.tanh)
            integer.min(_.tanh)
        end
    end
end
)",
            false,
            1,
            1
        )
            .size() > 0
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "FuzzTest23")
{
    CHECK(
        getCodegenAssembly(
            R"(
local _ = ...
for l0=_._,_,... do
repeat
until nil
end
)"
        )
            .size() > 0
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "FuzzTest24")
{
    // Check that this compiles with no assertions
    CHECK(
        getCodegenAssembly(
            R"(
local _ = function(l1,l1)
    local _
    n0,_,_,l0,_._,_[""] = _ == _,``,_,_,_
    _ ""
end
_ ""
while _ {} do end
_ ""
_ {_ == _,}
)"
        )
            .size() > 0
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "FuzzTest25")
{
    CHECK(
        getCodegenAssembly(
            R"(
local _ = ...
local _ = function(l0,l4,l0: ()->())
    local _ = l0,_.n249 + l0
    n0,_,l0 = _,_,{},n0,_
    n0 *= _
    while true do
    end
end
_()
)"
        )
            .size() > 0
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "FuzzTest26")
{
    ScopedFastFlag luauIntegerFastcalls{FFlag::LuauIntegerFastcalls, true};
    ScopedFastFlag LuauCodegenInteger3{FFlag::LuauCodegenInteger3, true};
    ScopedFastFlag luauIntegerType{FFlag::LuauIntegerType2, true};

    CHECK(
        getCodegenAssembly(
            R"(
local function foo(...)
    local _ = ...
    buffer.readu32(_,_,_,nil,integer.max(0i,_,_,_,_,_,_,_,_,_,_,_,_,_,nil,- _,_,_),_)
end
)"
        )
            .size() > 0
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "FuzzTest27")
{
    assemblyOptions.compilationOptions.flags = Luau::CodeGen::CodeGen_ColdFunctions;

    CHECK(
        getCodegenAssembly(
            R"(
for _ in 1,10 do
    _ /= _
    while math.acos(...,math.modf,{[_]=_,[169877609]=math.sinh,["z"]=169877609,},_) do
        _()
    end
end
)"
        )
            .size() > 0
    );

    CHECK(
        getCodegenAssembly(
            R"(
local _ = ...
bit32.replace(_ + _ + _ + _,_,_,_);
bit32.replace(0,bit32.replace(_,_,_,_),28257,_);
(0)(28257,bit32.replace(_,bit32.replace((_),_ + _,(_),_,_),_,_ + _),_);
)"
        )
            .size() > 0
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "FuzzTest28")
{
    assemblyOptions.compilationOptions.flags = Luau::CodeGen::CodeGen_ColdFunctions;

    CHECK(
        getCodegenAssembly(
            R"(
local _ = vector.cross(pcall(_),nil,- _)
do
    _ = _[_] + ...,- - - - - _ + ...,_
    _ = - - - - - _ + ...
    return _
end
)"
        )
            .size() > 0
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "UpvalueAccessLoadStore1")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local m = 1

local function foo(a: number, b: number)
    return m * a + m * b
end

function setm(x) m = x end
)"),
        R"(
; function foo($arg0, $arg1) line 4
bb_0:
  CHECK_TAG R0, tnumber, exit(entry)
  CHECK_TAG R1, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %6 = GET_UPVALUE U0
  STORE_TVALUE R4, %6
  CHECK_TAG R4, tnumber, exit(1)
  %12 = LOAD_DOUBLE R4
  %14 = MUL_NUM %12, R0
  %25 = MUL_NUM %12, R1
  %34 = ADD_NUM %14, %25
  STORE_DOUBLE R2, %34
  STORE_TAG R2, tnumber
  INTERRUPT 5u
  RETURN R2, 1i
; function setm($arg0) line 8
bb_bytecode_0:
  %0 = LOAD_TVALUE R0
  SET_UPVALUE U0, %0, undef
  INTERRUPT 1u
  RETURN R0, 0i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "UpvalueAccessLoadStore2")
{
    // TODO: opportunity - if the value was just stored to VM register in parts, we can use those parts to store upvalue
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local m

local function foo(a: number, b: number)
    m = a - b
    m = m * a + m * b
    return m + a
end
)"),
        R"(
; function foo($arg0, $arg1) line 4
bb_0:
  CHECK_TAG R0, tnumber, exit(entry)
  CHECK_TAG R1, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %10 = LOAD_DOUBLE R0
  %11 = LOAD_DOUBLE R1
  %12 = SUB_NUM %10, %11
  STORE_DOUBLE R2, %12
  STORE_TAG R2, tnumber
  %15 = LOAD_TVALUE R2, 0i, tnumber
  SET_UPVALUE U0, %15, tnumber
  %25 = MUL_NUM %12, %10
  %36 = MUL_NUM %12, %11
  %45 = ADD_NUM %25, %36
  STORE_DOUBLE R2, %45
  %48 = LOAD_TVALUE R2, 0i, tnumber
  SET_UPVALUE U0, %48, tnumber
  %58 = ADD_NUM %45, %10
  STORE_DOUBLE R2, %58
  INTERRUPT 10u
  RETURN R2, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "UpvalueAccessLoadStore3")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local m = 1

local function foo()
    local a = m
    m = a
    local b = m
    m = b
    return m + a + b
end

function setm(x, y) m = x end
)"),
        R"(
; function foo() line 4
bb_bytecode_0:
  %0 = GET_UPVALUE U0
  SET_UPVALUE U0, %0, undef
  SET_UPVALUE U0, %0, undef
  STORE_TVALUE R4, %0
  CHECK_TAG R4, tnumber, bb_exit_1
   ; exit sync: R1, R0, {%0}
  %14 = LOAD_DOUBLE R4
  %16 = ADD_NUM %14, %14
  %25 = ADD_NUM %16, %14
  STORE_DOUBLE R2, %25
  STORE_TAG R2, tnumber
  INTERRUPT 7u
  RETURN R2, 1i
; function setm($arg0, $arg1) line 12
bb_bytecode_0:
  %0 = LOAD_TVALUE R0
  SET_UPVALUE U0, %0, undef
  INTERRUPT 1u
  RETURN R0, 0i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "UpvalueAccessLoadStore4")
{
    ScopedFastFlag luauBackedgeHeapCheck{FFlag::LuauBackedgeHeapCheck, true};
    ScopedFastFlag luauCodegenPropagateFallbackTags{FFlag::LuauCodegenPropagateFallbackTags, true};

    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local arr: {number}

local function foo(a: number)
    for i = 1,#arr do
        arr[i] = arr[i] + arr[i] * a
    end
end

arr = {1, 2, 3, 4}
)"),
        R"(
; function foo($arg0) line 4
bb_0:
  CHECK_TAG R0, tnumber, exit(entry)
  JUMP bb_4
bb_4:
  JUMP bb_bytecode_1
bb_bytecode_1:
  STORE_DOUBLE R3, 1
  STORE_TAG R3, tnumber
  %6 = GET_UPVALUE U0
  STORE_TVALUE R4, %6
  CHECK_TAG R4, ttable, exit(2)
  %10 = LOAD_POINTER R4
  CHECK_NO_METATABLE %10, bb_fallback_5
  %12 = TABLE_LEN %10
  %13 = INT_TO_NUM %12
  STORE_DOUBLE R1, %13
  STORE_TAG R1, tnumber
  JUMP bb_6
bb_6:
  STORE_DOUBLE R2, 1
  STORE_TAG R2, tnumber
  %26 = LOAD_DOUBLE R1
  JUMP_CMP_NUM R3, %26, not_le, bb_bytecode_3, bb_bytecode_2
bb_bytecode_2:
  INTERRUPT 5u
  %30 = GET_UPVALUE U0
  STORE_TVALUE R4, %30
  STORE_TVALUE R7, %30
  CHECK_TAG R7, ttable, exit(7)
  %38 = LOAD_POINTER R7
  %39 = LOAD_DOUBLE R3
  %40 = TRY_NUM_TO_INDEX %39, bb_fallback_7
  %41 = SUB_INT %40, 1i
  CHECK_ARRAY_SIZE %38, %41, bb_fallback_7
  CHECK_NO_METATABLE %38, bb_fallback_7
  %44 = GET_ARR_ADDR %38, %41
  %45 = LOAD_TVALUE %44
  STORE_TVALUE R6, %45
  JUMP bb_linear_17
bb_linear_17:
  STORE_TVALUE R8, %45
  CHECK_TAG R8, tnumber, bb_fallback_11
  %140 = LOAD_DOUBLE R8
  %142 = MUL_NUM %140, R0
  %152 = ADD_NUM %140, %142
  STORE_DOUBLE R5, %152
  STORE_TAG R5, tnumber
  CHECK_READONLY %38, bb_fallback_15
  STORE_SPLIT_TVALUE %44, tnumber, %152
  CHECK_GC
  %171 = LOAD_DOUBLE R1
  %173 = ADD_NUM %39, 1
  STORE_DOUBLE R3, %173
  JUMP_CMP_NUM %173, %171, le, bb_bytecode_2, bb_bytecode_3
bb_8:
  %51 = GET_UPVALUE U0
  STORE_TVALUE R9, %51
  CHECK_TAG R9, ttable, exit(9)
  %57 = LOAD_POINTER R9
  %58 = LOAD_DOUBLE R3
  %59 = TRY_NUM_TO_INDEX %58, bb_fallback_9
  %60 = SUB_INT %59, 1i
  CHECK_ARRAY_SIZE %57, %60, bb_fallback_9
  CHECK_NO_METATABLE %57, bb_fallback_9
  %63 = GET_ARR_ADDR %57, %60
  %64 = LOAD_TVALUE %63
  STORE_TVALUE R8, %64
  JUMP bb_10
bb_10:
  CHECK_TAG R8, tnumber, bb_fallback_11
  %74 = LOAD_DOUBLE R8
  %76 = MUL_NUM %74, R0
  STORE_DOUBLE R7, %76
  STORE_TAG R7, tnumber
  JUMP bb_12
bb_12:
  CHECK_TAG R6, tnumber, bb_fallback_13
  CHECK_TAG R7, tnumber, bb_fallback_13
  %87 = LOAD_DOUBLE R6
  %89 = ADD_NUM %87, R7
  STORE_DOUBLE R5, %89
  STORE_TAG R5, tnumber
  JUMP bb_14
bb_14:
  CHECK_TAG R4, ttable, exit(12)
  %100 = LOAD_POINTER R4
  %101 = LOAD_DOUBLE R3
  %102 = TRY_NUM_TO_INDEX %101, bb_fallback_15
  %103 = SUB_INT %102, 1i
  CHECK_ARRAY_SIZE %100, %103, bb_fallback_15
  CHECK_NO_METATABLE %100, bb_fallback_15
  CHECK_READONLY %100, bb_fallback_15
  %107 = GET_ARR_ADDR %100, %103
  %108 = LOAD_TVALUE R5
  STORE_TVALUE %107, %108
  BARRIER_TABLE_FORWARD %100, R5, undef
  JUMP bb_16
bb_16:
  CHECK_GC
  %116 = LOAD_DOUBLE R1
  %117 = LOAD_DOUBLE R3
  %118 = ADD_NUM %117, 1
  STORE_DOUBLE R3, %118
  JUMP_CMP_NUM %118, %116, le, bb_bytecode_2, bb_bytecode_3
bb_bytecode_3:
  INTERRUPT 14u
  RETURN R0, 0i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "BufferLoadStoreProp1")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function test(b: buffer)
    return buffer.readf32(b, 0) * buffer.readf32(b, 0) + buffer.readf32(b, 4) * buffer.readf32(b, 4)
end
)"),
        R"(
; function test($arg0) line 2
bb_0:
  CHECK_TAG R0, tbuffer, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  CHECK_SAFE_ENV bb_fallback_3
  %7 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %7, 0i, 0i, 8i, undef, exit(2)
  %10 = BUFFER_READF32 %7, 0i, tbuffer
  %11 = FLOAT_TO_NUM %10
  JUMP bb_linear_19
bb_linear_19:
  %147 = MUL_NUM %11, %11
  %153 = BUFFER_READF32 %7, 4i, tbuffer
  %154 = FLOAT_TO_NUM %153
  %171 = MUL_NUM %154, %154
  %177 = ADD_NUM %147, %171
  STORE_DOUBLE R1, %177
  STORE_TAG R1, tnumber
  INTERRUPT 31u
  RETURN R1, 1i
bb_4:
  CHECK_SAFE_ENV bb_fallback_7
  %32 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %32, 0i, 0i, 4i, undef, exit(9)
  %35 = BUFFER_READF32 %32, 0i, tbuffer
  %36 = FLOAT_TO_NUM %35
  STORE_DOUBLE R4, %36
  STORE_TAG R4, tnumber
  JUMP bb_8
bb_8:
  CHECK_TAG R3, tnumber, exit(14)
  CHECK_TAG R4, tnumber, exit(14)
  %58 = LOAD_DOUBLE R3
  %60 = MUL_NUM %58, R4
  STORE_DOUBLE R2, %60
  STORE_TAG R2, tnumber
  CHECK_SAFE_ENV bb_fallback_11
  %66 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %66, 4i, 0i, 4i, undef, exit(17)
  %69 = BUFFER_READF32 %66, 4i, tbuffer
  %70 = FLOAT_TO_NUM %69
  STORE_DOUBLE R4, %70
  JUMP bb_12
bb_12:
  CHECK_SAFE_ENV bb_fallback_15
  %91 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %91, 4i, 0i, 4i, undef, exit(24)
  %94 = BUFFER_READF32 %91, 4i, tbuffer
  %95 = FLOAT_TO_NUM %94
  STORE_DOUBLE R5, %95
  STORE_TAG R5, tnumber
  JUMP bb_16
bb_16:
  CHECK_TAG R4, tnumber, exit(29)
  CHECK_TAG R5, tnumber, exit(29)
  %117 = LOAD_DOUBLE R4
  %119 = MUL_NUM %117, R5
  CHECK_TAG R2, tnumber, bb_exit_20
   ; exit sync: R3, {%119}
  %126 = LOAD_DOUBLE R2
  %128 = ADD_NUM %126, %119
  STORE_DOUBLE R1, %128
  STORE_TAG R1, tnumber
  INTERRUPT 31u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "BufferLoadStoreProp2")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function test(b: buffer)
    buffer.writei8(b, 10, 32)
    assert(buffer.readi8(b, 10) == 32)

    buffer.writei8(b, 14, 4)
    buffer.writei8(b, 13, 3)
    buffer.writei8(b, 12, 2)
    buffer.writei8(b, 11, 1)

    return buffer.readi8(b, 11) + buffer.readi8(b, 12) + buffer.readi8(b, 14) + buffer.readi8(b, 13)
end
)"),
        R"(
; function test($arg0) line 2
bb_0:
  CHECK_TAG R0, tbuffer, exit(entry)
  JUMP bb_4
bb_4:
  JUMP bb_bytecode_1
bb_bytecode_1:
  STORE_DOUBLE R3, 10
  STORE_TAG R3, tnumber
  STORE_DOUBLE R4, 32
  STORE_TAG R4, tnumber
  CHECK_SAFE_ENV bb_fallback_5
  %15 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %15, 10i, 0i, 1i, undef, exit(4)
  BUFFER_WRITEI8 %15, 10i, 32i, tbuffer
  JUMP bb_6
bb_6:
  CHECK_SAFE_ENV bb_fallback_9
  %38 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %38, 10i, 0i, 1i, undef, exit(10)
  %41 = BUFFER_READI8 %38, 10i, tbuffer
  %42 = INT_TO_NUM %41
  STORE_DOUBLE R3, %42
  STORE_TAG R3, tnumber
  JUMP bb_10
bb_10:
  %60 = LOAD_TAG R3
  %61 = LOAD_DOUBLE R3
  %62 = CMP_SPLIT_TVALUE %60, tnumber, %61, 32, eq
  STORE_TAG R2, tboolean
  STORE_INT R2, %62
  JUMP bb_bytecode_3
bb_bytecode_3:
  CHECK_SAFE_ENV bb_fallback_13
  CHECK_TRUTHY tboolean, R2, exit(20)
  JUMP bb_linear_49
bb_linear_49:
  %342 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %342, 14i, -3i, 1i, undef, bb_exit_50
   ; exit sync: R4, R3, {}
  BUFFER_WRITEI8 %342, 14i, 4i, tbuffer
  BUFFER_WRITEI8 %342, 13i, 3i, tbuffer
  BUFFER_WRITEI8 %342, 12i, 2i, tbuffer
  BUFFER_WRITEI8 %342, 11i, 1i, tbuffer
  STORE_DOUBLE R1, 10
  STORE_TAG R1, tnumber
  INTERRUPT 86u
  RETURN R1, 1i
bb_14:
  STORE_DOUBLE R3, 14
  STORE_TAG R3, tnumber
  STORE_DOUBLE R4, 4
  STORE_TAG R4, tnumber
  CHECK_SAFE_ENV bb_fallback_17
  %95 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %95, 14i, 0i, 1i, undef, exit(27)
  BUFFER_WRITEI8 %95, 14i, 4i, tbuffer
  JUMP bb_18
bb_18:
  STORE_DOUBLE R3, 13
  STORE_TAG R3, tnumber
  STORE_DOUBLE R4, 3
  STORE_TAG R4, tnumber
  CHECK_SAFE_ENV bb_fallback_21
  %126 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %126, 13i, 0i, 1i, undef, exit(35)
  BUFFER_WRITEI8 %126, 13i, 3i, tbuffer
  JUMP bb_22
bb_22:
  STORE_DOUBLE R3, 12
  STORE_TAG R3, tnumber
  STORE_DOUBLE R4, 2
  STORE_TAG R4, tnumber
  CHECK_SAFE_ENV bb_fallback_25
  %157 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %157, 12i, 0i, 1i, undef, exit(43)
  BUFFER_WRITEI8 %157, 12i, 2i, tbuffer
  JUMP bb_26
bb_26:
  STORE_DOUBLE R3, 11
  STORE_TAG R3, tnumber
  STORE_DOUBLE R4, 1
  STORE_TAG R4, tnumber
  CHECK_SAFE_ENV bb_fallback_29
  %188 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %188, 11i, 0i, 1i, undef, exit(51)
  BUFFER_WRITEI8 %188, 11i, 1i, tbuffer
  JUMP bb_30
bb_30:
  CHECK_SAFE_ENV bb_fallback_33
  %211 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %211, 11i, 0i, 1i, undef, exit(57)
  %214 = BUFFER_READI8 %211, 11i, tbuffer
  %215 = INT_TO_NUM %214
  STORE_DOUBLE R4, %215
  STORE_TAG R4, tnumber
  JUMP bb_34
bb_34:
  CHECK_SAFE_ENV bb_fallback_37
  %236 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %236, 12i, 0i, 1i, undef, exit(64)
  %239 = BUFFER_READI8 %236, 12i, tbuffer
  %240 = INT_TO_NUM %239
  STORE_DOUBLE R5, %240
  STORE_TAG R5, tnumber
  JUMP bb_38
bb_38:
  CHECK_TAG R4, tnumber, exit(69)
  CHECK_TAG R5, tnumber, exit(69)
  %262 = LOAD_DOUBLE R4
  %264 = ADD_NUM %262, R5
  STORE_DOUBLE R3, %264
  STORE_TAG R3, tnumber
  CHECK_SAFE_ENV bb_fallback_41
  %270 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %270, 14i, 0i, 1i, undef, exit(72)
  %273 = BUFFER_READI8 %270, 14i, tbuffer
  %274 = INT_TO_NUM %273
  STORE_DOUBLE R4, %274
  JUMP bb_42
bb_42:
  CHECK_TAG R3, tnumber, exit(77)
  CHECK_TAG R4, tnumber, exit(77)
  %296 = LOAD_DOUBLE R3
  %298 = ADD_NUM %296, R4
  STORE_DOUBLE R2, %298
  STORE_TAG R2, tnumber
  CHECK_SAFE_ENV bb_fallback_45
  %304 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %304, 13i, 0i, 1i, undef, exit(80)
  %307 = BUFFER_READI8 %304, 13i, tbuffer
  %308 = INT_TO_NUM %307
  STORE_DOUBLE R3, %308
  JUMP bb_46
bb_46:
  CHECK_TAG R2, tnumber, exit(85)
  CHECK_TAG R3, tnumber, exit(85)
  %330 = LOAD_DOUBLE R2
  %332 = ADD_NUM %330, R3
  STORE_DOUBLE R1, %332
  STORE_TAG R1, tnumber
  INTERRUPT 86u
  RETURN R1, 1i
)"
    );
}

// When dealing with constants and buffer loads/store of the same size, all assertions disappear as conditions are true
TEST_CASE_FIXTURE(LoweringFixture, "BufferLoadStoreProp3")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function storeloadpreserve(b: buffer)
    buffer.writeu32(b, 0, 0xffffffff)
    assert(buffer.readi32(b, 0) == -1)
    assert(buffer.readu32(b, 0) == 4294967295)

    buffer.writei32(b, 0, -1)
    assert(buffer.readi32(b, 0) == -1)
    assert(buffer.readu32(b, 0) == 4294967295)

    buffer.writei16(b, 0, 65535)
    assert(buffer.readi16(b, 0) == -1)
    assert(buffer.readu16(b, 0) == 65535)

    buffer.writeu16(b, 0, 65535)
    assert(buffer.readi16(b, 0) == -1)
    assert(buffer.readu16(b, 0) == 65535)

    buffer.writeu8(b, 0, 0xffffffff)
    assert(buffer.readi8(b, 0) == -1)
    assert(buffer.readu8(b, 0) == 255)

    buffer.writeu16(b, 0, 0xffffffff)
    assert(buffer.readi16(b, 0) == -1)
    assert(buffer.readu16(b, 0) == 65535)
end
)"),
        R"(
; function storeloadpreserve($arg0) line 2
bb_0:
  CHECK_TAG R0, tbuffer, exit(entry)
  JUMP bb_26
bb_26:
  JUMP bb_bytecode_1
bb_bytecode_1:
  STORE_DOUBLE R3, 0
  STORE_TAG R3, tnumber
  STORE_DOUBLE R4, 4294967295
  STORE_TAG R4, tnumber
  CHECK_SAFE_ENV bb_fallback_27
  %15 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %15, 0i, 0i, 4i, undef, exit(4)
  BUFFER_WRITEI32 %15, 0i, -1i, tbuffer
  JUMP bb_28
bb_28:
  CHECK_SAFE_ENV bb_fallback_31
  %38 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %38, 0i, 0i, 4i, undef, exit(10)
  %41 = BUFFER_READI32 %38, 0i, tbuffer
  %42 = INT_TO_NUM %41
  STORE_DOUBLE R3, %42
  STORE_TAG R3, tnumber
  JUMP bb_32
bb_32:
  %60 = LOAD_TAG R3
  %61 = LOAD_DOUBLE R3
  %62 = CMP_SPLIT_TVALUE %60, tnumber, %61, -1, eq
  STORE_TAG R2, tboolean
  STORE_INT R2, %62
  JUMP bb_bytecode_3
bb_bytecode_3:
  CHECK_SAFE_ENV bb_fallback_35
  CHECK_TRUTHY R2, R2, exit(20)
  JUMP bb_36
bb_36:
  CHECK_SAFE_ENV bb_fallback_39
  %87 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %87, 0i, 0i, 4i, undef, exit(25)
  %90 = BUFFER_READI32 %87, 0i, tbuffer
  %91 = UINT_TO_NUM %90
  STORE_DOUBLE R3, %91
  STORE_TAG R3, tnumber
  JUMP bb_40
bb_40:
  %109 = LOAD_TAG R3
  %110 = LOAD_DOUBLE R3
  %111 = CMP_SPLIT_TVALUE %109, tnumber, %110, 4294967295, eq
  STORE_TAG R2, tboolean
  STORE_INT R2, %111
  JUMP bb_bytecode_5
bb_bytecode_5:
  CHECK_SAFE_ENV bb_fallback_43
  CHECK_TRUTHY tboolean, R2, exit(35)
  JUMP bb_linear_147
bb_linear_147:
  STORE_DOUBLE R4, -1
  STORE_TAG R4, tnumber
  %785 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %785, 0i, 0i, 4i, undef, bb_exit_152
   ; exit sync: R3, {}
  BUFFER_WRITEI32 %785, 0i, -1i, tbuffer
  STORE_SPLIT_TVALUE R3, tnumber, -1
  STORE_INT R2, 1i
  JUMP bb_bytecode_7
bb_44:
  STORE_DOUBLE R3, 0
  STORE_TAG R3, tnumber
  STORE_DOUBLE R4, -1
  STORE_TAG R4, tnumber
  CHECK_SAFE_ENV bb_fallback_47
  %144 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %144, 0i, 0i, 4i, undef, exit(42)
  BUFFER_WRITEI32 %144, 0i, -1i, tbuffer
  JUMP bb_48
bb_48:
  CHECK_SAFE_ENV bb_fallback_51
  %167 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %167, 0i, 0i, 4i, undef, exit(48)
  %170 = BUFFER_READI32 %167, 0i, tbuffer
  %171 = INT_TO_NUM %170
  STORE_DOUBLE R3, %171
  STORE_TAG R3, tnumber
  JUMP bb_52
bb_52:
  %189 = LOAD_TAG R3
  %190 = LOAD_DOUBLE R3
  %191 = CMP_SPLIT_TVALUE %189, tnumber, %190, -1, eq
  STORE_TAG R2, tboolean
  STORE_INT R2, %191
  JUMP bb_bytecode_7
bb_bytecode_7:
  CHECK_SAFE_ENV bb_fallback_55
  CHECK_TRUTHY R2, R2, exit(58)
  JUMP bb_56
bb_56:
  CHECK_SAFE_ENV bb_fallback_59
  %216 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %216, 0i, 0i, 4i, undef, exit(63)
  %219 = BUFFER_READI32 %216, 0i, tbuffer
  %220 = UINT_TO_NUM %219
  STORE_DOUBLE R3, %220
  STORE_TAG R3, tnumber
  JUMP bb_60
bb_60:
  %238 = LOAD_TAG R3
  %239 = LOAD_DOUBLE R3
  %240 = CMP_SPLIT_TVALUE %238, tnumber, %239, 4294967295, eq
  STORE_TAG R2, tboolean
  STORE_INT R2, %240
  JUMP bb_bytecode_9
bb_bytecode_9:
  CHECK_SAFE_ENV bb_fallback_63
  CHECK_TRUTHY tboolean, R2, exit(73)
  JUMP bb_linear_148
bb_linear_148:
  STORE_DOUBLE R4, 65535
  STORE_TAG R4, tnumber
  %808 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %808, 0i, 0i, 2i, undef, bb_exit_153
   ; exit sync: R3, {}
  BUFFER_WRITEI16 %808, 0i, 65535i, tbuffer
  STORE_SPLIT_TVALUE R3, tnumber, -1
  STORE_INT R2, 1i
  JUMP bb_bytecode_11
bb_64:
  STORE_DOUBLE R3, 0
  STORE_TAG R3, tnumber
  STORE_DOUBLE R4, 65535
  STORE_TAG R4, tnumber
  CHECK_SAFE_ENV bb_fallback_67
  %273 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %273, 0i, 0i, 2i, undef, exit(80)
  BUFFER_WRITEI16 %273, 0i, 65535i, tbuffer
  JUMP bb_68
bb_68:
  CHECK_SAFE_ENV bb_fallback_71
  %296 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %296, 0i, 0i, 2i, undef, exit(86)
  %299 = BUFFER_READI16 %296, 0i, tbuffer
  %300 = INT_TO_NUM %299
  STORE_DOUBLE R3, %300
  STORE_TAG R3, tnumber
  JUMP bb_72
bb_72:
  %318 = LOAD_TAG R3
  %319 = LOAD_DOUBLE R3
  %320 = CMP_SPLIT_TVALUE %318, tnumber, %319, -1, eq
  STORE_TAG R2, tboolean
  STORE_INT R2, %320
  JUMP bb_bytecode_11
bb_bytecode_11:
  CHECK_SAFE_ENV bb_fallback_75
  CHECK_TRUTHY R2, R2, exit(96)
  JUMP bb_76
bb_76:
  CHECK_SAFE_ENV bb_fallback_79
  %345 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %345, 0i, 0i, 2i, undef, exit(101)
  %348 = BUFFER_READU16 %345, 0i, tbuffer
  %349 = INT_TO_NUM %348
  STORE_DOUBLE R3, %349
  STORE_TAG R3, tnumber
  JUMP bb_80
bb_80:
  %367 = LOAD_TAG R3
  %368 = LOAD_DOUBLE R3
  %369 = CMP_SPLIT_TVALUE %367, tnumber, %368, 65535, eq
  STORE_TAG R2, tboolean
  STORE_INT R2, %369
  JUMP bb_bytecode_13
bb_bytecode_13:
  CHECK_SAFE_ENV bb_fallback_83
  CHECK_TRUTHY tboolean, R2, exit(111)
  JUMP bb_linear_149
bb_linear_149:
  STORE_DOUBLE R4, 65535
  STORE_TAG R4, tnumber
  %831 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %831, 0i, 0i, 2i, undef, bb_exit_154
   ; exit sync: R3, {}
  BUFFER_WRITEI16 %831, 0i, 65535i, tbuffer
  STORE_SPLIT_TVALUE R3, tnumber, -1
  STORE_INT R2, 1i
  JUMP bb_bytecode_15
bb_84:
  STORE_DOUBLE R3, 0
  STORE_TAG R3, tnumber
  STORE_DOUBLE R4, 65535
  STORE_TAG R4, tnumber
  CHECK_SAFE_ENV bb_fallback_87
  %402 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %402, 0i, 0i, 2i, undef, exit(118)
  BUFFER_WRITEI16 %402, 0i, 65535i, tbuffer
  JUMP bb_88
bb_88:
  CHECK_SAFE_ENV bb_fallback_91
  %425 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %425, 0i, 0i, 2i, undef, exit(124)
  %428 = BUFFER_READI16 %425, 0i, tbuffer
  %429 = INT_TO_NUM %428
  STORE_DOUBLE R3, %429
  STORE_TAG R3, tnumber
  JUMP bb_92
bb_92:
  %447 = LOAD_TAG R3
  %448 = LOAD_DOUBLE R3
  %449 = CMP_SPLIT_TVALUE %447, tnumber, %448, -1, eq
  STORE_TAG R2, tboolean
  STORE_INT R2, %449
  JUMP bb_bytecode_15
bb_bytecode_15:
  CHECK_SAFE_ENV bb_fallback_95
  CHECK_TRUTHY R2, R2, exit(134)
  JUMP bb_96
bb_96:
  CHECK_SAFE_ENV bb_fallback_99
  %474 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %474, 0i, 0i, 2i, undef, exit(139)
  %477 = BUFFER_READU16 %474, 0i, tbuffer
  %478 = INT_TO_NUM %477
  STORE_DOUBLE R3, %478
  STORE_TAG R3, tnumber
  JUMP bb_100
bb_100:
  %496 = LOAD_TAG R3
  %497 = LOAD_DOUBLE R3
  %498 = CMP_SPLIT_TVALUE %496, tnumber, %497, 65535, eq
  STORE_TAG R2, tboolean
  STORE_INT R2, %498
  JUMP bb_bytecode_17
bb_bytecode_17:
  CHECK_SAFE_ENV bb_fallback_103
  CHECK_TRUTHY tboolean, R2, exit(149)
  JUMP bb_linear_150
bb_linear_150:
  STORE_DOUBLE R4, 4294967295
  STORE_TAG R4, tnumber
  %854 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %854, 0i, 0i, 1i, undef, bb_exit_155
   ; exit sync: R3, {}
  BUFFER_WRITEI8 %854, 0i, -1i, tbuffer
  STORE_SPLIT_TVALUE R3, tnumber, -1
  STORE_INT R2, 1i
  JUMP bb_bytecode_19
bb_104:
  STORE_DOUBLE R3, 0
  STORE_TAG R3, tnumber
  STORE_DOUBLE R4, 4294967295
  STORE_TAG R4, tnumber
  CHECK_SAFE_ENV bb_fallback_107
  %531 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %531, 0i, 0i, 1i, undef, exit(156)
  BUFFER_WRITEI8 %531, 0i, -1i, tbuffer
  JUMP bb_108
bb_108:
  CHECK_SAFE_ENV bb_fallback_111
  %554 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %554, 0i, 0i, 1i, undef, exit(162)
  %557 = BUFFER_READI8 %554, 0i, tbuffer
  %558 = INT_TO_NUM %557
  STORE_DOUBLE R3, %558
  STORE_TAG R3, tnumber
  JUMP bb_112
bb_112:
  %576 = LOAD_TAG R3
  %577 = LOAD_DOUBLE R3
  %578 = CMP_SPLIT_TVALUE %576, tnumber, %577, -1, eq
  STORE_TAG R2, tboolean
  STORE_INT R2, %578
  JUMP bb_bytecode_19
bb_bytecode_19:
  CHECK_SAFE_ENV bb_fallback_115
  CHECK_TRUTHY R2, R2, exit(172)
  JUMP bb_116
bb_116:
  CHECK_SAFE_ENV bb_fallback_119
  %603 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %603, 0i, 0i, 1i, undef, exit(177)
  %606 = BUFFER_READU8 %603, 0i, tbuffer
  %607 = INT_TO_NUM %606
  STORE_DOUBLE R3, %607
  STORE_TAG R3, tnumber
  JUMP bb_120
bb_120:
  %625 = LOAD_TAG R3
  %626 = LOAD_DOUBLE R3
  %627 = CMP_SPLIT_TVALUE %625, tnumber, %626, 255, eq
  STORE_TAG R2, tboolean
  STORE_INT R2, %627
  JUMP bb_bytecode_21
bb_bytecode_21:
  CHECK_SAFE_ENV bb_fallback_123
  CHECK_TRUTHY tboolean, R2, exit(187)
  JUMP bb_linear_151
bb_linear_151:
  STORE_DOUBLE R4, 4294967295
  STORE_TAG R4, tnumber
  %877 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %877, 0i, 0i, 2i, undef, bb_exit_156
   ; exit sync: R3, {}
  BUFFER_WRITEI16 %877, 0i, -1i, tbuffer
  STORE_SPLIT_TVALUE R3, tnumber, -1
  STORE_INT R2, 1i
  JUMP bb_bytecode_23
bb_124:
  STORE_DOUBLE R3, 0
  STORE_TAG R3, tnumber
  STORE_DOUBLE R4, 4294967295
  STORE_TAG R4, tnumber
  CHECK_SAFE_ENV bb_fallback_127
  %660 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %660, 0i, 0i, 2i, undef, exit(194)
  BUFFER_WRITEI16 %660, 0i, -1i, tbuffer
  JUMP bb_128
bb_128:
  CHECK_SAFE_ENV bb_fallback_131
  %683 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %683, 0i, 0i, 2i, undef, exit(200)
  %686 = BUFFER_READI16 %683, 0i, tbuffer
  %687 = INT_TO_NUM %686
  STORE_DOUBLE R3, %687
  STORE_TAG R3, tnumber
  JUMP bb_132
bb_132:
  %705 = LOAD_TAG R3
  %706 = LOAD_DOUBLE R3
  %707 = CMP_SPLIT_TVALUE %705, tnumber, %706, -1, eq
  STORE_TAG R2, tboolean
  STORE_INT R2, %707
  JUMP bb_bytecode_23
bb_bytecode_23:
  CHECK_SAFE_ENV bb_fallback_135
  CHECK_TRUTHY R2, R2, exit(210)
  JUMP bb_136
bb_136:
  CHECK_SAFE_ENV bb_fallback_139
  %732 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %732, 0i, 0i, 2i, undef, exit(215)
  %735 = BUFFER_READU16 %732, 0i, tbuffer
  %736 = INT_TO_NUM %735
  STORE_DOUBLE R3, %736
  STORE_TAG R3, tnumber
  JUMP bb_140
bb_140:
  %754 = LOAD_TAG R3
  %755 = LOAD_DOUBLE R3
  %756 = CMP_SPLIT_TVALUE %754, tnumber, %755, 65535, eq
  STORE_TAG R2, tboolean
  STORE_INT R2, %756
  JUMP bb_bytecode_25
bb_bytecode_25:
  CHECK_SAFE_ENV bb_fallback_143
  CHECK_TRUTHY R2, R2, exit(225)
  JUMP bb_144
bb_144:
  INTERRUPT 228u
  RETURN R0, 0i
)"
    );
}

// When dealing with unknown numbers, stores can be propagated to loads with proper zero/signed extension
TEST_CASE_FIXTURE(LoweringFixture, "BufferLoadStoreProp4")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(R"(
local function test(b: buffer, n: number, f: number)
    buffer.writei8(b, 0, n)
    buffer.writef64(b, 100, buffer.readi8(b, 0))
    buffer.writei8(b, 108, buffer.readi8(b, 0))
    buffer.writeu8(b, 109, buffer.readi8(b, 0))

    buffer.writeu8(b, 2, n)
    buffer.writef64(b, 116, buffer.readu8(b, 2))
    buffer.writeu8(b, 124, buffer.readu8(b, 2))
    buffer.writei8(b, 125, buffer.readu8(b, 2))

    buffer.writei16(b, 4, n)
    buffer.writef64(b, 132, buffer.readi16(b, 4))
    buffer.writei16(b, 140, buffer.readi16(b, 4))
    buffer.writeu16(b, 142, buffer.readi16(b, 4))

    buffer.writeu16(b, 8, n)
    buffer.writef64(b, 148, buffer.readu16(b, 8))
    buffer.writeu16(b, 156, buffer.readu16(b, 8))
    buffer.writei16(b, 158, buffer.readu16(b, 8))

    buffer.writei32(b, 12, n)
    buffer.writef64(b, 164, buffer.readi32(b, 12))
    buffer.writei32(b, 172, buffer.readi32(b, 12))
    buffer.writeu32(b, 176, buffer.readi32(b, 12))

    buffer.writeu32(b, 20, n)
    buffer.writef64(b, 180, buffer.readu32(b, 20))
    buffer.writeu32(b, 188, buffer.readu32(b, 20))
    buffer.writei32(b, 192, buffer.readu32(b, 20))

    buffer.writef32(b, 28, f)
    buffer.writef64(b, 196, buffer.readf32(b, 28))
    buffer.writef32(b, 196, buffer.readf32(b, 28))

    buffer.writef64(b, 32, f)
    buffer.writef64(b, 204, buffer.readf64(b, 32))
    buffer.writef32(b, 204, buffer.readf64(b, 32))
end
)"),
        R"(
; function test($arg0, $arg1, $arg2) line 2
bb_0:
  CHECK_TAG R0, tbuffer, exit(entry)
  CHECK_TAG R1, tnumber, exit(entry)
  CHECK_TAG R2, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  STORE_DOUBLE R5, 0
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_3
  %17 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %17, 0i, 0i, 212i, undef, exit(3)
  %21 = LOAD_DOUBLE R1
  %22 = NUM_TO_UINT %21
  BUFFER_WRITEI8 %17, 0i, %22, tbuffer
  JUMP bb_linear_211
bb_linear_211:
  %1440 = SEXTI8_INT %22
  %1441 = INT_TO_NUM %1440
  BUFFER_WRITEF64 %17, 100i, %1441, tbuffer
  BUFFER_WRITEI8 %17, 108i, %22, tbuffer
  BUFFER_WRITEI8 %17, 109i, %22, tbuffer
  BUFFER_WRITEI8 %17, 2i, %22, tbuffer
  %1517 = BITAND_UINT %22, 255i
  %1518 = INT_TO_NUM %1517
  BUFFER_WRITEF64 %17, 116i, %1518, tbuffer
  BUFFER_WRITEI8 %17, 124i, %22, tbuffer
  BUFFER_WRITEI8 %17, 125i, %22, tbuffer
  BUFFER_WRITEI16 %17, 4i, %22, tbuffer
  %1594 = SEXTI16_INT %22
  %1595 = INT_TO_NUM %1594
  BUFFER_WRITEF64 %17, 132i, %1595, tbuffer
  BUFFER_WRITEI16 %17, 140i, %22, tbuffer
  BUFFER_WRITEI16 %17, 142i, %22, tbuffer
  BUFFER_WRITEI16 %17, 8i, %22, tbuffer
  %1671 = BITAND_UINT %22, 65535i
  %1672 = INT_TO_NUM %1671
  BUFFER_WRITEF64 %17, 148i, %1672, tbuffer
  BUFFER_WRITEI16 %17, 156i, %22, tbuffer
  BUFFER_WRITEI16 %17, 158i, %22, tbuffer
  BUFFER_WRITEI32 %17, 12i, %22, tbuffer
  %1748 = TRUNCATE_UINT %22
  %1749 = INT_TO_NUM %1748
  BUFFER_WRITEF64 %17, 164i, %1749, tbuffer
  BUFFER_WRITEI32 %17, 172i, %22, tbuffer
  BUFFER_WRITEI32 %17, 176i, %22, tbuffer
  BUFFER_WRITEI32 %17, 20i, %22, tbuffer
  %1826 = UINT_TO_NUM %22
  BUFFER_WRITEF64 %17, 180i, %1826, tbuffer
  BUFFER_WRITEI32 %17, 188i, %22, tbuffer
  BUFFER_WRITEI32 %17, 192i, %22, tbuffer
  %1893 = LOAD_DOUBLE R2
  %1894 = NUM_TO_FLOAT %1893
  BUFFER_WRITEF32 %17, 28i, %1894, tbuffer
  %1903 = FLOAT_TO_NUM %1894
  BUFFER_WRITEF64 %17, 196i, %1903, tbuffer
  BUFFER_WRITEF32 %17, 196i, %1894, tbuffer
  BUFFER_WRITEF64 %17, 32i, %1893, tbuffer
  BUFFER_WRITEF64 %17, 204i, %1893, tbuffer
  BUFFER_WRITEF32 %17, 204i, %1894, tbuffer
  INTERRUPT 372u
  RETURN R0, 0i
bb_4:
  STORE_DOUBLE R5, 100
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_7
  %44 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %44, 0i, 0i, 1i, undef, exit(11)
  %47 = BUFFER_READI8 %44, 0i, tbuffer
  %48 = INT_TO_NUM %47
  STORE_DOUBLE R6, %48
  STORE_TAG R6, tnumber
  JUMP bb_8
bb_8:
  CHECK_SAFE_ENV bb_fallback_11
  CHECK_TAG R5, tnumber, exit(18)
  CHECK_TAG R6, tnumber, exit(18)
  %73 = LOAD_POINTER R0
  %74 = LOAD_DOUBLE R5
  %75 = NUM_TO_INT %74
  CHECK_BUFFER_LEN %73, %75, 0i, 8i, undef, exit(18)
  %77 = LOAD_DOUBLE R6
  BUFFER_WRITEF64 %73, %75, %77, tbuffer
  JUMP bb_12
bb_12:
  STORE_DOUBLE R5, 108
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_15
  %97 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %97, 0i, 0i, 1i, undef, exit(25)
  %100 = BUFFER_READI8 %97, 0i, tbuffer
  %101 = INT_TO_NUM %100
  STORE_DOUBLE R6, %101
  STORE_TAG R6, tnumber
  JUMP bb_16
bb_16:
  CHECK_SAFE_ENV bb_fallback_19
  CHECK_TAG R5, tnumber, exit(32)
  CHECK_TAG R6, tnumber, exit(32)
  %126 = LOAD_POINTER R0
  %127 = LOAD_DOUBLE R5
  %128 = NUM_TO_INT %127
  CHECK_BUFFER_LEN %126, %128, 0i, 1i, undef, exit(32)
  %130 = LOAD_DOUBLE R6
  %131 = NUM_TO_UINT %130
  BUFFER_WRITEI8 %126, %128, %131, tbuffer
  JUMP bb_20
bb_20:
  STORE_DOUBLE R5, 109
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_23
  %151 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %151, 0i, 0i, 1i, undef, exit(39)
  %154 = BUFFER_READI8 %151, 0i, tbuffer
  %155 = INT_TO_NUM %154
  STORE_DOUBLE R6, %155
  STORE_TAG R6, tnumber
  JUMP bb_24
bb_24:
  CHECK_SAFE_ENV bb_fallback_27
  CHECK_TAG R5, tnumber, exit(46)
  CHECK_TAG R6, tnumber, exit(46)
  %180 = LOAD_POINTER R0
  %181 = LOAD_DOUBLE R5
  %182 = NUM_TO_INT %181
  CHECK_BUFFER_LEN %180, %182, 0i, 1i, undef, exit(46)
  %184 = LOAD_DOUBLE R6
  %185 = NUM_TO_UINT %184
  BUFFER_WRITEI8 %180, %182, %185, tbuffer
  JUMP bb_28
bb_28:
  STORE_DOUBLE R5, 2
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_31
  %209 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %209, 2i, 0i, 1i, undef, exit(53)
  %213 = LOAD_DOUBLE R1
  %214 = NUM_TO_UINT %213
  BUFFER_WRITEI8 %209, 2i, %214, tbuffer
  JUMP bb_32
bb_32:
  STORE_DOUBLE R5, 116
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_35
  %236 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %236, 2i, 0i, 1i, undef, exit(61)
  %239 = BUFFER_READU8 %236, 2i, tbuffer
  %240 = INT_TO_NUM %239
  STORE_DOUBLE R6, %240
  STORE_TAG R6, tnumber
  JUMP bb_36
bb_36:
  CHECK_SAFE_ENV bb_fallback_39
  CHECK_TAG R5, tnumber, exit(68)
  CHECK_TAG R6, tnumber, exit(68)
  %265 = LOAD_POINTER R0
  %266 = LOAD_DOUBLE R5
  %267 = NUM_TO_INT %266
  CHECK_BUFFER_LEN %265, %267, 0i, 8i, undef, exit(68)
  %269 = LOAD_DOUBLE R6
  BUFFER_WRITEF64 %265, %267, %269, tbuffer
  JUMP bb_40
bb_40:
  STORE_DOUBLE R5, 124
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_43
  %289 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %289, 2i, 0i, 1i, undef, exit(75)
  %292 = BUFFER_READU8 %289, 2i, tbuffer
  %293 = INT_TO_NUM %292
  STORE_DOUBLE R6, %293
  STORE_TAG R6, tnumber
  JUMP bb_44
bb_44:
  CHECK_SAFE_ENV bb_fallback_47
  CHECK_TAG R5, tnumber, exit(82)
  CHECK_TAG R6, tnumber, exit(82)
  %318 = LOAD_POINTER R0
  %319 = LOAD_DOUBLE R5
  %320 = NUM_TO_INT %319
  CHECK_BUFFER_LEN %318, %320, 0i, 1i, undef, exit(82)
  %322 = LOAD_DOUBLE R6
  %323 = NUM_TO_UINT %322
  BUFFER_WRITEI8 %318, %320, %323, tbuffer
  JUMP bb_48
bb_48:
  STORE_DOUBLE R5, 125
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_51
  %343 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %343, 2i, 0i, 1i, undef, exit(89)
  %346 = BUFFER_READU8 %343, 2i, tbuffer
  %347 = INT_TO_NUM %346
  STORE_DOUBLE R6, %347
  STORE_TAG R6, tnumber
  JUMP bb_52
bb_52:
  CHECK_SAFE_ENV bb_fallback_55
  CHECK_TAG R5, tnumber, exit(96)
  CHECK_TAG R6, tnumber, exit(96)
  %372 = LOAD_POINTER R0
  %373 = LOAD_DOUBLE R5
  %374 = NUM_TO_INT %373
  CHECK_BUFFER_LEN %372, %374, 0i, 1i, undef, exit(96)
  %376 = LOAD_DOUBLE R6
  %377 = NUM_TO_UINT %376
  BUFFER_WRITEI8 %372, %374, %377, tbuffer
  JUMP bb_56
bb_56:
  STORE_DOUBLE R5, 4
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_59
  %401 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %401, 4i, 0i, 2i, undef, exit(103)
  %405 = LOAD_DOUBLE R1
  %406 = NUM_TO_UINT %405
  BUFFER_WRITEI16 %401, 4i, %406, tbuffer
  JUMP bb_60
bb_60:
  STORE_DOUBLE R5, 132
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_63
  %428 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %428, 4i, 0i, 2i, undef, exit(111)
  %431 = BUFFER_READI16 %428, 4i, tbuffer
  %432 = INT_TO_NUM %431
  STORE_DOUBLE R6, %432
  STORE_TAG R6, tnumber
  JUMP bb_64
bb_64:
  CHECK_SAFE_ENV bb_fallback_67
  CHECK_TAG R5, tnumber, exit(118)
  CHECK_TAG R6, tnumber, exit(118)
  %457 = LOAD_POINTER R0
  %458 = LOAD_DOUBLE R5
  %459 = NUM_TO_INT %458
  CHECK_BUFFER_LEN %457, %459, 0i, 8i, undef, exit(118)
  %461 = LOAD_DOUBLE R6
  BUFFER_WRITEF64 %457, %459, %461, tbuffer
  JUMP bb_68
bb_68:
  STORE_DOUBLE R5, 140
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_71
  %481 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %481, 4i, 0i, 2i, undef, exit(125)
  %484 = BUFFER_READI16 %481, 4i, tbuffer
  %485 = INT_TO_NUM %484
  STORE_DOUBLE R6, %485
  STORE_TAG R6, tnumber
  JUMP bb_72
bb_72:
  CHECK_SAFE_ENV bb_fallback_75
  CHECK_TAG R5, tnumber, exit(132)
  CHECK_TAG R6, tnumber, exit(132)
  %510 = LOAD_POINTER R0
  %511 = LOAD_DOUBLE R5
  %512 = NUM_TO_INT %511
  CHECK_BUFFER_LEN %510, %512, 0i, 2i, undef, exit(132)
  %514 = LOAD_DOUBLE R6
  %515 = NUM_TO_UINT %514
  BUFFER_WRITEI16 %510, %512, %515, tbuffer
  JUMP bb_76
bb_76:
  STORE_DOUBLE R5, 142
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_79
  %535 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %535, 4i, 0i, 2i, undef, exit(139)
  %538 = BUFFER_READI16 %535, 4i, tbuffer
  %539 = INT_TO_NUM %538
  STORE_DOUBLE R6, %539
  STORE_TAG R6, tnumber
  JUMP bb_80
bb_80:
  CHECK_SAFE_ENV bb_fallback_83
  CHECK_TAG R5, tnumber, exit(146)
  CHECK_TAG R6, tnumber, exit(146)
  %564 = LOAD_POINTER R0
  %565 = LOAD_DOUBLE R5
  %566 = NUM_TO_INT %565
  CHECK_BUFFER_LEN %564, %566, 0i, 2i, undef, exit(146)
  %568 = LOAD_DOUBLE R6
  %569 = NUM_TO_UINT %568
  BUFFER_WRITEI16 %564, %566, %569, tbuffer
  JUMP bb_84
bb_84:
  STORE_DOUBLE R5, 8
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_87
  %593 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %593, 8i, 0i, 2i, undef, exit(153)
  %597 = LOAD_DOUBLE R1
  %598 = NUM_TO_UINT %597
  BUFFER_WRITEI16 %593, 8i, %598, tbuffer
  JUMP bb_88
bb_88:
  STORE_DOUBLE R5, 148
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_91
  %620 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %620, 8i, 0i, 2i, undef, exit(161)
  %623 = BUFFER_READU16 %620, 8i, tbuffer
  %624 = INT_TO_NUM %623
  STORE_DOUBLE R6, %624
  STORE_TAG R6, tnumber
  JUMP bb_92
bb_92:
  CHECK_SAFE_ENV bb_fallback_95
  CHECK_TAG R5, tnumber, exit(168)
  CHECK_TAG R6, tnumber, exit(168)
  %649 = LOAD_POINTER R0
  %650 = LOAD_DOUBLE R5
  %651 = NUM_TO_INT %650
  CHECK_BUFFER_LEN %649, %651, 0i, 8i, undef, exit(168)
  %653 = LOAD_DOUBLE R6
  BUFFER_WRITEF64 %649, %651, %653, tbuffer
  JUMP bb_96
bb_96:
  STORE_DOUBLE R5, 156
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_99
  %673 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %673, 8i, 0i, 2i, undef, exit(175)
  %676 = BUFFER_READU16 %673, 8i, tbuffer
  %677 = INT_TO_NUM %676
  STORE_DOUBLE R6, %677
  STORE_TAG R6, tnumber
  JUMP bb_100
bb_100:
  CHECK_SAFE_ENV bb_fallback_103
  CHECK_TAG R5, tnumber, exit(182)
  CHECK_TAG R6, tnumber, exit(182)
  %702 = LOAD_POINTER R0
  %703 = LOAD_DOUBLE R5
  %704 = NUM_TO_INT %703
  CHECK_BUFFER_LEN %702, %704, 0i, 2i, undef, exit(182)
  %706 = LOAD_DOUBLE R6
  %707 = NUM_TO_UINT %706
  BUFFER_WRITEI16 %702, %704, %707, tbuffer
  JUMP bb_104
bb_104:
  STORE_DOUBLE R5, 158
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_107
  %727 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %727, 8i, 0i, 2i, undef, exit(189)
  %730 = BUFFER_READU16 %727, 8i, tbuffer
  %731 = INT_TO_NUM %730
  STORE_DOUBLE R6, %731
  STORE_TAG R6, tnumber
  JUMP bb_108
bb_108:
  CHECK_SAFE_ENV bb_fallback_111
  CHECK_TAG R5, tnumber, exit(196)
  CHECK_TAG R6, tnumber, exit(196)
  %756 = LOAD_POINTER R0
  %757 = LOAD_DOUBLE R5
  %758 = NUM_TO_INT %757
  CHECK_BUFFER_LEN %756, %758, 0i, 2i, undef, exit(196)
  %760 = LOAD_DOUBLE R6
  %761 = NUM_TO_UINT %760
  BUFFER_WRITEI16 %756, %758, %761, tbuffer
  JUMP bb_112
bb_112:
  STORE_DOUBLE R5, 12
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_115
  %785 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %785, 12i, 0i, 4i, undef, exit(203)
  %789 = LOAD_DOUBLE R1
  %790 = NUM_TO_UINT %789
  BUFFER_WRITEI32 %785, 12i, %790, tbuffer
  JUMP bb_116
bb_116:
  STORE_DOUBLE R5, 164
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_119
  %812 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %812, 12i, 0i, 4i, undef, exit(211)
  %815 = BUFFER_READI32 %812, 12i, tbuffer
  %816 = INT_TO_NUM %815
  STORE_DOUBLE R6, %816
  STORE_TAG R6, tnumber
  JUMP bb_120
bb_120:
  CHECK_SAFE_ENV bb_fallback_123
  CHECK_TAG R5, tnumber, exit(218)
  CHECK_TAG R6, tnumber, exit(218)
  %841 = LOAD_POINTER R0
  %842 = LOAD_DOUBLE R5
  %843 = NUM_TO_INT %842
  CHECK_BUFFER_LEN %841, %843, 0i, 8i, undef, exit(218)
  %845 = LOAD_DOUBLE R6
  BUFFER_WRITEF64 %841, %843, %845, tbuffer
  JUMP bb_124
bb_124:
  STORE_DOUBLE R5, 172
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_127
  %865 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %865, 12i, 0i, 4i, undef, exit(225)
  %868 = BUFFER_READI32 %865, 12i, tbuffer
  %869 = INT_TO_NUM %868
  STORE_DOUBLE R6, %869
  STORE_TAG R6, tnumber
  JUMP bb_128
bb_128:
  CHECK_SAFE_ENV bb_fallback_131
  CHECK_TAG R5, tnumber, exit(232)
  CHECK_TAG R6, tnumber, exit(232)
  %894 = LOAD_POINTER R0
  %895 = LOAD_DOUBLE R5
  %896 = NUM_TO_INT %895
  CHECK_BUFFER_LEN %894, %896, 0i, 4i, undef, exit(232)
  %898 = LOAD_DOUBLE R6
  %899 = NUM_TO_UINT %898
  BUFFER_WRITEI32 %894, %896, %899, tbuffer
  JUMP bb_132
bb_132:
  STORE_DOUBLE R5, 176
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_135
  %919 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %919, 12i, 0i, 4i, undef, exit(239)
  %922 = BUFFER_READI32 %919, 12i, tbuffer
  %923 = INT_TO_NUM %922
  STORE_DOUBLE R6, %923
  STORE_TAG R6, tnumber
  JUMP bb_136
bb_136:
  CHECK_SAFE_ENV bb_fallback_139
  CHECK_TAG R5, tnumber, exit(246)
  CHECK_TAG R6, tnumber, exit(246)
  %948 = LOAD_POINTER R0
  %949 = LOAD_DOUBLE R5
  %950 = NUM_TO_INT %949
  CHECK_BUFFER_LEN %948, %950, 0i, 4i, undef, exit(246)
  %952 = LOAD_DOUBLE R6
  %953 = NUM_TO_UINT %952
  BUFFER_WRITEI32 %948, %950, %953, tbuffer
  JUMP bb_140
bb_140:
  STORE_DOUBLE R5, 20
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_143
  %977 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %977, 20i, 0i, 4i, undef, exit(253)
  %981 = LOAD_DOUBLE R1
  %982 = NUM_TO_UINT %981
  BUFFER_WRITEI32 %977, 20i, %982, tbuffer
  JUMP bb_144
bb_144:
  STORE_DOUBLE R5, 180
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_147
  %1004 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %1004, 20i, 0i, 4i, undef, exit(261)
  %1007 = BUFFER_READI32 %1004, 20i, tbuffer
  %1008 = UINT_TO_NUM %1007
  STORE_DOUBLE R6, %1008
  STORE_TAG R6, tnumber
  JUMP bb_148
bb_148:
  CHECK_SAFE_ENV bb_fallback_151
  CHECK_TAG R5, tnumber, exit(268)
  CHECK_TAG R6, tnumber, exit(268)
  %1033 = LOAD_POINTER R0
  %1034 = LOAD_DOUBLE R5
  %1035 = NUM_TO_INT %1034
  CHECK_BUFFER_LEN %1033, %1035, 0i, 8i, undef, exit(268)
  %1037 = LOAD_DOUBLE R6
  BUFFER_WRITEF64 %1033, %1035, %1037, tbuffer
  JUMP bb_152
bb_152:
  STORE_DOUBLE R5, 188
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_155
  %1057 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %1057, 20i, 0i, 4i, undef, exit(275)
  %1060 = BUFFER_READI32 %1057, 20i, tbuffer
  %1061 = UINT_TO_NUM %1060
  STORE_DOUBLE R6, %1061
  STORE_TAG R6, tnumber
  JUMP bb_156
bb_156:
  CHECK_SAFE_ENV bb_fallback_159
  CHECK_TAG R5, tnumber, exit(282)
  CHECK_TAG R6, tnumber, exit(282)
  %1086 = LOAD_POINTER R0
  %1087 = LOAD_DOUBLE R5
  %1088 = NUM_TO_INT %1087
  CHECK_BUFFER_LEN %1086, %1088, 0i, 4i, undef, exit(282)
  %1090 = LOAD_DOUBLE R6
  %1091 = NUM_TO_UINT %1090
  BUFFER_WRITEI32 %1086, %1088, %1091, tbuffer
  JUMP bb_160
bb_160:
  STORE_DOUBLE R5, 192
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_163
  %1111 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %1111, 20i, 0i, 4i, undef, exit(289)
  %1114 = BUFFER_READI32 %1111, 20i, tbuffer
  %1115 = UINT_TO_NUM %1114
  STORE_DOUBLE R6, %1115
  STORE_TAG R6, tnumber
  JUMP bb_164
bb_164:
  CHECK_SAFE_ENV bb_fallback_167
  CHECK_TAG R5, tnumber, exit(296)
  CHECK_TAG R6, tnumber, exit(296)
  %1140 = LOAD_POINTER R0
  %1141 = LOAD_DOUBLE R5
  %1142 = NUM_TO_INT %1141
  CHECK_BUFFER_LEN %1140, %1142, 0i, 4i, undef, exit(296)
  %1144 = LOAD_DOUBLE R6
  %1145 = NUM_TO_UINT %1144
  BUFFER_WRITEI32 %1140, %1142, %1145, tbuffer
  JUMP bb_168
bb_168:
  STORE_DOUBLE R5, 28
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_171
  %1169 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %1169, 28i, 0i, 4i, undef, exit(303)
  %1173 = LOAD_DOUBLE R2
  %1174 = NUM_TO_FLOAT %1173
  BUFFER_WRITEF32 %1169, 28i, %1174, tbuffer
  JUMP bb_172
bb_172:
  STORE_DOUBLE R5, 196
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_175
  %1196 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %1196, 28i, 0i, 4i, undef, exit(311)
  %1199 = BUFFER_READF32 %1196, 28i, tbuffer
  %1200 = FLOAT_TO_NUM %1199
  STORE_DOUBLE R6, %1200
  STORE_TAG R6, tnumber
  JUMP bb_176
bb_176:
  CHECK_SAFE_ENV bb_fallback_179
  CHECK_TAG R5, tnumber, exit(318)
  CHECK_TAG R6, tnumber, exit(318)
  %1225 = LOAD_POINTER R0
  %1226 = LOAD_DOUBLE R5
  %1227 = NUM_TO_INT %1226
  CHECK_BUFFER_LEN %1225, %1227, 0i, 8i, undef, exit(318)
  %1229 = LOAD_DOUBLE R6
  BUFFER_WRITEF64 %1225, %1227, %1229, tbuffer
  JUMP bb_180
bb_180:
  STORE_DOUBLE R5, 196
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_183
  %1249 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %1249, 28i, 0i, 4i, undef, exit(325)
  %1252 = BUFFER_READF32 %1249, 28i, tbuffer
  %1253 = FLOAT_TO_NUM %1252
  STORE_DOUBLE R6, %1253
  STORE_TAG R6, tnumber
  JUMP bb_184
bb_184:
  CHECK_SAFE_ENV bb_fallback_187
  CHECK_TAG R5, tnumber, exit(332)
  CHECK_TAG R6, tnumber, exit(332)
  %1278 = LOAD_POINTER R0
  %1279 = LOAD_DOUBLE R5
  %1280 = NUM_TO_INT %1279
  CHECK_BUFFER_LEN %1278, %1280, 0i, 4i, undef, exit(332)
  %1282 = LOAD_DOUBLE R6
  %1283 = NUM_TO_FLOAT %1282
  BUFFER_WRITEF32 %1278, %1280, %1283, tbuffer
  JUMP bb_188
bb_188:
  STORE_DOUBLE R5, 32
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_191
  %1307 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %1307, 32i, 0i, 8i, undef, exit(339)
  %1311 = LOAD_DOUBLE R2
  BUFFER_WRITEF64 %1307, 32i, %1311, tbuffer
  JUMP bb_192
bb_192:
  STORE_DOUBLE R5, 204
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_195
  %1333 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %1333, 32i, 0i, 8i, undef, exit(347)
  %1336 = BUFFER_READF64 %1333, 32i, tbuffer
  STORE_DOUBLE R6, %1336
  STORE_TAG R6, tnumber
  JUMP bb_196
bb_196:
  CHECK_SAFE_ENV bb_fallback_199
  CHECK_TAG R5, tnumber, exit(354)
  CHECK_TAG R6, tnumber, exit(354)
  %1361 = LOAD_POINTER R0
  %1362 = LOAD_DOUBLE R5
  %1363 = NUM_TO_INT %1362
  CHECK_BUFFER_LEN %1361, %1363, 0i, 8i, undef, exit(354)
  %1365 = LOAD_DOUBLE R6
  BUFFER_WRITEF64 %1361, %1363, %1365, tbuffer
  JUMP bb_200
bb_200:
  STORE_DOUBLE R5, 204
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_203
  %1385 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %1385, 32i, 0i, 8i, undef, exit(361)
  %1388 = BUFFER_READF64 %1385, 32i, tbuffer
  STORE_DOUBLE R6, %1388
  STORE_TAG R6, tnumber
  JUMP bb_204
bb_204:
  CHECK_SAFE_ENV bb_fallback_207
  CHECK_TAG R5, tnumber, exit(368)
  CHECK_TAG R6, tnumber, exit(368)
  %1413 = LOAD_POINTER R0
  %1414 = LOAD_DOUBLE R5
  %1415 = NUM_TO_INT %1414
  CHECK_BUFFER_LEN %1413, %1415, 0i, 4i, undef, exit(368)
  %1417 = LOAD_DOUBLE R6
  %1418 = NUM_TO_FLOAT %1417
  BUFFER_WRITEF32 %1413, %1415, %1418, tbuffer
  JUMP bb_208
bb_208:
  INTERRUPT 372u
  RETURN R0, 0i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "LoopStepDetection1")
{
    ScopedFastFlag luauBackedgeHeapCheck{FFlag::LuauBackedgeHeapCheck, true};

    assemblyOptions.includeRegFlowInfo = Luau::CodeGen::IncludeRegFlowInfo::Yes;

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(n: number)
    local s = 0
    for i = 1,n do
        s += i
    end
    return s
end
)"
               ),
        R"(
; function foo($arg0) line 2
bb_0:
; in regs: R0
; out regs: R0
  CHECK_TAG R0, tnumber, exit(entry)
  JUMP bb_4
bb_4:
; in regs: R0
; out regs: R0
  JUMP bb_bytecode_1
bb_bytecode_1:
; in regs: R0
; out regs: R1, R2, R3, R4
  STORE_DOUBLE R1, 0
  STORE_TAG R1, tnumber
  STORE_DOUBLE R4, 1
  STORE_TAG R4, tnumber
  %8 = LOAD_TVALUE R0, 0i, tnumber
  STORE_TVALUE R2, %8
  STORE_DOUBLE R3, 1
  STORE_TAG R3, tnumber
  %16 = LOAD_DOUBLE R0
  JUMP_CMP_NUM 1, %16, not_le, bb_bytecode_3, bb_bytecode_2
bb_bytecode_2:
; in regs: R1, R2, R3, R4
; out regs: R1, R2, R3, R4
  INTERRUPT 5u
  CHECK_TAG R1, tnumber, exit(5)
  CHECK_TAG R4, tnumber, exit(5)
  %24 = LOAD_DOUBLE R1
  %25 = LOAD_DOUBLE R4
  %26 = ADD_NUM %24, %25
  STORE_DOUBLE R1, %26
  CHECK_GC
  %29 = LOAD_DOUBLE R2
  %31 = ADD_NUM %25, 1
  STORE_DOUBLE R4, %31
  JUMP_CMP_NUM %31, %29, le, bb_bytecode_2, bb_bytecode_3
bb_bytecode_3:
; in regs: R1
  INTERRUPT 7u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "LoopStepDetection2")
{
    ScopedFastFlag luauBackedgeHeapCheck{FFlag::LuauBackedgeHeapCheck, true};
    ScopedFastFlag luauCodegenPropagateFallbackTags{FFlag::LuauCodegenPropagateFallbackTags, true};

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(n: number, t: {number})
    local s = 0
    for i = 1,#t do
        s += t[i]
    end
    return s
end
)"
               ),
        R"(
; function foo($arg0, $arg1) line 2
bb_0:
  CHECK_TAG R0, tnumber, exit(entry)
  CHECK_TAG R1, ttable, exit(entry)
  JUMP bb_4
bb_4:
  JUMP bb_bytecode_1
bb_bytecode_1:
  STORE_DOUBLE R2, 0
  STORE_TAG R2, tnumber
  STORE_DOUBLE R5, 1
  STORE_TAG R5, tnumber
  %12 = LOAD_POINTER R1
  CHECK_NO_METATABLE %12, bb_fallback_5
  %14 = TABLE_LEN %12
  %15 = INT_TO_NUM %14
  STORE_DOUBLE R3, %15
  STORE_TAG R3, tnumber
  JUMP bb_6
bb_6:
  STORE_DOUBLE R4, 1
  STORE_TAG R4, tnumber
  %28 = LOAD_DOUBLE R3
  JUMP_CMP_NUM R5, %28, not_le, bb_bytecode_3, bb_bytecode_2
bb_bytecode_2:
  INTERRUPT 5u
  CHECK_TAG R5, tnumber, exit(5)
  %36 = LOAD_POINTER R1
  %37 = LOAD_DOUBLE R5
  %38 = TRY_NUM_TO_INDEX %37, bb_fallback_7
  %39 = SUB_INT %38, 1i
  CHECK_ARRAY_SIZE %36, %39, bb_fallback_7
  CHECK_NO_METATABLE %36, bb_fallback_7
  %42 = GET_ARR_ADDR %36, %39
  %43 = LOAD_TVALUE %42
  STORE_TVALUE R6, %43
  JUMP bb_8
bb_8:
  CHECK_TAG R2, tnumber, exit(6)
  CHECK_TAG R6, tnumber, bb_fallback_9
  %53 = LOAD_DOUBLE R2
  %55 = ADD_NUM %53, R6
  STORE_DOUBLE R2, %55
  JUMP bb_10
bb_10:
  CHECK_GC
  %62 = LOAD_DOUBLE R3
  %63 = LOAD_DOUBLE R5
  %64 = ADD_NUM %63, 1
  STORE_DOUBLE R5, %64
  JUMP_CMP_NUM %64, %62, le, bb_bytecode_2, bb_bytecode_3
bb_bytecode_3:
  INTERRUPT 8u
  RETURN R2, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "UintSourceSanity")
{
    // TODO: opportunity - many conversions and stores remain because of VM exits
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(b: buffer, a: number, s: string)
    local r1 = buffer.readi32(b, bit32.bor(a, 0))
    local r2 = buffer.readu32(b, r1)
    local r3 = buffer.readi32(b, r2)
    local r4 = buffer.readu32(b, string.len(s))
    return r1, r2, r3, r4
end
)"
               ),
        R"(
; function foo($arg0, $arg1, $arg2) line 2
bb_0:
  CHECK_TAG R0, tbuffer, exit(entry)
  CHECK_TAG R1, tnumber, exit(entry)
  CHECK_TAG R2, tstring, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  CHECK_SAFE_ENV bb_fallback_3
  %11 = LOAD_DOUBLE R1
  %12 = NUM_TO_UINT %11
  JUMP bb_linear_27
bb_linear_27:
  %167 = LOAD_POINTER R0
  %169 = TRUNCATE_UINT %12
  CHECK_BUFFER_LEN %167, %169, 0i, 4i, undef, bb_exit_28
   ; exit sync: R5, {%12}
  %171 = BUFFER_READI32 %167, %169, tbuffer
  %172 = INT_TO_NUM %171
  STORE_DOUBLE R3, %172
  STORE_TAG R3, tnumber
  CHECK_BUFFER_LEN %167, %171, 0i, 4i, undef, bb_exit_29
   ; exit sync: R5, {%12}
  %183 = BUFFER_READI32 %167, %171, tbuffer
  %184 = UINT_TO_NUM %183
  STORE_DOUBLE R4, %184
  STORE_TAG R4, tnumber
  CHECK_BUFFER_LEN %167, %183, 0i, 4i, undef, bb_exit_30
   ; exit sync: R5, {%12}
  %195 = BUFFER_READI32 %167, %183, tbuffer
  %196 = INT_TO_NUM %195
  STORE_SPLIT_TVALUE R5, tnumber, %196
  %201 = LOAD_POINTER R2
  %202 = STRING_LEN %201
  CHECK_BUFFER_LEN %167, %202, 0i, 4i, undef, bb_exit_31
   ; exit sync: R8, {%202}
  %214 = BUFFER_READI32 %167, %202, tbuffer
  %215 = UINT_TO_NUM %214
  STORE_DOUBLE R6, %215
  STORE_TAG R6, tnumber
  INTERRUPT 38u
  RETURN R3, 4i
bb_4:
  CHECK_SAFE_ENV bb_fallback_7
  CHECK_TAG R5, tnumber, exit(9)
  %38 = LOAD_POINTER R0
  %39 = LOAD_DOUBLE R5
  %40 = NUM_TO_INT %39
  CHECK_BUFFER_LEN %38, %40, 0i, 4i, undef, exit(9)
  %42 = BUFFER_READI32 %38, %40, tbuffer
  %43 = INT_TO_NUM %42
  STORE_DOUBLE R3, %43
  STORE_TAG R3, tnumber
  JUMP bb_8
bb_8:
  CHECK_SAFE_ENV bb_fallback_11
  CHECK_TAG R3, tnumber, exit(15)
  %64 = LOAD_POINTER R0
  %65 = LOAD_DOUBLE R3
  %66 = NUM_TO_INT %65
  CHECK_BUFFER_LEN %64, %66, 0i, 4i, undef, exit(15)
  %68 = BUFFER_READI32 %64, %66, tbuffer
  %69 = UINT_TO_NUM %68
  STORE_DOUBLE R4, %69
  STORE_TAG R4, tnumber
  JUMP bb_12
bb_12:
  CHECK_SAFE_ENV bb_fallback_15
  CHECK_TAG R4, tnumber, exit(22)
  %92 = LOAD_POINTER R0
  %93 = LOAD_DOUBLE R4
  %94 = NUM_TO_INT %93
  CHECK_BUFFER_LEN %92, %94, 0i, 4i, undef, exit(22)
  %96 = BUFFER_READI32 %92, %94, tbuffer
  %97 = INT_TO_NUM %96
  STORE_DOUBLE R5, %97
  STORE_TAG R5, tnumber
  JUMP bb_16
bb_16:
  CHECK_SAFE_ENV bb_fallback_19
  %118 = LOAD_POINTER R2
  %119 = STRING_LEN %118
  %120 = INT_TO_NUM %119
  STORE_DOUBLE R8, %120
  STORE_TAG R8, tnumber
  JUMP bb_20
bb_20:
  CHECK_SAFE_ENV bb_fallback_23
  CHECK_TAG R8, tnumber, exit(34)
  %141 = LOAD_POINTER R0
  %142 = LOAD_DOUBLE R8
  %143 = NUM_TO_INT %142
  CHECK_BUFFER_LEN %141, %143, 0i, 4i, undef, exit(34)
  %145 = BUFFER_READI32 %141, %143, tbuffer
  %146 = UINT_TO_NUM %145
  STORE_DOUBLE R6, %146
  STORE_TAG R6, tnumber
  JUMP bb_24
bb_24:
  INTERRUPT 38u
  RETURN R3, 4i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "LibmIsPure")
{
    ensureVectorFloat();
    ensureVectorSize3();

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(p: vector, v: vector): vector
    return vector.create(
        math.cos(0.6 * p.x + 0.4 * math.sin(v.y) + 0),
        math.cos(0.6 * p.x + 0.4 * math.sin(v.y) + 1),
        math.cos(0.6 * p.x + 0.4 * math.sin(v.y) + 2)
    )
end
)"
               ),
        R"(
; function foo($arg0, $arg1) line 2
bb_0:
  CHECK_TAG R0, tvector, exit(entry)
  CHECK_TAG R1, tvector, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %8 = LOAD_FLOAT R0, 0i
  %9 = FLOAT_TO_NUM %8
  %15 = MUL_NUM %9, 0.59999999999999998
  STORE_DOUBLE R6, %15
  STORE_TAG R6, tnumber
  %20 = LOAD_FLOAT R1, 4i
  %21 = FLOAT_TO_NUM %20
  STORE_DOUBLE R9, %21
  STORE_TAG R9, tnumber
  CHECK_SAFE_ENV bb_fallback_3
  %28 = INVOKE_LIBM 24u, %21
  JUMP bb_linear_31
bb_linear_31:
  %262 = MUL_NUM %28, 0.40000000000000002
  %268 = ADD_NUM %15, %262
  %271 = ADD_NUM %268, 0
  %275 = INVOKE_LIBM 9u, %271
  %307 = ADD_NUM %268, 1
  %311 = INVOKE_LIBM 9u, %307
  %343 = ADD_NUM %268, 2
  %347 = INVOKE_LIBM 9u, %343
  %361 = NUM_TO_FLOAT %275
  %362 = NUM_TO_FLOAT %311
  %363 = NUM_TO_FLOAT %347
  STORE_VECTOR R2, %361, %362, %363
  STORE_TAG R2, tvector
  INTERRUPT 52u
  RETURN R2, 1i
bb_4:
  CHECK_TAG R8, tnumber, exit(9)
  %44 = LOAD_DOUBLE R8
  %45 = MUL_NUM %44, 0.40000000000000002
  STORE_DOUBLE R7, %45
  STORE_TAG R7, tnumber
  CHECK_TAG R6, tnumber, exit(10)
  %52 = LOAD_DOUBLE R6
  %54 = ADD_NUM %52, %45
  STORE_DOUBLE R5, %54
  STORE_TAG R5, tnumber
  %60 = ADD_NUM %54, 0
  STORE_DOUBLE R4, %60
  STORE_TAG R4, tnumber
  CHECK_SAFE_ENV bb_fallback_7
  %67 = INVOKE_LIBM 9u, %60
  STORE_DOUBLE R3, %67
  STORE_TAG R3, tnumber
  JUMP bb_8
bb_8:
  %83 = LOAD_FLOAT R0, 0i
  %84 = FLOAT_TO_NUM %83
  STORE_DOUBLE R8, %84
  STORE_TAG R8, tnumber
  %90 = MUL_NUM %84, 0.59999999999999998
  STORE_DOUBLE R7, %90
  STORE_TAG R7, tnumber
  %95 = LOAD_FLOAT R1, 4i
  %96 = FLOAT_TO_NUM %95
  STORE_DOUBLE R10, %96
  STORE_TAG R10, tnumber
  CHECK_SAFE_ENV bb_fallback_11
  %103 = INVOKE_LIBM 24u, %96
  STORE_DOUBLE R9, %103
  STORE_TAG R9, tnumber
  JUMP bb_12
bb_12:
  CHECK_TAG R9, tnumber, exit(25)
  %119 = LOAD_DOUBLE R9
  %120 = MUL_NUM %119, 0.40000000000000002
  STORE_DOUBLE R8, %120
  STORE_TAG R8, tnumber
  CHECK_TAG R7, tnumber, exit(26)
  %127 = LOAD_DOUBLE R7
  %129 = ADD_NUM %127, %120
  STORE_DOUBLE R6, %129
  STORE_TAG R6, tnumber
  %135 = ADD_NUM %129, 1
  STORE_DOUBLE R5, %135
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_15
  %142 = INVOKE_LIBM 9u, %135
  STORE_DOUBLE R4, %142
  STORE_TAG R4, tnumber
  JUMP bb_16
bb_16:
  %158 = LOAD_FLOAT R0, 0i
  %159 = FLOAT_TO_NUM %158
  STORE_DOUBLE R9, %159
  STORE_TAG R9, tnumber
  %165 = MUL_NUM %159, 0.59999999999999998
  STORE_DOUBLE R8, %165
  STORE_TAG R8, tnumber
  %170 = LOAD_FLOAT R1, 4i
  %171 = FLOAT_TO_NUM %170
  STORE_DOUBLE R11, %171
  STORE_TAG R11, tnumber
  CHECK_SAFE_ENV bb_fallback_19
  %178 = INVOKE_LIBM 24u, %171
  STORE_DOUBLE R10, %178
  STORE_TAG R10, tnumber
  JUMP bb_20
bb_20:
  CHECK_TAG R10, tnumber, exit(41)
  %194 = LOAD_DOUBLE R10
  %195 = MUL_NUM %194, 0.40000000000000002
  STORE_DOUBLE R9, %195
  STORE_TAG R9, tnumber
  CHECK_TAG R8, tnumber, exit(42)
  %202 = LOAD_DOUBLE R8
  %204 = ADD_NUM %202, %195
  STORE_DOUBLE R7, %204
  STORE_TAG R7, tnumber
  %210 = ADD_NUM %204, 2
  STORE_DOUBLE R6, %210
  STORE_TAG R6, tnumber
  CHECK_SAFE_ENV bb_fallback_23
  %217 = INVOKE_LIBM 9u, %210
  STORE_DOUBLE R5, %217
  STORE_TAG R5, tnumber
  JUMP bb_24
bb_24:
  CHECK_SAFE_ENV bb_fallback_27
  CHECK_TAG R3, tnumber, exit(49)
  CHECK_TAG R4, tnumber, exit(49)
  CHECK_TAG R5, tnumber, exit(49)
  %238 = LOAD_DOUBLE R3
  %239 = LOAD_DOUBLE R4
  %240 = LOAD_DOUBLE R5
  %241 = NUM_TO_FLOAT %238
  %242 = NUM_TO_FLOAT %239
  %243 = NUM_TO_FLOAT %240
  STORE_VECTOR R2, %241, %242, %243
  STORE_TAG R2, tvector
  JUMP bb_28
bb_28:
  INTERRUPT 52u
  RETURN R2, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "VecOpReuse")
{
    ensureVectorFloat();
    ensureVectorSize3();

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(c: vector): vector
    return vector.create(
        math.sin(3.0 * vector.magnitude(c) + 6.0),
        math.sin(3.0 * vector.magnitude(c) + 1.0),
        math.sin(3.0 * vector.magnitude(c) + 2.0)
    )
end
)"
               ),
        R"(
; function foo($arg0) line 2
bb_0:
  CHECK_TAG R0, tvector, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  CHECK_SAFE_ENV bb_fallback_3
  %7 = LOAD_TVALUE R0, 0i, tvector
  %8 = DOT_VEC %7, %7
  %9 = SQRT_FLOAT %8
  %10 = FLOAT_TO_NUM %9
  JUMP bb_linear_31
bb_linear_31:
  %191 = MUL_NUM %10, 3
  %194 = ADD_NUM %191, 6
  %198 = INVOKE_LIBM 24u, %194
  %216 = ADD_NUM %191, 1
  %220 = INVOKE_LIBM 24u, %216
  %238 = ADD_NUM %191, 2
  %242 = INVOKE_LIBM 24u, %238
  %256 = NUM_TO_FLOAT %198
  %257 = NUM_TO_FLOAT %220
  %258 = NUM_TO_FLOAT %242
  STORE_VECTOR R1, %256, %257, %258
  STORE_TAG R1, tvector
  INTERRUPT 37u
  RETURN R1, 1i
bb_4:
  CHECK_TAG R5, tnumber, exit(5)
  %28 = LOAD_DOUBLE R5
  %29 = MUL_NUM %28, 3
  STORE_DOUBLE R4, %29
  STORE_TAG R4, tnumber
  %35 = ADD_NUM %29, 6
  STORE_DOUBLE R3, %35
  STORE_TAG R3, tnumber
  CHECK_SAFE_ENV bb_fallback_7
  %42 = INVOKE_LIBM 24u, %35
  STORE_DOUBLE R2, %42
  STORE_TAG R2, tnumber
  JUMP bb_8
bb_8:
  CHECK_SAFE_ENV bb_fallback_11
  %59 = LOAD_TVALUE R0, 0i, tvector
  %60 = DOT_VEC %59, %59
  %61 = SQRT_FLOAT %60
  %62 = FLOAT_TO_NUM %61
  STORE_DOUBLE R6, %62
  STORE_TAG R6, tnumber
  JUMP bb_12
bb_12:
  CHECK_TAG R6, tnumber, exit(16)
  %80 = LOAD_DOUBLE R6
  %81 = MUL_NUM %80, 3
  STORE_DOUBLE R5, %81
  STORE_TAG R5, tnumber
  %87 = ADD_NUM %81, 1
  STORE_DOUBLE R4, %87
  STORE_TAG R4, tnumber
  CHECK_SAFE_ENV bb_fallback_15
  %94 = INVOKE_LIBM 24u, %87
  STORE_DOUBLE R3, %94
  STORE_TAG R3, tnumber
  JUMP bb_16
bb_16:
  CHECK_SAFE_ENV bb_fallback_19
  %111 = LOAD_TVALUE R0, 0i, tvector
  %112 = DOT_VEC %111, %111
  %113 = SQRT_FLOAT %112
  %114 = FLOAT_TO_NUM %113
  STORE_DOUBLE R7, %114
  STORE_TAG R7, tnumber
  JUMP bb_20
bb_20:
  CHECK_TAG R7, tnumber, exit(27)
  %132 = LOAD_DOUBLE R7
  %133 = MUL_NUM %132, 3
  STORE_DOUBLE R6, %133
  STORE_TAG R6, tnumber
  %139 = ADD_NUM %133, 2
  STORE_DOUBLE R5, %139
  STORE_TAG R5, tnumber
  CHECK_SAFE_ENV bb_fallback_23
  %146 = INVOKE_LIBM 24u, %139
  STORE_DOUBLE R4, %146
  STORE_TAG R4, tnumber
  JUMP bb_24
bb_24:
  CHECK_SAFE_ENV bb_fallback_27
  CHECK_TAG R2, tnumber, exit(34)
  CHECK_TAG R3, tnumber, exit(34)
  CHECK_TAG R4, tnumber, exit(34)
  %167 = LOAD_DOUBLE R2
  %168 = LOAD_DOUBLE R3
  %169 = LOAD_DOUBLE R4
  %170 = NUM_TO_FLOAT %167
  %171 = NUM_TO_FLOAT %168
  %172 = NUM_TO_FLOAT %169
  STORE_VECTOR R1, %170, %171, %172
  STORE_TAG R1, tvector
  JUMP bb_28
bb_28:
  INTERRUPT 37u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "VecOpReuse2")
{
    ensureVectorFloat();

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(c: vector, d: vector): vector
    return {2 * c + d, 2 * c + d}
end
)"
               ),
        R"(
; function foo($arg0, $arg1) line 2
bb_0:
  CHECK_TAG R0, tvector, exit(entry)
  CHECK_TAG R1, tvector, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  SET_SAVEDPC 1u
  %7 = NEW_TABLE 2u, 0u
  STORE_POINTER R2, %7
  STORE_TAG R2, ttable
  CHECK_GC
  %13 = LOAD_TVALUE R0, 0i, tvector
  %15 = FLOAT_TO_VEC 2
  %16 = MUL_VEC %13, %15
  %24 = LOAD_TVALUE R1, 0i, tvector
  %25 = ADD_VEC %16, %24
  %26 = TAG_VECTOR %25
  STORE_TVALUE R3, %26
  STORE_TVALUE R4, %26
  SETLIST 6u, R2, R3, 2i, 1u, 2u
  INTERRUPT 8u
  RETURN R2, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "TableOperationTagSuggestion1")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function test(t: { x: number, y: number }, a: string)
    t.x = 2
    t[a] = 4
    return t.x * 2
end
)",
                   true,
                   1,
                   2,
                   true
               ),
        R"(
; function test($arg0, $arg1) line 2
; R0: table [argument]
; R1: string [argument]
bb_0:
  CHECK_TAG R0, ttable, exit(entry)
  CHECK_TAG R1, tstring, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  STORE_DOUBLE R2, 2
  STORE_TAG R2, tnumber
  %10 = LOAD_POINTER R0
  %11 = GET_SLOT_NODE_ADDR %10, 1u, K0 ('x')
  CHECK_SLOT_MATCH %11, K0 ('x'), bb_fallback_3
  CHECK_READONLY %10, bb_fallback_3
  STORE_SPLIT_TVALUE %11, tnumber, 2, 0i
  JUMP bb_linear_9
bb_linear_9:
  STORE_DOUBLE R2, 4
  SET_SAVEDPC 5u
  SET_TABLE R2, R0, R1
  %50 = LOAD_POINTER R0
  %51 = GET_SLOT_NODE_ADDR %50, 5u, K0 ('x')
  CHECK_SLOT_MATCH %51, K0 ('x'), bb_fallback_5
  %53 = LOAD_TVALUE %51, 0i
  STORE_TVALUE R3, %53
  CHECK_TAG R3, tnumber, bb_fallback_7
  %58 = LOAD_DOUBLE R3
  %59 = ADD_NUM %58, %58
  STORE_DOUBLE R2, %59
  INTERRUPT 8u
  RETURN R2, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "TableOperationTagSuggestion2")
{
    ScopedFastFlag emitCallFb{FFlag::LuauEmitCallFeedback, true};

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function test(self, t: { id: string }, a: number)
    self.map[t.id] = self.map[t.id] + a
    self.foo(self.map[t.id])
end
)",
                   true,
                   1,
                   2,
                   true
               ),
        R"(
; function test($arg0, $arg1, $arg2) line 2
; R1: table [argument]
; R2: number [argument]
; R4: string from 10 to 11
; R6: string from 17 to 18
; R8: string from 8 to 9
bb_0:
  CHECK_TAG R1, ttable, exit(entry)
  CHECK_TAG R2, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  CHECK_TAG R0, ttable, bb_fallback_3
  %8 = LOAD_POINTER R0
  %9 = GET_SLOT_NODE_ADDR %8, 0u, K0 ('map')
  CHECK_SLOT_MATCH %9, K0 ('map'), bb_fallback_3
  %11 = LOAD_TVALUE %9, 0i
  STORE_TVALUE R3, %11
  JUMP bb_linear_19
bb_linear_19:
  %100 = LOAD_POINTER R1
  %101 = GET_SLOT_NODE_ADDR %100, 2u, K1 ('id')
  CHECK_SLOT_MATCH %101, K1 ('id'), bb_fallback_5
  %103 = LOAD_TVALUE %101, 0i
  STORE_TVALUE R4, %103
  STORE_TVALUE R7, %11
  STORE_TVALUE R8, %103
  SET_SAVEDPC 9u
  GET_TABLE R6, R7, R8
  CHECK_TAG R6, tnumber, bb_fallback_11
  %124 = LOAD_DOUBLE R6
  %126 = ADD_NUM %124, R2
  STORE_DOUBLE R5, %126
  STORE_TAG R5, tnumber
  SET_SAVEDPC 11u
  SET_TABLE R5, R3, R4
  %134 = LOAD_POINTER R0
  %135 = GET_SLOT_NODE_ADDR %134, 11u, K2 ('foo')
  CHECK_SLOT_MATCH %135, K2 ('foo'), bb_fallback_13
  %137 = LOAD_TVALUE %135, 0i
  STORE_TVALUE R3, %137
  %143 = GET_SLOT_NODE_ADDR %134, 13u, K0 ('map')
  CHECK_SLOT_MATCH %143, K0 ('map'), bb_fallback_15
  %145 = LOAD_TVALUE %143, 0i
  STORE_TVALUE R5, %145
  %148 = LOAD_POINTER R1
  %149 = GET_SLOT_NODE_ADDR %148, 15u, K1 ('id')
  CHECK_SLOT_MATCH %149, K1 ('id'), bb_fallback_17
  %151 = LOAD_TVALUE %149, 0i
  STORE_TVALUE R6, %151
  JUMP bb_18
bb_4:
  %18 = LOAD_POINTER R1
  %19 = GET_SLOT_NODE_ADDR %18, 2u, K1 ('id')
  CHECK_SLOT_MATCH %19, K1 ('id'), bb_fallback_5
  %21 = LOAD_TVALUE %19, 0i
  STORE_TVALUE R4, %21
  JUMP bb_6
bb_6:
  CHECK_TAG R0, ttable, bb_fallback_7
  %28 = LOAD_POINTER R0
  %29 = GET_SLOT_NODE_ADDR %28, 4u, K0 ('map')
  CHECK_SLOT_MATCH %29, K0 ('map'), bb_fallback_7
  %31 = LOAD_TVALUE %29, 0i
  STORE_TVALUE R7, %31
  JUMP bb_8
bb_8:
  %38 = LOAD_POINTER R1
  %39 = GET_SLOT_NODE_ADDR %38, 6u, K1 ('id')
  CHECK_SLOT_MATCH %39, K1 ('id'), bb_fallback_9
  %41 = LOAD_TVALUE %39, 0i
  STORE_TVALUE R8, %41
  JUMP bb_10
bb_10:
  SET_SAVEDPC 9u
  GET_TABLE R6, R7, R8
  CHECK_TAG R6, tnumber, bb_fallback_11
  %52 = LOAD_DOUBLE R6
  %54 = ADD_NUM %52, R2
  STORE_DOUBLE R5, %54
  STORE_TAG R5, tnumber
  JUMP bb_12
bb_12:
  SET_SAVEDPC 11u
  SET_TABLE R5, R3, R4
  CHECK_TAG R0, ttable, bb_fallback_13
  %65 = LOAD_POINTER R0
  %66 = GET_SLOT_NODE_ADDR %65, 11u, K2 ('foo')
  CHECK_SLOT_MATCH %66, K2 ('foo'), bb_fallback_13
  %68 = LOAD_TVALUE %66, 0i
  STORE_TVALUE R3, %68
  JUMP bb_14
bb_14:
  CHECK_TAG R0, ttable, bb_fallback_15
  %75 = LOAD_POINTER R0
  %76 = GET_SLOT_NODE_ADDR %75, 13u, K0 ('map')
  CHECK_SLOT_MATCH %76, K0 ('map'), bb_fallback_15
  %78 = LOAD_TVALUE %76, 0i
  STORE_TVALUE R5, %78
  JUMP bb_16
bb_16:
  %85 = LOAD_POINTER R1
  %86 = GET_SLOT_NODE_ADDR %85, 15u, K1 ('id')
  CHECK_SLOT_MATCH %86, K1 ('id'), bb_fallback_17
  %88 = LOAD_TVALUE %86, 0i
  STORE_TVALUE R6, %88
  JUMP bb_18
bb_18:
  SET_SAVEDPC 18u
  GET_TABLE R4, R5, R6
  INTERRUPT 18u
  SET_SAVEDPC 20u
  CALL R3, 1i, 0i
  INTERRUPT 20u
  RETURN R0, 0i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "Collatz")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function collatz(x : number)
    return if ((x % 2) == 1) then 3 * x + 1 else x // 2
end
)",
                   true,
                   1,
                   2
               ),
        R"(
; function collatz($arg0) line 2
; R0: number [argument]
bb_0:
  CHECK_TAG R0, tnumber, exit(entry)
  JUMP bb_3
bb_3:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %6 = LOAD_DOUBLE R0
  %7 = MOD_NUM %6, 2
  JUMP bb_5
bb_5:
  JUMP_CMP_NUM %7, 1, not_eq, bb_bytecode_2, bb_4
bb_4:
  %16 = LOAD_DOUBLE R0
  %17 = MUL_NUM %16, 3
  %23 = ADD_NUM %17, 1
  STORE_DOUBLE R1, %23
  STORE_TAG R1, tnumber
  INTERRUPT 5u
  RETURN R1, 1i
bb_bytecode_2:
  %30 = LOAD_DOUBLE R0
  %31 = IDIV_NUM %30, 2
  STORE_DOUBLE R1, %31
  STORE_TAG R1, tnumber
  INTERRUPT 7u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "TypeAliasResolution")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
type foo = number
type bar = foo

local function meow(foo: foo, bar: bar)
  return foo + bar
end
)",
                   true,
                   1,
                   2
               ),
        R"(
; function meow($arg0, $arg1) line 5
; R0: number [argument]
; R1: number [argument]
bb_0:
  CHECK_TAG R0, tnumber, exit(entry)
  CHECK_TAG R1, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %10 = LOAD_DOUBLE R0
  %12 = ADD_NUM %10, R1
  STORE_DOUBLE R2, %12
  STORE_TAG R2, tnumber
  INTERRUPT 1u
  RETURN R2, 1i
)"
    );

    // ensure mutually recursive types do not break
    // this function looks smaller than the above, because it avoids the useless bb_2 block
    // however, by requiring bb_fallback_1, the generated assembly is larger
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
type foo = bar
type bar = foo

local function meow(foo: foo, bar: bar)
  return foo + bar
end
)",
                   true,
                   1,
                   2
               ),
        R"(
; function meow($arg0, $arg1) line 5
bb_bytecode_0:
  CHECK_TAG R0, tnumber, bb_fallback_1
  CHECK_TAG R1, tnumber, bb_fallback_1
  %4 = LOAD_DOUBLE R0
  %6 = ADD_NUM %4, R1
  STORE_DOUBLE R2, %6
  STORE_TAG R2, tnumber
  JUMP bb_2
bb_2:
  INTERRUPT 1u
  RETURN R2, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "IntegerMultiargValidate")
{
    ScopedFastFlag luauIntegerFastcalls{FFlag::LuauIntegerFastcalls, true};
    ScopedFastFlag LuauCodegenInteger3{FFlag::LuauCodegenInteger3, true};
    ScopedFastFlag luauIntegerType{FFlag::LuauIntegerType2, true};

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function f(a, b)
    return integer.bxor(a, b, a)
end
)"
               ),
        R"(
; function f($arg0, $arg1) line 2
bb_bytecode_0:
  CHECK_SAFE_ENV bb_fallback_1
  CHECK_TAG R0, tinteger, exit(2)
  CHECK_TAG R1, tinteger, exit(2)
  %7 = LOAD_INT64 R0
  %8 = LOAD_INT64 R1
  %9 = BITXOR_INT64 %7, %8
  %11 = BITXOR_INT64 %9, %7
  STORE_INT64 R2, %11
  STORE_TAG R2, tinteger
  JUMP bb_2
bb_2:
  INTERRUPT 8u
  RETURN R2, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "IntegerMultiargValidate2")
{
    ScopedFastFlag luauIntegerFastcalls{FFlag::LuauIntegerFastcalls, true};
    ScopedFastFlag LuauCodegenInteger3{FFlag::LuauCodegenInteger3, true};
    ScopedFastFlag luauIntegerType{FFlag::LuauIntegerType2, true};

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function f(a, b)
    return integer.clamp(a, b, a)
end
)"
               ),
        R"(
; function f($arg0, $arg1) line 2
bb_bytecode_0:
  CHECK_SAFE_ENV bb_fallback_1
  CHECK_TAG R0, tinteger, exit(2)
  CHECK_TAG R1, tinteger, exit(2)
  %7 = LOAD_INT64 R0
  %8 = LOAD_INT64 R1
  CHECK_CMP_INT64 %8, %7, le, exit(2)
  %11 = SELECT_INT64 %7, %8, %7, %8, lt
  %12 = SELECT_INT64 %11, %7, %11, %7, gt
  STORE_INT64 R2, %12
  STORE_TAG R2, tinteger
  JUMP bb_2
bb_2:
  INTERRUPT 8u
  RETURN R2, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "IntegerMultiargValidate3")
{
    ScopedFastFlag luauIntegerFastcalls{FFlag::LuauIntegerFastcalls, true};
    ScopedFastFlag LuauCodegenInteger3{FFlag::LuauCodegenInteger3, true};
    ScopedFastFlag luauIntegerType{FFlag::LuauIntegerType2, true};

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function f(a, b)
    return integer.mul(integer.min(a, b, a), integer.max(a, b, a))
end
)"
               ),
        R"(
; function f($arg0, $arg1) line 2
bb_bytecode_0:
  CHECK_SAFE_ENV bb_fallback_1
  CHECK_TAG R0, tinteger, exit(2)
  CHECK_TAG R1, tinteger, exit(2)
  %7 = LOAD_INT64 R0
  %8 = LOAD_INT64 R1
  %9 = SELECT_INT64 %7, %8, %8, %7, le
  %11 = SELECT_INT64 %7, %9, %9, %7, le
  JUMP bb_linear_13
bb_linear_13:
  %92 = SELECT_INT64 %7, %8, %8, %7, gt
  %93 = SELECT_INT64 %7, %92, %92, %7, gt
  %104 = MUL_INT64 %11, %93
  STORE_INT64 R2, %104
  STORE_TAG R2, tinteger
  INTERRUPT 21u
  RETURN R2, 1i
bb_2:
  CHECK_SAFE_ENV bb_fallback_5
  CHECK_TAG R0, tinteger, exit(10)
  CHECK_TAG R1, tinteger, exit(10)
  %38 = LOAD_INT64 R0
  %39 = LOAD_INT64 R1
  %40 = SELECT_INT64 %38, %39, %39, %38, gt
  %42 = SELECT_INT64 %38, %40, %40, %38, gt
  STORE_INT64 R4, %42
  STORE_TAG R4, tinteger
  JUMP bb_6
bb_6:
  CHECK_SAFE_ENV bb_fallback_9
  CHECK_TAG R3, tinteger, exit(18)
  CHECK_TAG R4, tinteger, exit(18)
  %67 = LOAD_INT64 R3
  %68 = LOAD_INT64 R4
  %69 = MUL_INT64 %67, %68
  STORE_INT64 R2, %69
  STORE_TAG R2, tinteger
  JUMP bb_10
bb_10:
  INTERRUPT 21u
  RETURN R2, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "IntegerFastcallWrongConst")
{
    ScopedFastFlag luauIntegerFastcalls{FFlag::LuauIntegerFastcalls, true};
    ScopedFastFlag LuauCodegenInteger3{FFlag::LuauCodegenInteger3, true};

    // Check that this compiles with no assertions
    CHECK(
        getCodegenAssembly(R"(
local function f(...)
    integer.add(..., 0.5)
    integer.sub(..., 0.5)
    integer.mul(..., 0.5)
    integer.div(..., 0.5)
    integer.idiv(..., 0.5)
    integer.udiv(..., 0.5)
    integer.rem(..., 0.5)
    integer.urem(..., 0.5)
    integer.mod(..., 0.5)

    integer.min(..., 0.5)
    integer.max(..., 0.5)

    integer.band(..., 0.5)
    integer.bor(..., 0.5)
    integer.bxor(..., 0.5)
    integer.btest(..., 0.5)

    integer.extract(..., 0.5)

    integer.lrotate(..., 0.5)
    integer.rrotate(..., 0.5)
    integer.lshift(..., 0.5)
    integer.rshift(..., 0.5)
    integer.arshift(..., 0.5)

    integer.lt(..., 0.5)
    integer.le(..., 0.5)
    integer.gt(..., 0.5)
    integer.ge(..., 0.5)
    integer.ult(..., 0.5)
    integer.ule(..., 0.5)
    integer.ugt(..., 0.5)
    integer.uge(..., 0.5)
end
)")
            .size() > 0
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "NumberFastcallWrongConst")
{
    ScopedFastFlag LuauCodegenInteger3{FFlag::LuauCodegenInteger3, true};
    ScopedFastFlag luauIntegerType{FFlag::LuauIntegerType2, true};

    // Check that this compiles with no assertions
    CHECK(
        getCodegenAssembly(R"(
local function f(...)
    -- 2-arg math
    math.pow(..., 5i)
    math.fmod(..., 5i)
    math.atan2(..., 5i)
    math.ldexp(..., 5i)
    math.min(..., 5i)
    math.max(..., 5i)

    -- bit32 multiarg
    bit32.band(..., 5i)
    bit32.bor(..., 5i)
    bit32.bxor(..., 5i)
    bit32.btest(..., 5i)

    -- bit32 shift/rotate
    bit32.lshift(..., 5i)
    bit32.rshift(..., 5i)
    bit32.arshift(..., 5i)
    bit32.lrotate(..., 5i)
    bit32.rrotate(..., 5i)

    -- bit32 extract (2-arg)
    bit32.extract(..., 5i)

    -- vector constructor (2-arg)
    vector.create(..., 5i)

    -- buffer reads (offset is checked as double)
    buffer.readi8(..., 5i)
    buffer.readu8(..., 5i)
    buffer.readi16(..., 5i)
    buffer.readu16(..., 5i)
    buffer.readi32(..., 5i)
    buffer.readu32(..., 5i)
    buffer.readf32(..., 5i)
    buffer.readf64(..., 5i)
end
)")
            .size() > 0
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "IntegerFastcallConstant")
{
    ScopedFastFlag luauIntegerFastcalls{FFlag::LuauIntegerFastcalls, true};
    ScopedFastFlag LuauCodegenInteger3{FFlag::LuauCodegenInteger3, true};
    ScopedFastFlag luauIntegerType{FFlag::LuauIntegerType2, true};

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(x: integer)
    return integer.band(x, 5i)
end
)",
                   true,
                   1,
                   2
               ),
        R"(
; function foo($arg0) line 2
; R0: integer [argument]
bb_0:
  CHECK_TAG R0, tinteger, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  CHECK_SAFE_ENV bb_fallback_3
  %7 = LOAD_INT64 R0
  %8 = BITAND_INT64 %7, 5i
  STORE_INT64 R1, %8
  STORE_TAG R1, tinteger
  JUMP bb_4
bb_4:
  INTERRUPT 7u
  RETURN R1, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "BufferWriteChecksExtraArgs")
{
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(b: buffer, offset: number, val)
    return buffer.writeu32(b, offset, val, 0) -- unused extra argument
end
)",
                   true,
                   1,
                   2
               ),
        R"(
; function foo($arg0, $arg1, $arg2) line 2
; R0: buffer [argument]
; R1: number [argument]
bb_0:
  CHECK_TAG R0, tbuffer, exit(entry)
  CHECK_TAG R1, tnumber, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %6 = LOAD_TVALUE R0, 0i, tbuffer
  STORE_TVALUE R4, %6
  %8 = LOAD_TVALUE R1, 0i, tnumber
  STORE_TVALUE R5, %8
  %10 = LOAD_TVALUE R2
  STORE_TVALUE R6, %10
  STORE_DOUBLE R7, 0
  STORE_TAG R7, tnumber
  CHECK_SAFE_ENV bb_fallback_3
  CHECK_TAG R2, tnumber, exit(5)
  %21 = LOAD_POINTER R0
  %22 = LOAD_DOUBLE R1
  %23 = NUM_TO_INT %22
  CHECK_BUFFER_LEN %21, %23, 0i, 4i, undef, exit(5)
  %25 = LOAD_DOUBLE R2
  %26 = NUM_TO_UINT %25
  BUFFER_WRITEI32 %21, %23, %26, tbuffer
  ADJUST_STACK_TO_REG R3, 0i
  JUMP bb_4
bb_4:
  INTERRUPT 8u
  RETURN R3, -1i
)"
    );
}
TEST_CASE_FIXTURE(LoweringFixture, "IntegerCompare")
{
    ScopedFastFlag luauIntegerFastcalls{FFlag::LuauIntegerFastcalls, true};
    ScopedFastFlag LuauCodegenInteger3{FFlag::LuauCodegenInteger3, true};
    ScopedFastFlag luauIntegerType{FFlag::LuauIntegerType2, true};
    ScopedFastFlag luauCodegenIntegerCompare{FFlag::LuauCodegenIntegerCompare, true};

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(x: integer, y: integer)
    if x == y then
        return 1
    end
    return 2
end
)",
                   true,
                   1,
                   2
               ),
        R"(
; function foo($arg0, $arg1) line 2
; R0: integer [argument]
; R1: integer [argument]
bb_0:
  CHECK_TAG R0, tinteger, exit(entry)
  CHECK_TAG R1, tinteger, exit(entry)
  JUMP bb_3
bb_3:
  JUMP bb_bytecode_1
bb_bytecode_1:
  %10 = LOAD_INT64 R0
  %11 = LOAD_INT64 R1
  JUMP_CMP_INT64 %10, %11, not_eq, bb_bytecode_2, bb_4
bb_4:
  STORE_DOUBLE R2, 1
  STORE_TAG R2, tnumber
  INTERRUPT 3u
  RETURN R2, 1i
bb_bytecode_2:
  STORE_DOUBLE R2, 2
  STORE_TAG R2, tnumber
  INTERRUPT 5u
  RETURN R2, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "IntegerCompareConstLhs")
{
    ScopedFastFlag luauIntegerFastcalls{FFlag::LuauIntegerFastcalls, true};
    ScopedFastFlag LuauCodegenInteger3{FFlag::LuauCodegenInteger3, true};
    ScopedFastFlag luauIntegerType{FFlag::LuauIntegerType2, true};
    ScopedFastFlag luauCodegenBufferInteger{FFlag::LuauCodegenBufferInteger, true};
    ScopedFastFlag luauIntegerBufferFastcalls{FFlag::LuauIntegerBufferFastcalls, true};
    ScopedFastFlag luauCodegenIntegerCompare{FFlag::LuauCodegenIntegerCompare, true};
    ScopedFastFlag luauCompileReuseLocalRegs{FFlag::LuauCompileReuseLocalRegs, true};

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(b: buffer, x: integer)
    -- bytecode compiler is unlikely to constant-fold this
    buffer.writeinteger(b, 0, 2i)
    local lhs = buffer.readinteger(b, 0)

    if lhs == x then
        return 1
    end
    return 2
end
)",
                   true,
                   1,
                   2
               ),
        R"(
; function foo($arg0, $arg1) line 2
; R0: buffer [argument]
; R1: integer [argument]
; R2: integer from 8 to 21
bb_0:
  CHECK_TAG R0, tbuffer, exit(entry)
  CHECK_TAG R1, tinteger, exit(entry)
  JUMP bb_3
bb_3:
  JUMP bb_bytecode_1
bb_bytecode_1:
  STORE_DOUBLE R4, 0
  STORE_TAG R4, tnumber
  STORE_INT64 R5, 2i
  STORE_TAG R5, tinteger
  CHECK_SAFE_ENV bb_fallback_4
  %17 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %17, 0i, 0i, 8i, undef, exit(4)
  BUFFER_WRITEI64 %17, 0i, 2i, tbuffer
  JUMP bb_5
bb_5:
  CHECK_SAFE_ENV bb_fallback_8
  %39 = LOAD_POINTER R0
  CHECK_BUFFER_LEN %39, 0i, 0i, 8i, undef, exit(10)
  %42 = BUFFER_READI64 %39, 0i, tbuffer
  STORE_INT64 R2, %42
  STORE_TAG R2, tinteger
  JUMP bb_9
bb_9:
  CHECK_TAG R2, tinteger, bb_fallback_13
  %64 = LOAD_INT64 R2
  %65 = LOAD_INT64 R1
  JUMP_CMP_INT64 %64, %65, not_eq, bb_bytecode_2, bb_12
bb_12:
  STORE_DOUBLE R3, 1
  STORE_TAG R3, tnumber
  INTERRUPT 18u
  RETURN R3, 1i
bb_bytecode_2:
  STORE_DOUBLE R3, 2
  STORE_TAG R3, tnumber
  INTERRUPT 20u
  RETURN R3, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "OptionalUserdataTypeRemapping")
{
    // This test requires runtime component to be present
    if (!Luau::CodeGen::isSupported())
        return;

    ScopedFastFlag remapOptional{FFlag::LuauLoadRemapOptionalUserdata, true};

    // Argument types
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(b: vec2?)
    return b
end
)",
                   /* includeIrTypes */ true
               ),
        R"(
; function foo($arg0) line 2
; R0: vec2? [argument]
bb_0:
  %0 = LOAD_TAG R0
  JUMP_EQ_TAG %0, tnil, bb_2, bb_3
bb_3:
  CHECK_TAG %0, tuserdata, exit(entry)
  JUMP bb_2
bb_2:
  JUMP bb_bytecode_1
bb_bytecode_1:
  INTERRUPT 0u
  RETURN R0, 1i
)"
    );

    // Upvalue types
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local up: vec2? = nil
local function foo()
    return up
end
return foo
)",
                   /* includeIrTypes */ true,
                   /* debugLevel */ 2
               ),
        R"(
; function foo() line 3
; U0: vec2? ['up']
bb_bytecode_0:
  STORE_TAG R0, tnil
  INTERRUPT 1u
  RETURN R0, 1i
)"
    );

    // Local types
    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo()
    local v: vec2? = nil
    return v
end
return foo
)",
                   /* includeIrTypes */ true,
                   /* debugLevel */ 2
               ),
        R"(
; function foo() line 2
; R0: vec2? from 0 to 2 [local 'v']
bb_bytecode_0:
  STORE_TAG R0, tnil
  INTERRUPT 1u
  RETURN R0, 1i
)"
    );
}

TEST_CASE_FIXTURE(LoweringFixture, "ClassConstruction")
{
    ScopedFastFlag debugLuauUserDefinedClasses{FFlag::DebugLuauUserDefinedClasses, true};
    ScopedFastFlag debugLuauUserDefinedClassesRuntime{FFlag::DebugLuauUserDefinedClassesRuntime, true};

    assemblyOptions.includeOutlinedCode = true;

    assemblyOptions.includeCfgInfo = Luau::CodeGen::IncludeCfgInfo::Yes;
    assemblyOptions.includeRegFlowInfo = Luau::CodeGen::IncludeRegFlowInfo::Yes;

    CHECK_EQ(
        "\n" + getCodegenAssembly(
                   R"(
local function foo(Class, x, y)
    local o = Class{x = x, y = y}
    return o
end
)",
                   true,
                   1,
                   2
               ),
        R"(
; function foo($arg0, $arg1, $arg2) line 2
; R3: any from 0 to 10
bb_bytecode_0:
; successors: bb_fallback_1, bb_2
; in regs: R0, R1, R2
; out regs: R3
  %0 = LOAD_TVALUE R0
  STORE_TVALUE R3, %0
  SET_SAVEDPC 3u
  CONSTRUCT R4, R3, 0u
  FALLBACK_SETTABLEKS 3u, R1, R4, K0 ('x')
  FALLBACK_SETTABLEKS 5u, R2, R4, K1 ('y')
  CHECK_TAG R4, tobject, bb_fallback_1
  %9 = LOAD_TVALUE R4, 0i, tobject
  STORE_TVALUE R3, %9
  JUMP bb_2
bb_2:
; predecessors: bb_bytecode_0, bb_fallback_1
; in regs: R3
  INTERRUPT 9u
  RETURN R3, 1i
bb_fallback_1:
; predecessors: bb_bytecode_0
; successors: bb_2
; in regs: R3, R4
; out regs: R3
  INTERRUPT 8u
  SET_SAVEDPC 9u
  CALL R3, 1i, 1i
  JUMP bb_2
)"
    );
}

TEST_SUITE_END();
