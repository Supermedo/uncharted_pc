// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// bbport: the barrier fixes for 32-lane subgroups (Uncharted 3: Sully's shirt flashing as flat
// triangles, the cloth solvers 1c957b01 and 809864d3) are BilalReloaded's, from
// https://github.com/bc160201983/Uncharted-The-Nathan-Drake-Collection-PC-port (GPL v2).

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <cstring>
#include <string_view>
#include <unordered_set>
#include <vector>
#include "shader_recompiler/ir/breadth_first_search.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/resource_pass.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"

namespace Shader::Optimization {

static bool IsLoadShared(const IR::Inst& inst) {
    return inst.GetOpcode() == IR::Opcode::LoadSharedU16 ||
           inst.GetOpcode() == IR::Opcode::LoadSharedU32 ||
           inst.GetOpcode() == IR::Opcode::LoadSharedU64;
}

static bool IsWriteShared(const IR::Inst& inst) {
    const IR::Opcode opcode = inst.GetOpcode();
    if (opcode >= IR::Opcode::SharedAtomicIAdd32 && opcode <= IR::Opcode::SharedAtomicCmpSwap64) {
        return !inst.Flags<bool>();
    }
    return opcode == IR::Opcode::WriteSharedU16 || opcode == IR::Opcode::WriteSharedU32 ||
           opcode == IR::Opcode::WriteSharedU64;
}

// Inserts barriers when a shared memory write and read occur in the same basic block.
static void EmitBarrierInBlock(IR::Block* block) {
    enum class BarrierAction : u32 {
        None,
        BarrierOnWrite,
        BarrierOnRead,
    };
    BarrierAction action{};
    for (IR::Inst& inst : block->Instructions()) {
        if (IsLoadShared(inst)) {
            if (action == BarrierAction::BarrierOnRead) {
                IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
                ir.Barrier();
            }
            action = BarrierAction::BarrierOnWrite;
            continue;
        }
        if (IsWriteShared(inst)) {
            if (action == BarrierAction::BarrierOnWrite) {
                IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
                ir.Barrier();
            }
            action = BarrierAction::BarrierOnRead;
        }
    }
    if (action != BarrierAction::None) {
        IR::IREmitter ir{*block, --block->end()};
        ir.Barrier();
    }
}

using NodeSet = std::unordered_set<const IR::Block*>;

static void EmitBarrierAtBlockStart(IR::Block* block) {
    auto insert_point = std::ranges::find_if_not(block->Instructions(), IR::IsPhi);
    IR::IREmitter ir{*block, insert_point};
    ir.Barrier();
}

static bool IsDivergent(const IR::U1& cond) {
    return IR::BreadthFirstSearch(cond, [](IR::Inst* inst) -> std::optional<bool> {
               if (inst->GetOpcode() == IR::Opcode::GetAttributeU32 &&
                   inst->Arg(0).Attribute() == IR::Attribute::LocalInvocationId) {
                   return true;
               }
               return std::nullopt;
           }) == true;
}

// BilalReloaded: with several GCN waves per group a condition can be uniform inside each wave (ballots,
// readfirstlane, loaded data) and still differ between waves: a group barrier inside it hangs
// the GPU. Only values built from constants, user data and the workgroup id count as uniform.
static bool IsGroupNonUniform(const IR::U1& cond) {
    using Op = IR::Opcode;
    return IR::BreadthFirstSearch(cond, [](IR::Inst* inst) -> std::optional<bool> {
               switch (inst->GetOpcode()) {
               case Op::GetAttribute:
               case Op::GetAttributeU1:
               case Op::GetAttributeU32:
                   if (inst->NumArgs() > 0 && inst->Arg(0).IsImmediate() &&
                       inst->Arg(0).Type() == IR::Type::Attribute &&
                       inst->Arg(0).Attribute() == IR::Attribute::WorkgroupId) {
                       return std::nullopt;
                   }
                   return true;
               case Op::GetExec:
               case Op::GetVcc:
               case Op::GetVccLo:
               case Op::GetVccHi:
               case Op::GetScc:
               case Op::GetScalarRegister:
               case Op::GetVectorRegister:
               case Op::GetVirtualRegister:
               case Op::MaskedBitCount32:
               case Op::LaneId:
               case Op::QuadBroadcast:
               case Op::ReadFirstLane:
               case Op::ReadLane:
               case Op::WriteLane:
               case Op::Ballot:
               case Op::BallotFindLsb:
               case Op::InverseBallot:
               case Op::GroupAny:
                   return true;
               default:
                   break;
               }
               const auto name = IR::NameOf(inst->GetOpcode());
               if (name.starts_with("LoadShared") || name.starts_with("SharedAtomic") ||
                   name.starts_with("LoadBuffer") || name.starts_with("BufferAtomic") ||
                   name.starts_with("Image") || name.starts_with("Load")) {
                   return true;
               }
               return std::nullopt;
           }) == true;
}

// Inserts a barrier after divergent conditional blocks to avoid undefined
// behavior when some threads write and others read from shared memory.
static void EmitBarrierInMergeBlock(const IR::AbstractSyntaxNode::Data& data,
                                    NodeSet& divergence_end, u32& divergence_depth,
                                    bool group_check = false,
                                    const std::function<bool(const IR::Block*)>& allowed = {},
                                    const NodeSet* barrier_regions = nullptr) {
    const IR::U1 cond = data.if_node.cond;
    if (barrier_regions && barrier_regions->contains(data.if_node.merge)) {
        return; // the game's own barrier inside: uniform
    }
    if (group_check ? IsGroupNonUniform(cond) : IsDivergent(cond)) {
        if (divergence_depth == 0 && (!allowed || allowed(data.if_node.merge))) {
            EmitBarrierAtBlockStart(data.if_node.merge);
        }
        ++divergence_depth;
        divergence_end.emplace(data.if_node.merge);
    }
}

// A barrier inside a loop is invalid when different invocations leave on different iterations.
// Mark such loops so their shared-memory synchronization can be deferred to the merge block.
// BilalReloaded 17 Oct 2026: merge blocks of the ifs and loops whose body already holds one of the
// game's own barriers (s_barrier). The game runs those barriers in every wave, so the conditions
// around them are uniform across the group however they are computed (a loop exit read from LDS
// after a reduction). Uncharted 3's cloth solver: its convergence loop re-wrote lds[0] at the top
// of the next iteration while slower warps still read the previous sum at the bottom; the loop
// counted as divergent (LDS-read exit), so no barrier went between them and Sully's shirt still
// exploded now and then while he walked.
static NodeSet FindBarrierRegions(const IR::AbstractSyntaxList& syntax_list) {
    using Type = IR::AbstractSyntaxNode::Type;
    NodeSet regions;
    std::vector<const IR::Block*> open;
    for (const IR::AbstractSyntaxNode& node : syntax_list) {
        switch (node.type) {
        case Type::If:
            open.push_back(node.data.if_node.merge);
            break;
        case Type::Loop:
            open.push_back(node.data.loop.merge);
            break;
        case Type::EndIf:
        case Type::Repeat: {
            const IR::Block* merge =
                node.type == Type::EndIf ? node.data.end_if.merge : node.data.repeat.merge;
            const auto it = std::ranges::find(open, merge);
            if (it != open.end()) {
                open.erase(it, open.end());
            }
            break;
        }
        case Type::Block:
            for (const IR::Inst& inst : node.data.block->Instructions()) {
                if (inst.GetOpcode() == IR::Opcode::Barrier) {
                    regions.insert(open.begin(), open.end());
                    break;
                }
            }
            break;
        default:
            break;
        }
    }
    return regions;
}

static NodeSet FindDivergentLoops(const IR::AbstractSyntaxList& syntax_list, bool group_check = false,
                                  const NodeSet* barrier_regions = nullptr) {
    NodeSet divergent_loops;
    const auto divergent = [group_check](const IR::U1& cond) {
        return group_check ? IsGroupNonUniform(cond) : IsDivergent(cond);
    };
    const auto game_barrier = [barrier_regions](const IR::Block* merge) {
        return barrier_regions && barrier_regions->contains(merge);
    };
    for (const IR::AbstractSyntaxNode& node : syntax_list) {
        switch (node.type) {
        case IR::AbstractSyntaxNode::Type::Repeat:
            if (divergent(node.data.repeat.cond) && !game_barrier(node.data.repeat.merge)) {
                divergent_loops.emplace(node.data.repeat.merge);
            }
            break;
        case IR::AbstractSyntaxNode::Type::Break:
            if (divergent(node.data.break_node.cond) && !game_barrier(node.data.break_node.merge)) {
                divergent_loops.emplace(node.data.break_node.merge);
            }
            break;
        default:
            break;
        }
    }
    return divergent_loops;
}

// GCN waves run in lockstep, so games load from a buffer what another invocation of the wave
// stored without a barrier. Inserts one after such stores. (shadPS4 PR #5310; BilalReloaded 16 Oct 2026:
// Uncharted 3's objects flickered as flat-coloured shapes on NVIDIA, 32-lane subgroups, and not
// on an AMD GPU, which runs these shaders with 64 lanes.)
static void EmitBufferBarriers(IR::Program& program) {
    using Type = IR::AbstractSyntaxNode::Type;
    // Index of the last load from each buffer, counting instructions in program order.
    std::array<u32, NUM_BUFFERS> last_load{};
    // Threads that skip one just run to the end.
    NodeSet early_outs;
    u32 index{};
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        if (node.type == Type::Block) {
            for (const IR::Inst& inst : node.data.block->Instructions()) {
                ++index;
                if (IsBufferInstruction(inst) && !IsBufferStore(inst)) {
                    last_load[inst.Arg(0).U32()] = index;
                }
            }
        } else if (node.type == Type::EndIf) {
            early_outs.emplace(node.data.end_if.merge);
        } else if (node.type != Type::Return) {
            early_outs.clear();
        }
    }
    const NodeSet divergent_loops = FindDivergentLoops(program.syntax_list);
    NodeSet divergence_end;
    u32 divergence_depth{};
    std::vector<u32> loops;
    index = 0;
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        if (node.type == Type::If && !early_outs.contains(node.data.if_node.merge) &&
            IsDivergent(node.data.if_node.cond)) {
            divergence_end.emplace(node.data.if_node.merge);
            ++divergence_depth;
        } else if (node.type == Type::EndIf) {
            divergence_depth -= static_cast<u32>(divergence_end.contains(node.data.end_if.merge));
        } else if (node.type == Type::Loop) {
            loops.push_back(index);
            divergence_depth += static_cast<u32>(divergent_loops.contains(node.data.loop.merge));
        } else if (node.type == Type::Repeat) {
            loops.pop_back();
            divergence_depth -= static_cast<u32>(divergent_loops.contains(node.data.repeat.merge));
        } else if (node.type == Type::Block) {
            const auto emit_barrier = [&](IR::Block::iterator insert_point) {
                IR::IREmitter{*node.data.block, insert_point}.Barrier(true);
            };
            // Last store to a buffer that is loaded later, in a loop also on the next iteration.
            IR::Inst* store{};
            for (IR::Inst& inst : node.data.block->Instructions()) {
                ++index;
                if (store && IsBufferInstruction(inst) && !IsBufferStore(inst)) {
                    emit_barrier(IR::Block::InstructionList::s_iterator_to(inst));
                    store = nullptr;
                } else if (divergence_depth == 0 && IsBufferStore(inst) && !IsBufferAtomic(inst) &&
                           last_load[inst.Arg(0).U32()] > (loops.empty() ? index : loops.front())) {
                    store = &inst;
                }
            }
            if (store) {
                emit_barrier(std::next(IR::Block::InstructionList::s_iterator_to(*store)));
            }
        }
    }
}

