// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <shared_mutex>
#include <variant>
#include <boost/container/static_vector.hpp>
#include <tsl/robin_map.h>
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/specialization.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"

template <>
struct std::hash<vk::ShaderModule> {
    std::size_t operator()(const vk::ShaderModule& module) const noexcept {
        return std::hash<size_t>{}(reinterpret_cast<size_t>((VkShaderModule)module));
    }
};

namespace AmdGpu {
class Liverpool;
}

namespace Serialization {
struct Archive;
}

namespace Shader {
struct Info;
}

namespace Vulkan {

class Instance;
class Scheduler;
class ShaderCache;

struct Program {
    struct Module {
        vk::ShaderModule module;
        Shader::StageSpecialization spec;
    };
    static constexpr size_t MaxPermutations = 8;
    using ModuleList = boost::container::small_vector<Module, MaxPermutations>;

    Shader::Info info;
    ModuleList modules{};
    size_t last_used = 0; ///< bbport: permutation of the previous lookup, compared first
    /// bbport: `info` as translated, for draw-preparation workers (they must not read `info`,
    /// whose user data the GPU thread rewrites every draw). Guarded by programs_mutex.
    std::unique_ptr<Shader::Info> info_template;

    Program() = default;
    Program(Shader::HwStage stage, Shader::SwStage l_stage, Shader::ShaderParams params)
        : info{stage, l_stage, params} {}

    void AddPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec) {
        modules.emplace_back(module, std::move(spec));
    }

    void InsertPermut(vk::ShaderModule module, Shader::StageSpecialization&& spec,
                      size_t perm_idx) {
        modules.resize(std::max(modules.size(), perm_idx + 1)); // <-- beware of realloc
        modules[perm_idx] = {module, std::move(spec)};
    }
};

struct DrawIndirectParams {
    u16 vertex_sgpr_offset;
    u32 instance_sgpr_offset;
};

} // namespace Vulkan

namespace AmdGpu {
union Regs;
}

namespace Vulkan {

/// bbport: state of one graphics/compute pipeline selection. The GPU thread owns one
/// (PipelineCache::sel); draw-preparation workers use their own with their register copies.
struct PipelineSelection {
    const AmdGpu::Regs* regs{};
    std::array<Shader::RuntimeInfo, MaxShaderStages> runtime_infos{};
    std::array<const Shader::Info*, MaxShaderStages> infos{};
    std::array<vk::ShaderModule, MaxShaderStages> modules{};
    std::optional<Shader::Gcn::FetchShaderData> fetch_shader{};
    GraphicsPipelineKey graphics_key{};
    bool motion = false;
    DrawIndirectParams draw_indirect_params{};
    struct PrepWorker* worker{}; ///< set: read-only selection for a draw-preparation worker
};

/// bbport: a draw-preparation worker's own program state (see vk_draw_prep.h).
struct PrepWorker {
    struct Stage {
        const Program* program;
        u64 hash;
        Shader::HwStage hw_stage;
        VAddr pgm_base;
        const std::vector<u32>* flat;
    };
    // Node-based: stages keep pointers to these Infos while later stages are inserted.
    std::unordered_map<const Program*, Shader::Info> infos;
    boost::container::static_vector<Stage, MaxShaderStages> stages;
    bool failed = false;
    bool compile = false; ///< may translate missing programs and build pipelines
    std::unique_ptr<Shader::Pools> pools; ///< the worker's own translation pools
};

struct PreparedDraw;

class PipelineCache {
public:
    explicit PipelineCache(const Instance& instance, Scheduler& scheduler,
                           AmdGpu::Liverpool* liverpool, u32 sparse_page_shift);
    ~PipelineCache();

    /// bbport: starts the preload of the stored pipelines on background threads. Called by the
    /// rasterizer once the object motion buffers exist (the stored motion shaders need their
    /// addresses). Selection and creation wait for the preload to finish (EnsureWarm).
    void StartWarmUp();
    void Sync();
    /// bbport: writes the driver's pipeline cache and flushes the queued blobs; called as the
    /// process ends (the window thread ends it with _Exit).
    void SaveForExit();

    /// bbport: a pipeline or shader was compiled within the last second (draw-preparation
    /// helpers then assist; see vk_draw_prep.cpp).
    [[nodiscard]] static bool CompilingRecently();

    /// bbport: true once the preload is done (or was never started).
    [[nodiscard]] bool IsWarm() const noexcept {
        return warm_done.load(std::memory_order_acquire);
    }

    const GraphicsPipeline* GetGraphicsPipeline(const DrawIndirectParams params = {},
                                                const PreparedDraw* prepared = nullptr);

    /// bbport: worker side of draw preparation: selects the pipeline key for `sel.regs` without
    /// creating anything. False when a program or permutation does not exist yet.
    bool PrepareGraphicsPipeline(PipelineSelection& sel);
    /// bbport: builds the selected graphics pipeline for a worker when it does not exist yet.
    void EnsureWorkerPipeline(PipelineSelection& sel);

    /// bbport: GPU-thread side: the pipeline for a prepared draw after checking that registers
    /// and flattened user data match; null to take the regular path.
    const GraphicsPipeline* TryPreparedPipeline(const PreparedDraw& prepared);

    /// bbport: the prepared draw the last GetGraphicsPipeline used, or null (regular path).
    [[nodiscard]] const PreparedDraw* UsedPrepared() const noexcept {
        return used_prepared;
    }

    const ComputePipeline* GetComputePipeline();

