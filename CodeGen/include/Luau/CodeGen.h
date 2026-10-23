// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#pragma once

#include "Luau/CodeGenCommon.h"
#include "Luau/CodeGenOptions.h"
#include "Luau/LoweringStats.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

#include <stddef.h>
#include <stdint.h>

struct lua_State;

namespace Luau
{
namespace CodeGen
{

// These enum values can be reported through telemetry.
// To ensure consistency, changes should be additive.
enum class CodeGenCompilationResult
{
    Success = 0,          // Successfully generated code for at least one function
    NothingToCompile = 1, // There were no new functions to compile
    NotNativeModule = 2,  // Module does not have `--!native` comment

    CodeGenNotInitialized = 3,                // Native codegen system is not initialized
    CodeGenOverflowInstructionLimit = 4,      // Instruction limit overflow
    CodeGenOverflowBlockLimit = 5,            // Block limit overflow
    CodeGenOverflowBlockInstructionLimit = 6, // Block instruction limit overflow
    CodeGenAssemblerFinalizationFailure = 7,  // Failure during assembler finalization
    CodeGenLoweringFailure = 8,               // Lowering failed
    AllocationFailed = 9,                     // Native codegen failed due to an allocation error

    Count = 10,
};

std::string toString(const CodeGenCompilationResult& result);

struct ProtoCompilationFailure
{
    CodeGenCompilationResult result = CodeGenCompilationResult::Success;

    std::string debugname;
    int line = -1;
};

struct CompilationResult
{
    CodeGenCompilationResult result = CodeGenCompilationResult::Success;

    std::vector<ProtoCompilationFailure> protoFailures;

    [[nodiscard]] bool hasErrors() const
    {
        return result != CodeGenCompilationResult::Success || !protoFailures.empty();
    }
};

struct CompilationStats
{
    size_t bytecodeSizeBytes = 0;
    size_t nativeCodeSizeBytes = 0;
    size_t nativeDataSizeBytes = 0;
    size_t nativeMetadataSizeBytes = 0;

    uint32_t functionsTotal = 0;
    uint32_t functionsCompiled = 0;
    uint32_t functionsBound = 0;
};

bool isSupported();

class SharedCodeGenContext;

struct SharedCodeGenContextDeleter
{
    void operator()(const SharedCodeGenContext* context) const noexcept;
};

using UniqueSharedCodeGenContext = std::unique_ptr<SharedCodeGenContext, SharedCodeGenContextDeleter>;

// Creates a new SharedCodeGenContext that can be used by multiple Luau VMs
// concurrently, using either the default allocator parameters or custom
// allocator parameters.
[[nodiscard]] UniqueSharedCodeGenContext createSharedCodeGenContext();

[[nodiscard]] UniqueSharedCodeGenContext createSharedCodeGenContext(AllocationCallback* allocationCallback, void* allocationCallbackContext);

[[nodiscard]] UniqueSharedCodeGenContext createSharedCodeGenContext(
    size_t blockSize,
    size_t maxTotalSize,
    AllocationCallback* allocationCallback,
    void* allocationCallbackContext
);

// Destroys the provided SharedCodeGenContext.  All Luau VMs using the
// SharedCodeGenContext must be destroyed before this function is called.
void destroySharedCodeGenContext(const SharedCodeGenContext* codeGenContext) noexcept;

// Initializes native code-gen on the provided Luau VM, using a VM-specific
// code-gen context and either the default allocator parameters or custom
// allocator parameters.
void create(lua_State* L);
void create(lua_State* L, AllocationCallback* allocationCallback, void* allocationCallbackContext);
void create(lua_State* L, size_t blockSize, size_t maxTotalSize, AllocationCallback* allocationCallback, void* allocationCallbackContext);

// Initializes native code-gen on the provided Luau VM, using the provided
// SharedCodeGenContext.  Note that after this function is called, the
// SharedCodeGenContext must not be destroyed until after the Luau VM L is
// destroyed via lua_close.
void create(lua_State* L, SharedCodeGenContext* codeGenContext);

// Check if native execution is enabled
[[nodiscard]] bool isNativeExecutionEnabled(lua_State* L);

// Enable or disable native execution according to `enabled` argument
void setNativeExecutionEnabled(lua_State* L, bool enabled);

// Given a name, this function must return the index of the type which matches the type array used all CompilationOptions and AssemblyOptions
// If the type is unknown, 0xff has to be returned
using UserdataRemapperCallback = uint8_t(void* context, const char* name, size_t nameLength);

void setUserdataRemapper(lua_State* L, void* context, UserdataRemapperCallback cb);

using ModuleId = std::array<uint8_t, 16>;

// Builds target function and all inner functions
CompilationResult compile(lua_State* L, int idx, unsigned int flags, CompilationStats* stats = nullptr);
CompilationResult compile(const ModuleId& moduleId, lua_State* L, int idx, unsigned int flags, CompilationStats* stats = nullptr);

CompilationResult compile(lua_State* L, int idx, const CompilationOptions& options, CompilationStats* stats = nullptr);
CompilationResult compile(const ModuleId& moduleId, lua_State* L, int idx, const CompilationOptions& options, CompilationStats* stats = nullptr);

// Tier-up compilation. Protos loaded while the hot threshold is nonzero count interpreted entries and loop iterations, and join a bounded
// queue when the count runs out. A threshold change applies to protos loaded afterwards. A full queue rearms the counter instead of growing.
void setHotThreshold(lua_State* L, int threshold);

// Resizes the hot queue; protos that no longer fit stop counting. This allocates VM memory and can raise a Lua memory error.
void setHotQueueCapacity(lua_State* L, int capacity);

[[nodiscard]] int getHotQueueSize(lua_State* L);

// Compiles queued protos oldest first until the next one would exceed the bytecode instruction budget. The first uncompiled proto is always
// taken. Already compiled protos are dropped from the queue without compiling. Nested functions are not compiled with their parent.
CompilationResult compileHot(lua_State* L, size_t instructionBudget, CompilationStats* stats = nullptr);

// Promotion split so that code generation can run away from the VM thread. takeHot dequeues protos as compileHot does, snapshots their
// bytecode, and keeps them alive; it returns nullptr when nothing needs compiling. compileHotBatch generates code from the snapshots and
// touches no VM state, so any one thread may run it while the VM continues. bindHotBatch installs the code on the VM thread, skipping protos
// compiled through another path or given a breakpoint meanwhile, and frees the batch. discardHotBatch frees a batch without installing it.
// Every batch is bound or discarded on the VM thread before the VM closes, and never while compileHotBatch is still running on it.
struct HotBatch;
[[nodiscard]] HotBatch* takeHot(lua_State* L, size_t instructionBudget);
void compileHotBatch(HotBatch* batch);
CompilationResult bindHotBatch(lua_State* L, HotBatch* batch, CompilationStats* stats = nullptr);
void discardHotBatch(lua_State* L, HotBatch* batch);

// Generates assembly for target function and all inner functions
std::string getAssembly(lua_State* L, int idx, AssemblyOptions options = {}, LoweringStats* stats = nullptr);

// Generate assembly for manually-constructed IR
std::string getAssemblyFromIr(struct IrBuilder& ir, AssemblyOptions options = {}, LoweringStats* stats = nullptr);

using PerfLogFn = void (*)(void* context, uintptr_t addr, unsigned size, const char* symbol);

void setPerfLog(void* context, PerfLogFn logFn);

} // namespace CodeGen
} // namespace Luau
