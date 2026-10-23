// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "CodeGenContext.h"

#include "CodeGenA64.h"
#include "CodeGenLower.h"
#include "CodeGenX64.h"

#include "Luau/CodeGenCommon.h"
#include "Luau/CodeBlockUnwind.h"
#include "Luau/UnwindBuilder.h"
#include "Luau/UnwindBuilderDwarf2.h"
#include "Luau/UnwindBuilderWin.h"

#include "lapi.h"
#include "lfunc.h"
#include "lgc.h"
#include "lstate.h"

#include <algorithm>

#include <stdio.h>

LUAU_FASTINTVARIABLE(LuauCodeGenBlockSize, 4 * 1024 * 1024)
LUAU_FASTINTVARIABLE(LuauCodeGenMaxTotalSize, 256 * 1024 * 1024)

namespace Luau
{
namespace CodeGen
{

// PCG32 PRNG helpers for JIT layout randomization.
// Uses the same algorithm and constants as the Lua VM (lmathlib.cpp) for consistency.
uint64_t jitRngSeed(uintptr_t ptr)
{
    uint64_t state = 0;
    state = state * 6364136223846793005ULL + (105 | 1);
    state += uint64_t(ptr);
    state = state * 6364136223846793005ULL + (105 | 1);
    return state;
}

uint32_t jitRngRandom(uint64_t& state)
{
    uint64_t oldstate = state;
    state = oldstate * 6364136223846793005ULL + (105 | 1);
    uint32_t xorshifted = uint32_t(((oldstate >> 18u) ^ oldstate) >> 27u);
    uint32_t rot = uint32_t(oldstate >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((-int32_t(rot)) & 31));
}


static const Instruction kCodeEntryInsn = LOP_NATIVECALL;

// From CodeGen.cpp
static void* gPerfLogContext = nullptr;
static PerfLogFn gPerfLogFn = nullptr;

unsigned int getCpuFeaturesA64();
unsigned int getCpuFeaturesX64();

void setPerfLog(void* context, PerfLogFn logFn)
{
    gPerfLogContext = context;
    gPerfLogFn = logFn;
}

static void logPerfFunction(Proto* p, uintptr_t addr, unsigned size)
{
    CODEGEN_ASSERT(p->source);

    const char* source = getstr(p->source);
    source = (source[0] == '=' || source[0] == '@') ? source + 1 : "[string]";

    char name[256];
    snprintf(name, sizeof(name), "<luau> %s:%d %s", source, p->linedefined, p->debugname ? getstr(p->debugname) : "");

    if (gPerfLogFn)
        gPerfLogFn(gPerfLogContext, addr, size, name);
}

static void logPerfFunctions(
    const std::vector<Proto*>& moduleProtos,
    const uint8_t* nativeModuleBaseAddress,
    const std::vector<NativeProtoExecDataPtr>& nativeProtos
)
{
    if (gPerfLogFn == nullptr)
        return;

    if (nativeProtos.size() > 0)
        gPerfLogFn(
            gPerfLogContext,
            uintptr_t(nativeModuleBaseAddress),
            unsigned(getNativeProtoExecDataHeader(nativeProtos[0].get()).entryOffsetOrAddress - nativeModuleBaseAddress),
            "<luau helpers>"
        );

    auto protoIt = moduleProtos.begin();

    for (const NativeProtoExecDataPtr& nativeProto : nativeProtos)
    {
        const NativeProtoExecDataHeader& header = getNativeProtoExecDataHeader(nativeProto.get());

        while (protoIt != moduleProtos.end() && uint32_t((**protoIt).bytecodeid) != header.bytecodeId)
        {
            ++protoIt;
        }

        CODEGEN_ASSERT(protoIt != moduleProtos.end());

        logPerfFunction(*protoIt, uintptr_t(header.entryOffsetOrAddress), uint32_t(header.nativeCodeSize));
    }
}

// If Release is true, the native proto will be removed from the vector and
// ownership will be assigned to the Proto object (for use with the
// StandaloneCodeContext).  If Release is false, the native proto will not be
// removed from the vector (for use with the SharedCodeContext).
template<bool Release, typename NativeProtosVector>
[[nodiscard]] static uint32_t bindNativeProtos(const std::vector<Proto*>& moduleProtos, NativeProtosVector& nativeProtos)
{
    uint32_t protosBound = 0;

    auto protoIt = moduleProtos.begin();

    for (auto& nativeProto : nativeProtos)
    {
        const NativeProtoExecDataHeader& header = getNativeProtoExecDataHeader(nativeProto.get());

        while (protoIt != moduleProtos.end() && uint32_t((**protoIt).bytecodeid) != header.bytecodeId)
        {
            ++protoIt;
        }

        CODEGEN_ASSERT(protoIt != moduleProtos.end());

        // The NativeProtoExecData is now owned by the VM and will be destroyed
        // via onDestroyFunction.
        Proto* proto = *protoIt;

        if constexpr (Release)
        {
            proto->execdata = nativeProto.release();
        }
        else
        {
            proto->execdata = nativeProto.get();
        }

        proto->exectarget = reinterpret_cast<uintptr_t>(header.entryOffsetOrAddress);
        proto->codeentry = &kCodeEntryInsn;

        // A queued proto keeps its queue entry until promotion takes it, which then skips it as compiled.
        if (proto->hotcount > 0)
            proto->hotcount = 0;

        ++protosBound;
    }

    return protosBound;
}

BaseCodeGenContext::BaseCodeGenContext(size_t blockSize, size_t maxTotalSize, AllocationCallback* allocationCallback, void* allocationCallbackContext)
    : codeAllocator{blockSize, maxTotalSize, allocationCallback, allocationCallbackContext}
{
    CODEGEN_ASSERT(isSupported());

#if defined(_WIN32)
    unwindBuilder = std::make_unique<UnwindBuilderWin>();
#else
    unwindBuilder = std::make_unique<UnwindBuilderDwarf2>();
#endif

    codeAllocator.context = unwindBuilder.get();
    codeAllocator.createBlockUnwindInfo = createBlockUnwindInfo;
    codeAllocator.destroyBlockUnwindInfo = destroyBlockUnwindInfo;

    initFunctions(context);
}

BaseCodeGenContext::~BaseCodeGenContext()
{
    codeAllocator.deallocate(gateAllocationData);
}

[[nodiscard]] bool BaseCodeGenContext::initHeaderFunctions()
{
#if defined(CODEGEN_TARGET_X64)
    if (!X64::initHeaderFunctions(*this))
        return false;
#elif defined(CODEGEN_TARGET_A64)
    if (!A64::initHeaderFunctions(*this))
        return false;
#endif

    if (gPerfLogFn)
        gPerfLogFn(gPerfLogContext, uintptr_t(context.gateEntry), 4096, "<luau gate>");

    return true;
}


StandaloneCodeGenContext::StandaloneCodeGenContext(
    size_t blockSize,
    size_t maxTotalSize,
    AllocationCallback* allocationCallback,
    void* allocationCallbackContext
)
    : BaseCodeGenContext{blockSize, maxTotalSize, allocationCallback, allocationCallbackContext}
    , sharedAllocator{&codeAllocator}
{
}

[[nodiscard]] std::optional<ModuleBindResult> StandaloneCodeGenContext::tryBindExistingModule(const ModuleId&, const std::vector<Proto*>&)
{
    // The StandaloneCodeGenContext does not support sharing of native code
    return {};
}

[[nodiscard]] ModuleBindResult StandaloneCodeGenContext::bindModule(
    const std::optional<ModuleId>&,
    const std::vector<Proto*>& moduleProtos,
    std::vector<NativeProtoExecDataPtr> nativeProtos,
    const uint8_t* data,
    size_t dataSize,
    const uint8_t* code,
    size_t codeSize
)
{
    NativeModuleRef moduleRef = sharedAllocator.insertAnonymousNativeModule(std::move(nativeProtos), data, dataSize, code, codeSize);

    // If we did not get a NativeModule back, allocation failed:
    if (moduleRef.empty())
        return {CodeGenCompilationResult::AllocationFailed};

    logPerfFunctions(moduleProtos, moduleRef->getModuleBaseAddress(), moduleRef->getNativeProtos());

    // Bind the native protos and acquire an owning reference for each:
    const uint32_t protosBound = bindNativeProtos<false>(moduleProtos, moduleRef->getNativeProtos());
    moduleRef->addRefs(protosBound);

    return {CodeGenCompilationResult::Success, protosBound};
}

void StandaloneCodeGenContext::onCloseState() noexcept
{
    // The StandaloneCodeGenContext is owned by the one VM that owns it, so when
    // that VM is destroyed, we destroy *this as well:
    delete this;
}

void StandaloneCodeGenContext::onDestroyFunction(void* execdata) noexcept
{
    getNativeProtoExecDataHeader(static_cast<const uint32_t*>(execdata)).nativeModule->release();
}


SharedCodeGenContext::SharedCodeGenContext(
    size_t blockSize,
    size_t maxTotalSize,
    AllocationCallback* allocationCallback,
    void* allocationCallbackContext
)
    : BaseCodeGenContext{blockSize, maxTotalSize, allocationCallback, allocationCallbackContext}
    , sharedAllocator{&codeAllocator}
{
}

[[nodiscard]] std::optional<ModuleBindResult> SharedCodeGenContext::tryBindExistingModule(
    const ModuleId& moduleId,
    const std::vector<Proto*>& moduleProtos
)
{
    NativeModuleRef nativeModule = sharedAllocator.tryGetNativeModule(moduleId);
    if (nativeModule.empty())
    {
        return {};
    }

    // Bind the native protos and acquire an owning reference for each:
    const uint32_t protosBound = bindNativeProtos<false>(moduleProtos, nativeModule->getNativeProtos());
    nativeModule->addRefs(protosBound);

    return {{CodeGenCompilationResult::Success, protosBound}};
}

[[nodiscard]] ModuleBindResult SharedCodeGenContext::bindModule(
    const std::optional<ModuleId>& moduleId,
    const std::vector<Proto*>& moduleProtos,
    std::vector<NativeProtoExecDataPtr> nativeProtos,
    const uint8_t* data,
    size_t dataSize,
    const uint8_t* code,
    size_t codeSize
)
{
    const std::pair<NativeModuleRef, bool> insertionResult = [&]() -> std::pair<NativeModuleRef, bool>
    {
        if (moduleId.has_value())
        {
            return sharedAllocator.getOrInsertNativeModule(*moduleId, std::move(nativeProtos), data, dataSize, code, codeSize);
        }
        else
        {
            return {sharedAllocator.insertAnonymousNativeModule(std::move(nativeProtos), data, dataSize, code, codeSize), true};
        }
    }();

    // If we did not get a NativeModule back, allocation failed:
    if (insertionResult.first.empty())
        return {CodeGenCompilationResult::AllocationFailed};

    // If we allocated a new module, log the function code ranges for perf:
    if (insertionResult.second)
        logPerfFunctions(moduleProtos, insertionResult.first->getModuleBaseAddress(), insertionResult.first->getNativeProtos());

    // Bind the native protos and acquire an owning reference for each:
    const uint32_t protosBound = bindNativeProtos<false>(moduleProtos, insertionResult.first->getNativeProtos());
    insertionResult.first->addRefs(protosBound);

    return {CodeGenCompilationResult::Success, protosBound};
}

void SharedCodeGenContext::onCloseState() noexcept
{
    // The lifetime of the SharedCodeGenContext is managed separately from the
    // VMs that use it.  When a VM is destroyed, we don't need to do anything
    // here.
}

void SharedCodeGenContext::onDestroyFunction(void* execdata) noexcept
{
    getNativeProtoExecDataHeader(static_cast<const uint32_t*>(execdata)).nativeModule->release();
}


[[nodiscard]] UniqueSharedCodeGenContext createSharedCodeGenContext()
{
    return createSharedCodeGenContext(size_t(FInt::LuauCodeGenBlockSize), size_t(FInt::LuauCodeGenMaxTotalSize), nullptr, nullptr);
}

[[nodiscard]] UniqueSharedCodeGenContext createSharedCodeGenContext(AllocationCallback* allocationCallback, void* allocationCallbackContext)
{
    return createSharedCodeGenContext(
        size_t(FInt::LuauCodeGenBlockSize), size_t(FInt::LuauCodeGenMaxTotalSize), allocationCallback, allocationCallbackContext
    );
}

[[nodiscard]] UniqueSharedCodeGenContext createSharedCodeGenContext(
    size_t blockSize,
    size_t maxTotalSize,
    AllocationCallback* allocationCallback,
    void* allocationCallbackContext
)
{
    UniqueSharedCodeGenContext codeGenContext{new SharedCodeGenContext{blockSize, maxTotalSize, nullptr, nullptr}};

    if (!codeGenContext->initHeaderFunctions())
        return {};

    return codeGenContext;
}

void destroySharedCodeGenContext(const SharedCodeGenContext* codeGenContext) noexcept
{
    delete codeGenContext;
}

void SharedCodeGenContextDeleter::operator()(const SharedCodeGenContext* codeGenContext) const noexcept
{
    destroySharedCodeGenContext(codeGenContext);
}


[[nodiscard]] static BaseCodeGenContext* getCodeGenContext(lua_State* L) noexcept
{
    return static_cast<BaseCodeGenContext*>(L->global->ecb.context);
}

static void onCloseState(lua_State* L)
{
    getCodeGenContext(L)->onCloseState();
    L->global->ecb = lua_ExecutionCallbacks{};
}

static void onDestroyFunction(lua_State* L, Proto* proto)
{
    getCodeGenContext(L)->onDestroyFunction(proto->execdata);
    proto->execdata = nullptr;
    proto->exectarget = 0;
    proto->codeentry = proto->code;
}

static int onEnter(lua_State* L, Proto* proto)
{
    BaseCodeGenContext* codeGenContext = getCodeGenContext(L);

    CODEGEN_ASSERT(proto->execdata);
    CODEGEN_ASSERT(L->ci->savedpc >= proto->code && L->ci->savedpc < proto->code + proto->sizecode);

    uintptr_t target = proto->exectarget + static_cast<uint32_t*>(proto->execdata)[L->ci->savedpc - proto->code];

    // Returns 1 to finish the function in the VM
    return GateFn(codeGenContext->context.gateEntry)(L, proto, target, &codeGenContext->context);
}

static int onEnterDisabled(lua_State* L, Proto* proto)
{
    // If the function wasn't entered natively, it cannot be resumed natively later
    L->ci->flags &= ~LUA_CALLINFO_NATIVE;

    return 1;
}

// Defined in CodeGen.cpp
void onDisable(lua_State* L, Proto* proto);

static size_t getMemorySize(lua_State* L, Proto* proto)
{
    const NativeProtoExecDataHeader& execDataHeader = getNativeProtoExecDataHeader(static_cast<const uint32_t*>(proto->execdata));

    const size_t execDataSize = sizeof(NativeProtoExecDataHeader) + execDataHeader.bytecodeInstructionCount * sizeof(Instruction);

    // While execDataSize is exactly the size of the allocation we made and hold for 'execdata' field, the code size is approximate
    // This is because code+data page is shared and owned by all Proto from a single module and each one can keep the whole region alive
    // So individual Proto being freed by GC will not reflect memory use by native code correctly
    return execDataSize + execDataHeader.nativeCodeSize;
}

static char* getCounterData(lua_State* L, Proto* proto, size_t* count)
{
    CODEGEN_ASSERT(count != nullptr);

    const NativeProtoExecDataHeader& execDataHeader = getNativeProtoExecDataHeader(static_cast<const uint32_t*>(proto->execdata));

    *count = execDataHeader.extraDataCount / 4;
    return reinterpret_cast<char*>(static_cast<uint32_t*>(proto->execdata) + proto->sizecode);
}

static void initializeExecutionCallbacks(lua_State* L, BaseCodeGenContext* codeGenContext) noexcept
{
    CODEGEN_ASSERT(codeGenContext != nullptr);

    lua_ExecutionCallbacks* ecb = &L->global->ecb;

    ecb->context = codeGenContext;
    ecb->close = onCloseState;
    ecb->destroy = onDestroyFunction;
    ecb->enter = onEnter;
    ecb->disable = onDisable;
    ecb->getmemorysize = getMemorySize;
    ecb->getcounterdata = getCounterData;
}

void create(lua_State* L)
{
    return create(L, size_t(FInt::LuauCodeGenBlockSize), size_t(FInt::LuauCodeGenMaxTotalSize), nullptr, nullptr);
}

void create(lua_State* L, AllocationCallback* allocationCallback, void* allocationCallbackContext)
{
    return create(L, size_t(FInt::LuauCodeGenBlockSize), size_t(FInt::LuauCodeGenMaxTotalSize), allocationCallback, allocationCallbackContext);
}

void create(lua_State* L, size_t blockSize, size_t maxTotalSize, AllocationCallback* allocationCallback, void* allocationCallbackContext)
{
    std::unique_ptr<StandaloneCodeGenContext> codeGenContext =
        std::make_unique<StandaloneCodeGenContext>(blockSize, maxTotalSize, allocationCallback, allocationCallbackContext);

    if (!codeGenContext->initHeaderFunctions())
        return;

    initializeExecutionCallbacks(L, codeGenContext.release());
}

void create(lua_State* L, SharedCodeGenContext* codeGenContext)
{
    initializeExecutionCallbacks(L, codeGenContext);
}

[[nodiscard]] static NativeProtoExecDataPtr createNativeProtoExecData(Proto* proto, const IrBuilder& ir)
{
    uint32_t extraDataCount = uint32_t(ir.function.extraNativeData.size());

    NativeProtoExecDataPtr nativeExecData = createNativeProtoExecData(proto->sizecode, extraDataCount);

    uint32_t instTarget = ir.function.entryLocation;
    uint32_t unassignedOffset = ir.function.endLocation - instTarget;

    for (int i = 0; i < proto->sizecode; ++i)
    {
        const BytecodeMapping& bcMapping = ir.function.bcMapping[i];

        CODEGEN_ASSERT(bcMapping.asmLocation >= instTarget);

        if (bcMapping.asmLocation != ~0u)
            nativeExecData[i] = bcMapping.asmLocation - instTarget;
        else
            nativeExecData[i] = unassignedOffset;
    }

    // After the instruction offsets, custom native data is placed
    for (uint32_t i = 0; i < extraDataCount; i++)
        nativeExecData[proto->sizecode + i] = ir.function.extraNativeData[i];

    // Set first instruction offset to 0 so that entering this function still
    // executes any generated entry code.
    nativeExecData[0] = 0;

    NativeProtoExecDataHeader& header = getNativeProtoExecDataHeader(nativeExecData.get());
    header.entryOffsetOrAddress = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(instTarget));
    header.bytecodeId = uint32_t(proto->bytecodeid);
    header.bytecodeInstructionCount = proto->sizecode;
    header.extraDataCount = extraDataCount;

