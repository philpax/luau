// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "Luau/CodeAllocator.h"
#include "Luau/CodeGen.h"
#include "Luau/IrRegAllocX64.h"

#include "../CodeGen/src/EmitCommonX64.h"

#include "ScopedFlags.h"
#include "doctest.h"

#include <cstring>

using namespace Luau::CodeGen;
using namespace Luau::CodeGen::X64;

LUAU_FASTFLAG(LuauCodegenX64IntSpillRestore)

class IrRegAllocX64Fixture
{
public:
    IrRegAllocX64Fixture()
        : build(&logger, ABIX64::Windows, /* features */ 0)
        , regs(&logger, build, function, nullptr)
    {
    }

    void checkMatch(std::string expected)
    {
        build.finalize();

        CHECK("\n" + logger.text == expected);
    }

    AssemblyOptions options;
    LogBuilder logger{options};
    AssemblyBuilderX64 build;
    IrFunction function;
    IrRegAllocX64 regs;
};

TEST_SUITE_BEGIN("IrRegAllocX64");

TEST_CASE_FIXTURE(IrRegAllocX64Fixture, "RelocateFix")
{
    IrInst irInst0{IrCmd::LOAD_DOUBLE};
    irInst0.lastUse = 2;
    function.instructions.push_back(irInst0);

    IrInst irInst1{IrCmd::LOAD_DOUBLE};
    irInst1.lastUse = 2;
    function.instructions.push_back(irInst1);

    function.instructions[0].regX64 = regs.takeReg(rax, 0);
    regs.preserve(function.instructions[0]);

    function.instructions[1].regX64 = regs.takeReg(rax, 1);
    regs.restore(function.instructions[0], true);

    LUAU_ASSERT(function.instructions[0].regX64 == rax);
    LUAU_ASSERT(function.instructions[1].spilled);

    checkMatch(R"(
 vmovsd      qword ptr [rsp+048h],rax
 vmovsd      qword ptr [rsp+050h],rax
 vmovsd      rax,qword ptr [rsp+048h]
)");
}

TEST_CASE_FIXTURE(IrRegAllocX64Fixture, "RestoreStackSpillIgnoresLaterConvertedLocation")
{
    ScopedFastFlag luauCodegenX64IntSpillRestore{FFlag::LuauCodegenX64IntSpillRestore, true};

    IrInst irInst0{IrCmd::BUFFER_READI32};
    irInst0.lastUse = 2;
    function.instructions.push_back(irInst0);

    IrInst irInst1{IrCmd::BUFFER_READI32};
    irInst1.lastUse = 2;
    function.instructions.push_back(irInst1);

    function.instructions[0].regX64 = regs.takeReg(eax, 0);
    regs.preserve(function.instructions[0]);
    function.recordRestoreLocation(0, {IrOp{IrOpKind::VmReg, 16}, IrValueKind::Double, IrCmd::UINT_TO_NUM, false});

    function.instructions[1].regX64 = regs.takeReg(eax, 1);
    regs.restore(function.instructions[0], true);

    LUAU_ASSERT(function.instructions[0].regX64 == eax);

    checkMatch(R"(
 mov         dword ptr [rsp+048h],eax
 mov         dword ptr [rsp+04Ch],eax
 mov         eax,dword ptr [rsp+048h]
)");
}

#if defined(CODEGEN_TARGET_X64) && LUA_VECTOR_SIZE == 4 && LUA_VECTOR_DOUBLE == 0

struct TValueRegAllocX64Fixture
{
    explicit TValueRegAllocX64Fixture(ABIX64 abi)
        : build(nullptr, abi, 0)
        , regs(nullptr, build, function, nullptr)
    {
        build.push(rBase);
        build.push(rConstants);
        build.sub(rsp, 128);
        build.mov64(rBase, int64_t(uintptr_t(vm)));
        build.mov64(rConstants, int64_t(uintptr_t(constants)));
        memset(&result, 0xa5, sizeof(result));
    }

    void addValue(IrCmd cmd, RegisterX64 reg)
    {
        uint32_t index = uint32_t(function.instructions.size());
        function.instructions.push_back(IrInst{cmd});
        function.instructions.back().lastUse = 100;
        function.instructions.back().regX64 = regs.takeReg(reg, index);
    }

    void recordRestore(IrOpKind source, bool lazy)
    {
        IrBlock block;
        block.start = 0;
        block.finish = uint32_t(function.instructions.size() - 1);
        function.blocks.push_back(block);
        function.validRestoreOpBlocks.push_back(0);
        function.recordRestoreLocation(0, {IrOp{source, 1}, IrValueKind::Tvalue, IrCmd::NOP, lazy});
    }

    void saveResult(RegisterX64 reg)
    {
        build.mov64(rax, int64_t(uintptr_t(&result)));
        storeTValue(build, xmmword[rax], reg, xmm5);
        getTValueTag(build, edx, reg, xmm5);
        build.mov64(rax, int64_t(uintptr_t(&extractedTag)));
        build.mov(dword[rax], edx);
    }