static constexpr u32 GcnSubgroupSize = 64;

// BilalReloaded 17 Oct 2026: GCN shaders with groups of several 64-lane waves still pass data through LDS
// inside one wave without a barrier (the wave runs in lockstep). With 32-lane subgroups (NVIDIA)
// the two halves of such a wave are separate warps and race: Uncharted 3's cloth solver
// (cs 1c957b01, 1024 threads, warp-synchronous reductions) blew up Sully's shirt into flat
// triangles on every other frame at 60 FPS (2 sub-steps), never on an AMD GPU (wave64).
// BB_LDS_BARRIERS_MULTIWAVE: unset = the shaders below, 0 = none, 1 = every multi-wave compute
// shader (Uncharted 3 ran clean with it; not tested in 1 and 2), or hex hashes separated by commas.
bool MultiwaveLdsBarriers(u64 pgm_hash) {
    static const std::vector<u64> hashes = [] {
        std::vector<u64> list;
        const char* env = std::getenv("BB_LDS_BARRIERS_MULTIWAVE");
        if (!env) {
            // Uncharted 3's two cloth solvers (Sully's shirt): 1024- and 128-thread groups.
            return std::vector<u64>{0x1c957b01ULL, 0x809864d3ULL};
        }
        if (env[0] == '1' && env[1] == 0) {
            return std::vector<u64>{~0ULL};
        }
        for (const char* s = env; s && *s;) {
            char* next{};
            const u64 hash = std::strtoull(s, &next, 16);
            if (hash > 1) {
                list.push_back(hash);
            }
            s = next && *next == ',' ? next + 1 : nullptr;
        }
        return list;
    }();
    return std::ranges::any_of(hashes, [pgm_hash](u64 hash) { return hash == ~0ULL || hash == pgm_hash; });
}