    return nativeExecData;
}

template<typename AssemblyBuilder>
[[nodiscard]] static NativeProtoExecDataPtr createNativeFunction(
    LogBuilder* logger,
    AssemblyBuilder& build,
    ModuleHelpers& helpers,
    Proto* proto,
    uint32_t& totalIrInstCount,
    const CompilationOptions& options,
    CodeGenCompilationResult& result,
    const VmEnvironmentInfo& envInfo
)
{
    IrBuilder ir(options.hooks, envInfo);
    ir.buildFunctionIr(proto);

    unsigned instCount = unsigned(ir.function.instructions.size());

    if (totalIrInstCount + instCount >= unsigned(FInt::CodegenHeuristicsInstructionLimit.value))
    {
        result = CodeGenCompilationResult::CodeGenOverflowInstructionLimit;
        return {};
    }

    totalIrInstCount += instCount;

    AssemblyOptions assemblyOptions;
    assemblyOptions.compilationOptions = options;

    if (!lowerFunction(ir, logger, build, helpers, proto, assemblyOptions, /* stats */ nullptr, result))
    {
        return {};
    }

    return createNativeProtoExecData(proto, ir);
}

[[nodiscard]] static CompilationResult compileProtos(
    const std::optional<ModuleId>& moduleId,
    lua_State* L,
    BaseCodeGenContext* codeGenContext,
    const std::vector<Proto*>& protos,
    const CompilationOptions& options,
    CompilationStats* stats
);

[[nodiscard]] static CompilationResult compileInternal(
    const std::optional<ModuleId>& moduleId,
    lua_State* L,
    int idx,
    const CompilationOptions& options,
    CompilationStats* stats
)
{
    CODEGEN_ASSERT(lua_isLfunction(L, idx));
    const TValue* func = luaA_toobject(L, idx);

    Proto* root = clvalue(func)->l.p;

    if ((options.flags & CodeGen_OnlyNativeModules) != 0 && (root->flags & LPF_NATIVE_MODULE) == 0 && (root->flags & LPF_NATIVE_FUNCTION) == 0)
        return CompilationResult{CodeGenCompilationResult::NotNativeModule};

    BaseCodeGenContext* codeGenContext = getCodeGenContext(L);
    if (codeGenContext == nullptr)
        return CompilationResult{CodeGenCompilationResult::CodeGenNotInitialized};

    std::vector<Proto*> protos;
    gatherFunctions(protos, root, options.flags, root->flags & LPF_NATIVE_FUNCTION);

    // Skip protos that have been compiled during previous invocations of CodeGen::compile
    protos.erase(
        std::remove_if(
            protos.begin(),
            protos.end(),
            [](Proto* p)
            {
                return p == nullptr || p->execdata != nullptr;
            }
        ),
        protos.end()
    );

    if (protos.empty())
        return CompilationResult{CodeGenCompilationResult::NothingToCompile};

    return compileProtos(moduleId, L, codeGenContext, protos, options, stats);
}

// One native module's generated code, built from protos whose bytecode ids are unique and ascending, because the module sorts and binds its
// functions by bytecode id. Building reads only the sources, so it can run away from the VM thread when they are snapshots.
struct CompiledModule
{
    // The protos the module binds to, in the order of the sources it was built from.
    std::vector<Proto*> targets;
    std::vector<Proto*> sources;
    std::vector<NativeProtoExecDataPtr> nativeProtos;
    std::vector<uint8_t> code;
    std::vector<uint8_t> data;
    CompilationResult result;
    CompilationStats stats;
};

static void compileModule(CompiledModule& module, const CompilationOptions& options, const VmEnvironmentInfo& envInfo)
{
    module.stats.functionsTotal = uint32_t(module.sources.size());

#if defined(CODEGEN_TARGET_A64)
    static unsigned int cpuFeatures = getCpuFeaturesA64();
    A64::AssemblyBuilderA64 build(/* logger= */ nullptr, cpuFeatures);
#else
    static unsigned int cpuFeatures = getCpuFeaturesX64();
    X64::AssemblyBuilderX64 build(/* logger= */ nullptr, cpuFeatures);
#endif

    ModuleHelpers helpers;
#if defined(CODEGEN_TARGET_A64)
    A64::assembleHelpers(/* logger= */ nullptr, build, helpers);
#else
    X64::assembleHelpers(/* logger= */ nullptr, build, helpers);
#endif

    module.nativeProtos.reserve(module.sources.size());

    uint32_t totalIrInstCount = 0;

    for (Proto* source : module.sources)
    {
        CodeGenCompilationResult protoResult = CodeGenCompilationResult::Success;

        NativeProtoExecDataPtr nativeExecData =
            createNativeFunction(nullptr, build, helpers, source, totalIrInstCount, options, protoResult, envInfo);
        if (nativeExecData != nullptr)
        {
            module.nativeProtos.push_back(std::move(nativeExecData));
        }
        else
        {
            module.result.protoFailures.push_back({protoResult, source->debugname ? getstr(source->debugname) : "", source->linedefined});
        }
    }

    // Very large modules might result in overflowing a jump offset; in this
    // case we currently abandon the entire module
    if (!build.finalize())
    {
        module.nativeProtos.clear();
        module.result.result = CodeGenCompilationResult::CodeGenAssemblerFinalizationFailure;
        return;
    }

    // If no functions were assembled, we don't need to allocate/copy executable pages for helpers
    if (module.nativeProtos.empty())
        return;

    for (const NativeProtoExecDataPtr& nativeExecData : module.nativeProtos)
    {
        NativeProtoExecDataHeader& header = getNativeProtoExecDataHeader(nativeExecData.get());

        module.stats.bytecodeSizeBytes += header.bytecodeInstructionCount * sizeof(Instruction);

        // Account for the native -> bytecode instruction offsets mapping:
        module.stats.nativeMetadataSizeBytes += header.bytecodeInstructionCount * sizeof(uint32_t);
    }

    module.stats.functionsCompiled += uint32_t(module.nativeProtos.size());
    module.stats.nativeCodeSizeBytes += build.code.size() * sizeof(build.code[0]);
    module.stats.nativeDataSizeBytes += build.data.size();

    for (size_t i = 0; i < module.nativeProtos.size(); ++i)
    {
        NativeProtoExecDataHeader& header = getNativeProtoExecDataHeader(module.nativeProtos[i].get());

        uint32_t begin = uint32_t(reinterpret_cast<uintptr_t>(header.entryOffsetOrAddress));
        uint32_t end = i + 1 < module.nativeProtos.size()
                           ? uint32_t(uintptr_t(getNativeProtoExecDataHeader(module.nativeProtos[i + 1].get()).entryOffsetOrAddress))
                           : uint32_t(build.code.size() * sizeof(build.code[0]));

        CODEGEN_ASSERT(begin < end);

        header.nativeCodeSize = end - begin;
    }

    const uint8_t* code = reinterpret_cast<const uint8_t*>(build.code.data());
    module.code.assign(code, code + build.code.size() * sizeof(build.code[0]));
    const uint8_t* data = reinterpret_cast<const uint8_t*>(build.data.data());
    module.data.assign(data, data + build.data.size());
}

// Installs a module's code into its targets. A target compiled through another path, or given a breakpoint, while the module was built
// keeps its current state, and its generated function is dropped.
static CompilationResult bindCompiledModule(const std::optional<ModuleId>& moduleId, BaseCodeGenContext* codeGenContext, CompiledModule& module)
{
    CompilationResult result = std::move(module.result);
    if (module.nativeProtos.empty())
        return result;

    std::vector<Proto*> targets;
    std::vector<NativeProtoExecDataPtr> nativeProtos;
    for (NativeProtoExecDataPtr& nativeProto : module.nativeProtos)
    {
        uint32_t bytecodeId = getNativeProtoExecDataHeader(nativeProto.get()).bytecodeId;
        auto target = std::find_if(
            module.targets.begin(),
            module.targets.end(),
            [bytecodeId](const Proto* proto)
            {
                return uint32_t(proto->bytecodeid) == bytecodeId;
            }
        );
        CODEGEN_ASSERT(target != module.targets.end());

        if ((*target)->execdata == nullptr && (*target)->debuginsn == nullptr)
        {
            targets.push_back(*target);
            nativeProtos.push_back(std::move(nativeProto));
        }
    }
    module.nativeProtos.clear();

    if (nativeProtos.empty())
        return result;

    const ModuleBindResult bindResult = codeGenContext->bindModule(
        moduleId, targets, std::move(nativeProtos), module.data.data(), module.data.size(), module.code.data(), module.code.size()
    );

    module.stats.functionsBound = bindResult.functionsBound;

    if (bindResult.compilationResult != CodeGenCompilationResult::Success)
        result.result = bindResult.compilationResult;

    return result;
}

static VmEnvironmentInfo getVmEnvironmentInfo(lua_State* L)
{
    VmEnvironmentInfo envInfo;
    envInfo.hasPcall = L->global->builtinPcall != nullptr;
    envInfo.hasXpcall = L->global->builtinXpcall != nullptr;
    return envInfo;
}

// Compiles uncompiled protos into one native module on the VM thread. Their bytecode ids must be unique and ascending.
[[nodiscard]] static CompilationResult compileProtos(
    const std::optional<ModuleId>& moduleId,
    lua_State* L,
    BaseCodeGenContext* codeGenContext,
    const std::vector<Proto*>& protos,
    const CompilationOptions& options,
    CompilationStats* stats
)
{
    if (stats != nullptr)
        stats->functionsTotal = uint32_t(protos.size());

    if (moduleId.has_value())
    {
        if (std::optional<ModuleBindResult> existingModuleBindResult = codeGenContext->tryBindExistingModule(*moduleId, protos))
        {
            if (stats != nullptr)
                stats->functionsBound = existingModuleBindResult->functionsBound;

            return CompilationResult{existingModuleBindResult->compilationResult};
        }
    }

    CompiledModule module;
    module.targets = protos;
    module.sources = protos;
    compileModule(module, options, getVmEnvironmentInfo(L));
    CompilationResult result = bindCompiledModule(moduleId, codeGenContext, module);

    if (stats != nullptr)
        *stats = module.stats;

    return result;
}

static void addStats(CompilationStats& total, const CompilationStats& stats)
{
    total.bytecodeSizeBytes += stats.bytecodeSizeBytes;
    total.nativeCodeSizeBytes += stats.nativeCodeSizeBytes;
    total.nativeDataSizeBytes += stats.nativeDataSizeBytes;
    total.nativeMetadataSizeBytes += stats.nativeMetadataSizeBytes;
    total.functionsTotal += stats.functionsTotal;
    total.functionsCompiled += stats.functionsCompiled;
    total.functionsBound += stats.functionsBound;
}

void setHotThreshold(lua_State* L, int threshold)
{
    CODEGEN_ASSERT(threshold >= 0);
    L->global->hotthreshold = threshold;
}

void setHotQueueCapacity(lua_State* L, int capacity)
{
    luaF_sethotqueuecapacity(L, capacity);
}

int getHotQueueSize(lua_State* L)
{
    return L->global->hotqueuesize;
}

// A promotion batch compiled away from the VM thread. Its modules build from shallow proto copies whose bytecode is snapshotted at take,
// because the interpreter patches instructions while it runs; the originals stay pinned until the batch binds or is discarded.
struct HotBatch
{
    std::vector<CompiledModule> modules;
    std::vector<std::vector<Instruction>> code;
    std::vector<Proto> copies;
    VmEnvironmentInfo envInfo;
};

HotBatch* takeHot(lua_State* L, size_t instructionBudget)
{
    if (getCodeGenContext(L) == nullptr)
        return nullptr;

    global_State* g = L->global;
    std::vector<Proto*> taken;
    size_t instructions = 0;

    // The first uncompiled proto is always taken, so a proto larger than the budget still promotes.
    while (g->hotqueuesize > 0 && (taken.empty() || instructions + size_t(g->hotqueue[0]->sizecode) <= instructionBudget))
    {
        Proto* proto = nullptr;
        luaF_takehot(L, &proto, 1);

        // A proto the collector has found unreachable may wait in the weak queue until the sweep frees it, so it is not pinned.
        if (proto->execdata != nullptr || isdead(g, obj2gco(proto)))
            continue;

        instructions += size_t(proto->sizecode);
        taken.push_back(proto);
    }

    if (taken.empty())
        return nullptr;

    // Reserving first means a memory error leaves no proto pinned without a batch to release it.
    luaF_reservepins(L, int(taken.size()));
    std::unique_ptr<HotBatch> batch = std::make_unique<HotBatch>();
    batch->envInfo = getVmEnvironmentInfo(L);
    batch->code.reserve(taken.size());
    // The copies vector never reallocates after this, because sources point into it.
    batch->copies.reserve(taken.size());

    // Bytecode ids are unique only within one loaded chunk, so protos from different chunks can collide.
    for (Proto* proto : taken)
    {
        auto module = std::find_if(
            batch->modules.begin(),
            batch->modules.end(),
            [proto](const CompiledModule& members)
            {
                return std::none_of(
                    members.targets.begin(),
                    members.targets.end(),
                    [proto](const Proto* member)
                    {
                        return member->bytecodeid == proto->bytecodeid;
                    }
                );
            }
        );

        if (module == batch->modules.end())
            module = batch->modules.emplace(batch->modules.end());

        module->targets.push_back(proto);
    }

    for (CompiledModule& module : batch->modules)
    {
        std::sort(
            module.targets.begin(),
            module.targets.end(),
            [](const Proto* left, const Proto* right)
            {
                return left->bytecodeid < right->bytecodeid;
            }
        );

        for (Proto* target : module.targets)
        {
            luaF_pinhot(L, target);
            std::vector<Instruction>& code = batch->code.emplace_back(target->code, target->code + target->sizecode);
            Proto& copy = batch->copies.emplace_back(*target);
            copy.code = code.data();
            module.sources.push_back(&copy);
        }
    }

    return batch.release();
}

void compileHotBatch(HotBatch* batch)
{
    // Promotion has runtime evidence that a function is hot, so the compiler's cold-function heuristic and module markers do not apply.
    CompilationOptions options{CodeGen_ColdFunctions};

    for (CompiledModule& module : batch->modules)
        compileModule(module, options, batch->envInfo);
}

static void unpinBatch(lua_State* L, HotBatch* batch)
{
    for (CompiledModule& module : batch->modules)
    {
        for (Proto* target : module.targets)
            luaF_unpinhot(L, target);
    }
}

CompilationResult bindHotBatch(lua_State* L, HotBatch* batch, CompilationStats* stats)
{
    std::unique_ptr<HotBatch> owned(batch);
    BaseCodeGenContext* codeGenContext = getCodeGenContext(L);
    CODEGEN_ASSERT(codeGenContext != nullptr);

    CompilationResult combined;

    for (CompiledModule& module : owned->modules)
    {
        CompilationResult result = bindCompiledModule({}, codeGenContext, module);

        if (stats != nullptr)
            addStats(*stats, module.stats);

        if (result.result != CodeGenCompilationResult::Success && combined.result == CodeGenCompilationResult::Success)
            combined.result = result.result;

        combined.protoFailures.insert(combined.protoFailures.end(), result.protoFailures.begin(), result.protoFailures.end());
    }

    unpinBatch(L, owned.get());
    return combined;
}

void discardHotBatch(lua_State* L, HotBatch* batch)
{
    std::unique_ptr<HotBatch> owned(batch);
    unpinBatch(L, owned.get());
}

CompilationResult compileHot(lua_State* L, size_t instructionBudget, CompilationStats* stats)
{
    if (getCodeGenContext(L) == nullptr)
        return CompilationResult{CodeGenCompilationResult::CodeGenNotInitialized};

    HotBatch* batch = takeHot(L, instructionBudget);
    if (batch == nullptr)
        return CompilationResult{CodeGenCompilationResult::NothingToCompile};

    compileHotBatch(batch);
    return bindHotBatch(L, batch, stats);
}

CompilationResult compile(const ModuleId& moduleId, lua_State* L, int idx, const CompilationOptions& options, CompilationStats* stats)
{
    return compileInternal(moduleId, L, idx, options, stats);
}

CompilationResult compile(lua_State* L, int idx, const CompilationOptions& options, CompilationStats* stats)
{
    return compileInternal({}, L, idx, options, stats);
}

CompilationResult compile(lua_State* L, int idx, unsigned int flags, CompilationStats* stats)
{
    return compileInternal({}, L, idx, CompilationOptions{flags}, stats);
}

CompilationResult compile(const ModuleId& moduleId, lua_State* L, int idx, unsigned int flags, CompilationStats* stats)
{
    return compileInternal(moduleId, L, idx, CompilationOptions{flags}, stats);
}

[[nodiscard]] bool isNativeExecutionEnabled(lua_State* L)
{
    return getCodeGenContext(L) != nullptr && L->global->ecb.enter == onEnter;
}

void setNativeExecutionEnabled(lua_State* L, bool enabled)
{
    if (getCodeGenContext(L) != nullptr)
        L->global->ecb.enter = enabled ? onEnter : onEnterDisabled;
}

static uint8_t userdataRemapperWrap(lua_State* L, const char* str, size_t len)
{
    if (BaseCodeGenContext* codegenCtx = getCodeGenContext(L))
    {
        uint8_t index = codegenCtx->userdataRemapper(codegenCtx->userdataRemappingContext, str, len);

        if (index < (LBC_TYPE_TAGGED_USERDATA_END - LBC_TYPE_TAGGED_USERDATA_BASE))
            return LBC_TYPE_TAGGED_USERDATA_BASE + index;
    }

    return LBC_TYPE_USERDATA;
}

void setUserdataRemapper(lua_State* L, void* context, UserdataRemapperCallback cb)
{
    if (BaseCodeGenContext* codegenCtx = getCodeGenContext(L))
    {
        codegenCtx->userdataRemappingContext = context;
        codegenCtx->userdataRemapper = cb;

        L->global->ecb.gettypemapping = cb ? userdataRemapperWrap : nullptr;
    }
}

} // namespace CodeGen
} // namespace Luau