    void run()
    {
        build.add(rsp, 128);
        build.pop(rConstants);
        build.pop(rBase);
        build.ret();
        build.finalize();

        REQUIRE(isSupported());
        CodeAllocator allocator(1024 * 1024, 1024 * 1024);
        CodeAllocationData allocation = allocator.allocate(build.data.data(), build.data.size(), build.code.data(), build.code.size());
        REQUIRE(allocation.codeStart);
        reinterpret_cast<void (*)()>(allocation.codeStart)();
        allocator.deallocate(allocation);
    }

    void checkResult(const TValue& expected)
    {
        CHECK(memcmp(&result, &expected, 16) == 0);
        CHECK(result.tt == expected.tt);
        CHECK(extractedTag == expected.tt);
        if (expected.tt == LUA_TVECTOR)
        {
            float fourthComponent;
            memcpy(&fourthComponent, reinterpret_cast<const char*>(&result) + 3 * sizeof(float), sizeof(fourthComponent));
            CHECK(fourthComponent == -47.25f);
        }

        // The VM slot has padding after the tag, not another native register lane.
        const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&result);
        for (size_t i = offsetof(TValue, tt) + sizeof(result.tt); i < sizeof(result); ++i)
            CHECK(bytes[i] == 0xa5);
    }

    AssemblyBuilderX64 build;
    IrFunction function;
    IrRegAllocX64 regs;
    TValue input{};
    TValue vm[3]{};
    TValue constants[3]{};
    TValue result{};
    int extractedTag = -1;
};

static TValue makeTransferValue(int kind)
{
    TValue value;
    memset(&value, 0x5a, sizeof(value));
    if (kind == LUA_TVECTOR)
    {
        setvvalue(nullptr, &value, 1.5f, -2.25f, 17.0f, -47.25f);
    }
    else if (kind == LUA_TQUATERNION)
    {
        setqvalue(&value, 0.5f, -0.25f, 0.125f, -0.75f);
    }
    else
    {
        setnvalue(&value, -1234.5);
    }
    return value;
}

TEST_CASE("TValueMixedIndirectSpillReload")
{
    for (ABIX64 abi : {ABIX64::Windows, ABIX64::SystemV})
    {
        for (int kind : {LUA_TNUMBER, LUA_TVECTOR, LUA_TQUATERNION})
        {
            CAPTURE(int(abi));
            CAPTURE(kind);
            TValueRegAllocX64Fixture fixture(abi);
            auto& build = fixture.build;
            auto& regs = fixture.regs;
            auto& instructions = fixture.function.instructions;
            fixture.input = makeTransferValue(kind);
            double number = 19.75;
            double restoredNumber = 0;
            int restoredInt = 0;

            fixture.function.instructions.push_back(IrInst{IrCmd::LOAD_POINTER});
            instructions[0].regX64 = rax;
            fixture.addValue(IrCmd::LOAD_DOUBLE, xmm1);
            build.vmovsd(xmm1, build.f64(number));
            regs.preserve(instructions[1]);

            // An indirect load has no VM register or constant from which to rematerialize.
            fixture.addValue(IrCmd::LOAD_TVALUE, xmm0);
            getOp(instructions[2], 0) = IrOp{IrOpKind::Inst, 0};
            build.mov64(rax, int64_t(uintptr_t(&fixture.input)));
            loadTValue(build, xmm0, xmmword[rax], xmm5);
            CHECK_FALSE(fixture.function.hasRestoreLocation(instructions[2], true));
            regs.preserve(instructions[2]);

            fixture.addValue(IrCmd::BUFFER_READI32, ecx);
            build.mov(ecx, 0x12345678);
            regs.preserve(instructions[3]);

            REQUIRE(regs.spills.size() == 3);
            CHECK(regs.spills[0].valueKind == IrValueKind::Double);
            CHECK(regs.spills[0].stackSlot == 0);
            CHECK(regs.spills[1].valueKind == IrValueKind::Tvalue);
            CHECK(regs.spills[1].stackSlot == 2);
            CHECK(regs.spills[2].valueKind == IrValueKind::Int);
            CHECK(regs.spills[2].stackSlot == 10);
            CHECK(regs.usedSpillSlotHalfs.count() == 11);
            CHECK(regs.maxUsedSlot == 6);
            CHECK(instructions[2].spilled);
            CHECK_FALSE(instructions[2].needsReload);

            build.vxorps(ymm0, ymm0, ymm0);
            build.vxorps(xmm1, xmm1, xmm1);
            build.mov(ecx, 0);
            regs.restore(instructions[2], true);
            regs.restore(instructions[1], true);
            regs.restore(instructions[3], true);
            CHECK(regs.spills.empty());
            CHECK(regs.usedSpillSlotHalfs.none());
            CHECK_FALSE(instructions[2].spilled);
            CHECK(instructions[2].regX64 == xmm0);

            fixture.saveResult(xmm0);
            build.mov64(rax, int64_t(uintptr_t(&restoredNumber)));
            build.vmovsd(qword[rax], xmm1);
            build.mov64(rax, int64_t(uintptr_t(&restoredInt)));
            build.mov(dword[rax], ecx);
            fixture.run();
            fixture.checkResult(fixture.input);
            CHECK(restoredNumber == number);
            CHECK(restoredInt == 0x12345678);
        }
    }
}