    using Result = std::tuple<const Shader::Info*, vk::ShaderModule,
                              std::optional<Shader::Gcn::FetchShaderData>, u64>;
    Result GetProgram(PipelineSelection& sel, Shader::HwStage stage, Shader::SwStage l_stage,
                      const Shader::ShaderParams& params, Shader::Backend::Bindings& binding);

    std::optional<vk::ShaderModule> ReplaceShader(vk::ShaderModule module,
                                                  std::span<const u32> spv_code);

    static std::string GetShaderName(Shader::HwStage stage, u64 hash,
                                     std::optional<size_t> perm = {});

    auto& GetProfile() const {
        return profile;
    }

private:
    bool RefreshGraphicsKey(PipelineSelection& sel);
    bool RefreshGraphicsStages(PipelineSelection& sel);
    bool RefreshComputeKey();

    void WarmUpThread();
    void EnsureWarm() {
        if (!warm_done.load(std::memory_order_acquire)) {
            WaitWarm();
        }
    }
    void WaitWarm();
    /// The driver cache blob was written by this device and driver version.
    bool DriverCacheHeaderMatches(const std::vector<char>& data) const;
    void SaveDriverCache();
    size_t DriverCacheSize();
    void DriverCacheSaver(std::stop_token stop);

    void DumpShader(std::span<const u32> code, u64 hash, Shader::HwStage stage, size_t perm_idx,
                    std::string_view ext);
    std::optional<std::vector<u32>> GetShaderPatch(u64 hash, Shader::HwStage stage, size_t perm_idx,
                                                   std::string_view ext);
    vk::ShaderModule CompileModule(Shader::Info& info, Shader::RuntimeInfo& runtime_info,
                                   const std::span<const u32>& code, size_t perm_idx,
                                   Shader::Backend::Bindings& binding,
                                   Shader::Pools& translate_pools);
    struct NewProgram {
        Program* program;
        vk::ShaderModule module;
        std::optional<Shader::Gcn::FetchShaderData> fetch_shader;
        u64 perm_hash;
    };
    NewProgram CreateProgram(Shader::HwStage hw_stage, Shader::SwStage sw_stage,
                             const Shader::ShaderParams& params, Shader::RuntimeInfo& runtime_info,
                             Shader::Backend::Bindings& binding, Shader::Pools& translate_pools);
    bool WorkerTranslate(PrepWorker& worker, Shader::HwStage hw_stage, Shader::SwStage sw_stage,
                         const Shader::ShaderParams& params, Shader::RuntimeInfo& runtime_info,
                         const Shader::Backend::Bindings& binding,
                         Shader::StageSpecialization* spec);
    Program* FindProgram(u64 hash);
    /// Serializes the creation of one program/permutation or pipeline: who finds the id taken
    /// waits for the creator and then finds the result.
    void AcquireClaim(u64 id);
    void ReleaseClaim(u64 id);
    const Shader::RuntimeInfo& BuildRuntimeInfo(PipelineSelection& sel, Shader::HwStage stage,
                                                Shader::SwStage l_stage);

    [[nodiscard]] bool IsPipelineCacheDirty() const {
        return num_new_pipelines > 0;
    }

private:
    const Instance& instance;
    Scheduler& scheduler;
    AmdGpu::Liverpool* liverpool;
    DescriptorHeap desc_heap;
    vk::UniquePipelineCache pipeline_cache;
    vk::UniquePipelineLayout pipeline_layout;
    Shader::Profile profile{};
    Shader::Pools pools;
    tsl::robin_map<size_t, std::unique_ptr<Program>> program_cache;
    /// bbport: exclusive for program/permutation insertions, shared for worker lookups.
    std::shared_mutex programs_mutex;
    u64 prepared_hits = 0, prepared_misses = 0;
    const PreparedDraw* used_prepared = nullptr;
    /// bbport: guards graphics_pipelines (draw-preparation workers insert into it).
    std::shared_mutex pipelines_mutex;
    static constexpr u64 PipelineClaimTag = 0x9E3779B97F4A7C15ull;
    std::mutex claim_mutex;
    std::condition_variable claim_cv;
    std::unordered_set<u64> claims;
    bool worker_compile = true; ///< BB_PREP_COMPILE=0 keeps all compiles on the GPU thread
    tsl::robin_map<ComputePipelineKey, std::unique_ptr<ComputePipeline>> compute_pipelines;
    tsl::robin_map<GraphicsPipelineKey, std::unique_ptr<GraphicsPipeline>> graphics_pipelines;
    PipelineSelection sel{}; ///< GPU thread selection state
    ComputePipelineKey compute_key{};
    std::atomic<u32> num_new_pipelines{}; // new pipelines added to the cache since the game start

    // bbport: preload on background threads and the driver's pipeline cache on disk.
    std::atomic<bool> warm_done{true};
    std::thread warm_thread;
    std::filesystem::path driver_cache_path;
    std::mutex driver_cache_mutex;
    std::atomic<u32> driver_cache_dirty{0}; ///< pipelines created since the last save
    size_t driver_cache_loaded_size = 0;
    std::jthread driver_cache_saver;

    // Only if Config::collectShadersForDebug()
    tsl::robin_map<vk::ShaderModule,
                   std::vector<std::variant<GraphicsPipelineKey, ComputePipelineKey>>>
        module_related_pipelines;
};

/// bbport: the process is about to end: saves the driver's pipeline cache, flushes the queued blobs.
void FlushPipelineCacheForExit();

} // namespace Vulkan
