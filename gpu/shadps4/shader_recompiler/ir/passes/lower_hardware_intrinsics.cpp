// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "bbport_nan_guard.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/program.h"

namespace Shader::Optimization {

// bbport: V_CMP_CLASS_F32 with every bit of its class mask (BB_CMP_CLASS_EXACT=1), from the
// value's bits: sign, exponent, mantissa and the quiet bit of NaNs.
static IR::U1 ExactClassTest(IR::IREmitter& ir, const IR::F32& value, u32 mask) {
    using Class = IR::FloatClassFunc;
    const auto has = [mask](Class c) { return (mask & static_cast<u32>(c)) != 0; };
    const auto all = [mask](Class c) {
        return (mask & static_cast<u32>(c)) == static_cast<u32>(c);
    };
    const u32 nan_inf = static_cast<u32>(Class::NaN | Class::Infinity);
    // Exact shortcuts for the common masks.
    if (mask == static_cast<u32>(Class::NaN)) {
        return ir.FPIsNan(value);
    }
    if (mask == static_cast<u32>(Class::Infinity)) {
        return ir.FPIsInf(value);
    }
    if (mask == nan_inf) {
        return ir.LogicalOr(ir.FPIsNan(value), ir.FPIsInf(value));
    }
    if (mask == static_cast<u32>(Class::Finite)) {
        return ir.LogicalNot(ir.LogicalOr(ir.FPIsNan(value), ir.FPIsInf(value)));
    }
    const IR::U32 bits{ir.BitCast<IR::U32>(value)};
    const IR::U32 exponent{ir.BitFieldExtract(bits, ir.Imm32(23), ir.Imm32(8))};
    const IR::U32 mantissa{ir.BitwiseAnd(bits, ir.Imm32(0x7FFFFFu))};
    const IR::U1 negative{ir.INotEqual(ir.BitwiseAnd(bits, ir.Imm32(0x80000000u)), ir.Imm32(0u))};
    const IR::U1 positive{ir.LogicalNot(negative)};
    const IR::U1 exp_max{ir.IEqual(exponent, ir.Imm32(0xFFu))};
    const IR::U1 exp_zero{ir.IEqual(exponent, ir.Imm32(0u))};
    const IR::U1 mant_zero{ir.IEqual(mantissa, ir.Imm32(0u))};
    const IR::U1 quiet{ir.INotEqual(ir.BitwiseAnd(bits, ir.Imm32(0x400000u)), ir.Imm32(0u))};
    const IR::U1 is_nan{ir.LogicalAnd(exp_max, ir.LogicalNot(mant_zero))};
    const IR::U1 is_inf{ir.LogicalAnd(exp_max, mant_zero)};
    const IR::U1 is_zero{ir.LogicalAnd(exp_zero, mant_zero)};
    const IR::U1 is_denorm{ir.LogicalAnd(exp_zero, ir.LogicalNot(mant_zero))};
    const IR::U1 is_normal{ir.LogicalNot(ir.LogicalOr(exp_max, exp_zero))};
    IR::U1 result{ir.Imm1(false)};
    const auto add = [&](const IR::U1& term) { result = ir.LogicalOr(result, term); };
    if (all(Class::NaN)) {
        add(is_nan);
    } else if (has(Class::SignalingNan)) {
        add(ir.LogicalAnd(is_nan, ir.LogicalNot(quiet)));
    } else if (has(Class::QuietNan)) {
        add(ir.LogicalAnd(is_nan, quiet));
    }
    const auto add_signed = [&](Class neg_class, Class pos_class, const IR::U1& is_class) {
        if (has(neg_class) && has(pos_class)) {
            add(is_class);
        } else if (has(neg_class)) {
            add(ir.LogicalAnd(is_class, negative));
        } else if (has(pos_class)) {
            add(ir.LogicalAnd(is_class, positive));
        }
    };
    add_signed(Class::NegativeInfinity, Class::PositiveInfinity, is_inf);
    add_signed(Class::NegativeNormal, Class::PositiveNormal, is_normal);
    add_signed(Class::NegativeDenorm, Class::PositiveDenorm, is_denorm);
    add_signed(Class::NegativeZero, Class::PositiveZero, is_zero);
    return result;
}

static void LowerCmpClass(IR::Block& block, IR::Inst& inst) {
    ASSERT_MSG(inst.Arg(1).IsImmediate(), "Unable to resolve compare operation");
    if (BbNanGuard::ExactCmpClass()) {
        IR::IREmitter ir{block, IR::Block::InstructionList::s_iterator_to(inst)};
        inst.ReplaceUsesWithAndRemove(
            ExactClassTest(ir, IR::F32{inst.Arg(0)}, inst.Arg(1).U32()));
        return;
    }
    const auto class_mask = static_cast<IR::FloatClassFunc>(inst.Arg(1).U32());
    if ((class_mask & IR::FloatClassFunc::NaN) == IR::FloatClassFunc::NaN) {
        inst.ReplaceOpcode(IR::Opcode::FPIsNan32);
    } else if ((class_mask & IR::FloatClassFunc::Infinity) == IR::FloatClassFunc::Infinity) {
        inst.ReplaceOpcode(IR::Opcode::FPIsInf32);
    } else if ((class_mask & IR::FloatClassFunc::Finite) == IR::FloatClassFunc::Finite) {
        IR::IREmitter ir{block, IR::Block::InstructionList::s_iterator_to(inst)};
        const IR::F32 value = IR::F32{inst.Arg(0)};
        inst.ReplaceUsesWithAndRemove(
            ir.LogicalNot(ir.LogicalOr(ir.FPIsNan(value), ir.FPIsInf(value))));
    } else {
        UNREACHABLE();
    }
}

static void LowerPackedAncillary(IR::Block& block, IR::Inst& inst) {
    if (inst.Arg(0).IsImmediate() || !inst.Arg(1).IsImmediate() || !inst.Arg(2).IsImmediate()) {
        return;
    }
    IR::Inst* value = inst.Arg(0).Inst();
    if (value->GetOpcode() != IR::Opcode::GetAttributeU32 ||
        value->Arg(0).Attribute() != IR::Attribute::PackedAncillary) {
        return;
    }
    const u32 offset = inst.Arg(1).U32();
    const u32 bits = inst.Arg(2).U32();
    IR::IREmitter ir{block, IR::Block::InstructionList::s_iterator_to(inst)};
    if (offset >= 8 && offset + bits <= 12) {
        const auto sample_index = ir.GetAttributeU32(IR::Attribute::SampleIndex);
        if (offset == 8 && bits == 4) {
            inst.ReplaceUsesWithAndRemove(sample_index);
        } else {
            inst.ReplaceUsesWithAndRemove(
                ir.BitFieldExtract(sample_index, ir.Imm32(offset - 8), ir.Imm32(bits)));
        }
    } else if (offset >= 16 && offset + bits <= 27) {
        const auto mrt_index = ir.GetAttributeU32(IR::Attribute::RenderTargetIndex);
        if (offset == 16 && bits == 11) {
            inst.ReplaceUsesWithAndRemove(mrt_index);
        } else {
            inst.ReplaceUsesWithAndRemove(
                ir.BitFieldExtract(mrt_index, ir.Imm32(offset - 16), ir.Imm32(bits)));
        }
    } else {
        UNREACHABLE_MSG("Unhandled bitfield extract from ancillary VGPR offset={}, bits={}", offset,
                        bits);
    }
    value->ReplaceUsesWithAndRemove(ir.Imm32(0U));
}

static void LowerMaskedBitCount(IR::Block& block, IR::Inst& inst) {
    IR::IREmitter ir{block, IR::Block::InstructionList::s_iterator_to(inst)};
    const IR::U32 thread_mask{
        ir.GetAttributeU32(IR::Attribute::SubgroupLtMask, inst.Arg(2).U1() ? 1 : 0)};
    const IR::U32 masked_value{ir.BitCount(ir.BitwiseAnd(IR::U32{inst.Arg(0)}, thread_mask))};
    inst.ReplaceUsesWithAndRemove(ir.IAdd(masked_value, IR::U32{inst.Arg(1)}));
}

static void Lower(IR::Block& block, IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::FPCmpClass32:
        return LowerCmpClass(block, inst);
    case IR::Opcode::BitFieldUExtract:
        return LowerPackedAncillary(block, inst);
    case IR::Opcode::MaskedBitCount32:
        return LowerMaskedBitCount(block, inst);
    default:
        break;
    }
}

void LowerHardwareIntrinsics(IR::Program& program) {
    for (IR::Block* const block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            Lower(*block, inst);
        }
    }
}

} // namespace Shader::Optimization