TEST_CASE("TValueLazyMaterializationAndRematerialization")
{
    for (ABIX64 abi : {ABIX64::Windows, ABIX64::SystemV})
    {
        for (int kind : {LUA_TNUMBER, LUA_TVECTOR, LUA_TQUATERNION})
        {
            CAPTURE(int(abi));
            CAPTURE(kind);
            TValueRegAllocX64Fixture fixture(abi);
            fixture.input = makeTransferValue(kind);
            memset(&fixture.vm[1], 0xa5, sizeof(TValue));
            fixture.addValue(IrCmd::LOAD_TVALUE, xmm0);
            fixture.recordRestore(IrOpKind::VmReg, true);
            fixture.build.mov64(rax, int64_t(uintptr_t(&fixture.input)));
            loadTValue(fixture.build, xmm0, xmmword[rax], xmm5);
            size_t beforePreserve = fixture.build.getCodeSize();
            fixture.regs.preserve(fixture.function.instructions[0]);

            REQUIRE(fixture.regs.spills.size() == 1);
            CHECK(fixture.regs.spills[0].stackSlot == kNoStackSlot);
            CHECK(fixture.regs.usedSpillSlotHalfs.none());
            CHECK_FALSE(fixture.function.instructions[0].spilled);
            CHECK(fixture.function.instructions[0].needsReload);
            CHECK_FALSE(fixture.function.findRestoreLocation(0, true).lazy);
            CHECK(fixture.build.getCodeSize() > beforePreserve);

            fixture.build.vxorps(ymm0, ymm0, ymm0);
            fixture.regs.restore(fixture.function.instructions[0], true);
            CHECK(fixture.regs.spills.empty());
            CHECK_FALSE(fixture.function.instructions[0].needsReload);
            fixture.saveResult(xmm0);
            fixture.run();
            fixture.checkResult(fixture.input);
            CHECK(memcmp(&fixture.vm[1], &fixture.input, 16) == 0);
            CHECK(fixture.vm[1].tt == kind);
            const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&fixture.vm[1]);
            for (size_t i = offsetof(TValue, tt) + sizeof(int); i < sizeof(TValue); ++i)
                CHECK(bytes[i] == 0xa5);
            CHECK(fixture.vm[2].tt == LUA_TNIL);
        }
    }
}

TEST_CASE("TValueVmRegAndVmConstRematerialization")
{
    for (ABIX64 abi : {ABIX64::Windows, ABIX64::SystemV})
    {
        for (IrOpKind source : {IrOpKind::VmReg, IrOpKind::VmConst})
        {
            for (int kind : {LUA_TNUMBER, LUA_TVECTOR, LUA_TQUATERNION})
            {
                CAPTURE(int(abi));
                CAPTURE(int(source));
                CAPTURE(kind);
                TValueRegAllocX64Fixture fixture(abi);
                TValue expected = makeTransferValue(kind);
                if (source == IrOpKind::VmReg)
                    fixture.vm[1] = expected;
                else
                    fixture.constants[1] = expected;
                fixture.addValue(IrCmd::LOAD_TVALUE, xmm0);
                fixture.recordRestore(source, false);
                loadTValue(fixture.build, xmm0, source == IrOpKind::VmReg ? luauReg(1) : luauConstant(1), xmm5);
                size_t beforePreserve = fixture.build.getCodeSize();
                fixture.regs.preserve(fixture.function.instructions[0]);

                REQUIRE(fixture.regs.spills.size() == 1);
                CHECK(fixture.regs.spills[0].stackSlot == kNoStackSlot);
                CHECK(fixture.regs.usedSpillSlotHalfs.none());
                CHECK_FALSE(fixture.function.instructions[0].spilled);
                CHECK(fixture.function.instructions[0].needsReload);
                CHECK(fixture.build.getCodeSize() == beforePreserve);

                fixture.build.vxorps(ymm0, ymm0, ymm0);
                fixture.regs.restore(fixture.function.instructions[0], true);
                CHECK(fixture.regs.spills.empty());
                CHECK_FALSE(fixture.function.instructions[0].needsReload);
                fixture.saveResult(xmm0);
                fixture.run();
                fixture.checkResult(expected);
            }
        }
    }
}

#endif

TEST_SUITE_END();