// BilalReloaded 17 Oct 2026: shaders whose ifs/loops around the game's own barriers count as uniform
// (FindBarrierRegions). Both cloth solvers: 809864d3 has the same convergence loop as 1c957b01
// (exit read from lds[0], 17 instead of 47 barriers in it without the rule) and still flung
// shirt triangles while 1c957b01 was fixed. An earlier hang blamed on it came from a shader dump
// run that compiled 809864d3 only after the stall. Per shader:
// BB_LDS_BARRIER_REGIONS = hex hashes separated by commas, 1 = all, 0 = none.
static bool BarrierRegionShader(u64 pgm_hash) {
    static const std::vector<u64> hashes = [] {
        const char* env = std::getenv("BB_LDS_BARRIER_REGIONS");
        if (!env) {
            return std::vector<u64>{0x1c957b01ULL, 0x809864d3ULL};
        }
        if (env[0] == '1' && env[1] == 0) {
            return std::vector<u64>{~0ULL};
        }
        std::vector<u64> list;
        for (const char* s = env; s && *s;) {
            char* next{};
            const u64 hash = std::strtoull(s, &next, 16);
            if (hash > 1) {
                list.push_back(hash);
            }
            s = next && *next == ',' ? next + 1 : nullptr;
        }
        return list;
    }();
    return std::ranges::any_of(hashes, [pgm_hash](u64 hash) { return hash == ~0ULL || hash == pgm_hash; });
}

void SharedMemoryBarrierPass(IR::Program& program, const RuntimeInfo& runtime_info,
                             const Profile& profile) {
    if (program.info.hw_stage != HwStage::Compute) {
        return;
    }
    const auto& cs_info = runtime_info.hw.cs;
    const u32 shared_memory_size = cs_info.shared_memory_size;
    const u32 threadgroup_size =
        cs_info.workgroup_size[0] * cs_info.workgroup_size[1] * cs_info.workgroup_size[2];
    // Opt-in while untested in Uncharted 1/2 (it did not fix Uncharted 3's flicker). Cached
    // shaders do not record the switch: clear the pipeline cache when changing it.
    static const bool buffer_barriers = [] {
        const char* env = std::getenv("BB_WAVE_BUFFER_BARRIERS");
        return env && env[0] == '1';
    }();
    if (buffer_barriers && threadgroup_size == GcnSubgroupSize &&
        profile.subgroup_size != GcnSubgroupSize) {
        EmitBufferBarriers(program);
    }
    // BilalReloaded's diagnostics (BB_LOG_LDS=1): compute shaders with shared memory, their group size and
    // whether LDS barriers are inserted.
    static const bool log_lds = [] {
        const char* env = std::getenv("BB_LOG_LDS");
        return env && env[0] == '1';
    }();
    if (log_lds && shared_memory_size) {
        u32 lds_ops = 0, barriers = 0;
        for (const IR::Block* block : program.blocks) {
            for (const IR::Inst& inst : block->Instructions()) {
                lds_ops += IsLoadShared(inst) || IsWriteShared(inst);
                barriers += inst.GetOpcode() == IR::Opcode::Barrier;
            }
        }
        std::printf("LDS shader %016llx: group %ux%ux%u (%u threads), %u bytes shared, %u LDS ops, %u barriers "
                    "in the shader, subgroup %u, pass %s\n",
                    static_cast<unsigned long long>(program.info.pgm_hash), cs_info.workgroup_size[0],
                    cs_info.workgroup_size[1], cs_info.workgroup_size[2], threadgroup_size, shared_memory_size,
                    lds_ops, barriers, profile.subgroup_size,
                    !profile.needs_lds_barriers                    ? "skipped"
                    : threadgroup_size == GcnSubgroupSize          ? "inserts barriers"
                    : MultiwaveLdsBarriers(program.info.pgm_hash) ? "inserts multi-wave barriers"
                                                                   : "skipped");
        std::fflush(stdout);
    }
    // The compiler can only omit barriers when the local workgroup size is the same as the HW
    // subgroup. BilalReloaded: groups of several waves too, on GPUs with 32-lane subgroups (see
    // MultiwaveLdsBarriers).
    const bool multiwave = MultiwaveLdsBarriers(program.info.pgm_hash);
    const bool wave_split = profile.subgroup_size < GcnSubgroupSize;
    const bool eligible = threadgroup_size == GcnSubgroupSize ||
                          (multiwave && wave_split && threadgroup_size > GcnSubgroupSize &&
                           threadgroup_size % GcnSubgroupSize == 0);
    if (shared_memory_size == 0 || !eligible || !profile.needs_lds_barriers) {
        return;
    }
    using Type = IR::AbstractSyntaxNode::Type;
    u32 divergence_depth{};
    NodeSet divergence_end;
    const bool group_check = threadgroup_size != GcnSubgroupSize;
    if (group_check) {
        // An early return could leave waves short of a barrier added below: skip such shaders.
        for (size_t i = 0; i + 1 < program.syntax_list.size(); ++i) {
            if (program.syntax_list[i].type == Type::Return) {
                return;
            }
        }
    }
    const bool use_regions = group_check && BarrierRegionShader(program.info.pgm_hash);
    const NodeSet barrier_regions = use_regions ? FindBarrierRegions(program.syntax_list) : NodeSet{};
    const NodeSet divergent_loops =
        FindDivergentLoops(program.syntax_list, group_check, use_regions ? &barrier_regions : nullptr);
    // BilalReloaded's diagnostics (BB_LDS_BARRIER_SITES=<first>-<end>[,<first>-<end>...]): for groups of
    // several waves, only insert the barrier sites (merge blocks and blocks with LDS ops, in
    // program order) in those ranges; BB_LOG_LDS=1 lists them. Bisects a site that hangs the GPU.
    static const std::vector<std::pair<u32, u32>> site_ranges = [] {
        std::vector<std::pair<u32, u32>> ranges;
        const char* env = std::getenv("BB_LDS_BARRIER_SITES");
        for (const char* s = env; s && *s;) {
            char* next{};
            const u32 first = u32(std::strtoul(s, &next, 10));
            u32 end = first + 1;
            if (*next == '-') {
                end = u32(std::strtoul(next + 1, &next, 10));
            }
            ranges.emplace_back(first, end);
            s = *next == ',' ? next + 1 : nullptr;
        }
        if (ranges.empty()) {
            ranges.emplace_back(0u, ~0u);
        }
        return ranges;
    }();
    u32 site = 0;
    const std::function<bool(const IR::Block*)> allowed = [&](const IR::Block* block) {
        if (!group_check) {
            return true;
        }
        const u32 index = site++;
        const bool in_range = std::ranges::any_of(site_ranges, [index](const auto& range) {
            return index >= range.first && index < range.second;
        });
        if (log_lds) {
            const auto it = std::ranges::find(program.blocks, block);
            std::printf("LDS barrier site %u: block $%zu %s\n", index,
                        static_cast<size_t>(it - program.blocks.begin()),
                        in_range ? "inserted" : "skipped");
        }
        return in_range;
    };
    const auto has_lds = [](const IR::Block* block) {
        for (const IR::Inst& inst : block->Instructions()) {
            if (IsLoadShared(inst) || IsWriteShared(inst)) {
                return true;
            }
        }
        return false;
    };
    // For groups of several waves only conditionals and loops that access LDS get a barrier at
    // their merge block: NVIDIA hung on one after a loop without LDS ops (cs 1c957b01, a loop the
    // dispatches never entered).
    NodeSet lds_regions;
    if (group_check) {
        std::vector<const IR::Block*> open;
        for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
            if (node.type == Type::If) {
                open.push_back(node.data.if_node.merge);
            } else if (node.type == Type::Loop) {
                open.push_back(node.data.loop.merge);
            } else if (node.type == Type::EndIf || node.type == Type::Repeat) {
                const IR::Block* merge =
                    node.type == Type::EndIf ? node.data.end_if.merge : node.data.repeat.merge;
                const auto it = std::ranges::find(open, merge);
                if (it != open.end()) {
                    open.erase(it, open.end());
                }
            } else if (node.type == Type::Block && has_lds(node.data.block)) {
                lds_regions.insert(open.begin(), open.end());
            }
        }
    }
    const std::function<bool(const IR::Block*)> allowed_merge = [&](const IR::Block* merge) {
        return !group_check || (lds_regions.contains(merge) && allowed(merge));
    };
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        if (node.type == Type::EndIf) {
            if (divergence_end.contains(node.data.end_if.merge)) {
                --divergence_depth;
            }
            continue;
        }
        // Check if branch depth is zero, we don't want to insert barrier in potentially divergent
        // code.
        if (node.type == Type::If) {
            EmitBarrierInMergeBlock(node.data, divergence_end, divergence_depth, group_check,
                                    allowed_merge, use_regions ? &barrier_regions : nullptr);
            continue;
        }
        if (node.type == Type::Loop && divergent_loops.contains(node.data.loop.merge)) {
            ++divergence_depth;
            continue;
        }
        if (node.type == Type::Repeat && divergent_loops.contains(node.data.repeat.merge)) {
            ASSERT(divergence_depth > 0);
            --divergence_depth;
            if (divergence_depth == 0 && allowed_merge(node.data.repeat.merge)) {
                EmitBarrierAtBlockStart(node.data.repeat.merge);
            }
            continue;
        }
        if (node.type == Type::Block && divergence_depth == 0 &&
            (!has_lds(node.data.block) || allowed(node.data.block))) {
            EmitBarrierInBlock(node.data.block);
        }
    }
}

} // namespace Shader::Optimization
