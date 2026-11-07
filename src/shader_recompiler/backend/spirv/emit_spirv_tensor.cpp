// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "common/common_types.h"
#include "shader_recompiler/frontend/ir/program.h"

namespace Shader::Backend::SPIRV {
void DefineSoftwareMatrixFunctions(EmitContext& ctx, const IR::Program& program) {
    std::array<bool, 2> needed{};
    u32 total_sites{};
    for (const auto* block : program.blocks) {
        for (const auto& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::TensorMulAdd || inst.Flags<u32>() == 0) continue;
            const u32 depth = inst.Flags<u32>();
            if (depth != 8 && depth != 16) throw LogicError("Invalid software HMMA depth");
            needed[depth == 16] = true;
            ++total_sites;
        }
    }
    if (ctx.profile.fast_software_hmma && (needed[0] || needed[1])) {
        if (!Sirit::ValidId(ctx.F16[1])) {
            ctx.AddCapability(spv::Capability::Float16);
            ctx.F16.Define(ctx, ctx.TypeFloat(16), "f16");
        }
    }
    for (u32 variant = 0; variant < 2; ++variant) {
        if (!needed[variant]) continue;
        ctx.AddCapability(spv::Capability::GroupNonUniform);
        ctx.AddCapability(spv::Capability::GroupNonUniformShuffle);
        if (!ctx.profile.SupportsSubgroupStage(program.stage) ||
            ctx.profile.has_broken_spirv_subgroup_shuffle)
            throw NotImplementedException("Software HMMA requires subgroup shuffle support");
        const Id u32ty = ctx.U32[1], f32ty = ctx.F32[1];
        const auto imm = [&](u32 v) { return ctx.Const(v); };
        const auto add = [&](Id a, Id b) { return ctx.OpIAdd(u32ty, a, b); };
        const auto and_ = [&](Id a, u32 b) { return ctx.OpBitwiseAnd(u32ty, a, imm(b)); };
        const auto unpack = [&](Id v) { return ctx.OpUnpackHalf2x16(ctx.F32[2], v); };
        if (ctx.profile.fast_software_hmma) {
            const Id type = ctx.TypeFunction(ctx.U32[2], ctx.U32[4], ctx.U32[2], ctx.U32[2]);
            const auto fn_ctrl = total_sites <= 256 ? spv::FunctionControlMask::MaskNone
                                                    : spv::FunctionControlMask::DontInline;
            const Id fn = ctx.OpFunction(ctx.U32[2], fn_ctrl, type);
            ctx.software_matrix_functions[variant] = fn;
            const Id matrix_a = ctx.OpFunctionParameter(ctx.U32[4]);
            const Id matrix_b = ctx.OpFunctionParameter(ctx.U32[2]);
            const Id matrix_c = ctx.OpFunctionParameter(ctx.U32[2]);
            ctx.AddLabel();

            std::array<Id, 4> a_regs;
            for (u32 i = 0; i < 4; ++i) {
                a_regs[i] = ctx.OpCompositeExtract(u32ty, matrix_a, i);
            }
            const std::array<Id, 2> b_regs{
                ctx.OpCompositeExtract(u32ty, matrix_b, 0),
                ctx.OpCompositeExtract(u32ty, matrix_b, 1),
            };
            std::array<Id, 4> acc;
            for (u32 row = 0; row < 2; ++row) {
                const Id c = unpack(ctx.OpCompositeExtract(u32ty, matrix_c, row));
                acc[row * 2 + 0] = ctx.OpCompositeExtract(f32ty, c, 0);
                acc[row * 2 + 1] = ctx.OpCompositeExtract(f32ty, c, 1);
            }

            const Id lane = ctx.OpLoad(u32ty, ctx.subgroup_local_invocation_id);
            Id row_base{};
            Id col_base0{};
            if (ctx.profile.warp_size_potentially_larger_than_guest) {
                const Id warp_base = and_(lane, ~31U);
                row_base = and_(lane, ~3U);
                col_base0 = ctx.OpBitwiseOr(
                    u32ty, warp_base, ctx.OpShiftLeftLogical(u32ty, and_(lane, 3U), imm(3)));
            } else {
                row_base = and_(lane, 28U);
                col_base0 = ctx.OpShiftLeftLogical(u32ty, and_(lane, 3U), imm(3));
            }
            const Id col_base1 = ctx.OpBitwiseOr(u32ty, col_base0, imm(4));

            std::array<Id, 4> a_lanes;
            std::array<Id, 4> b_lanes0;
            std::array<Id, 4> b_lanes1;
            for (u32 comp = 0; comp < 4; ++comp) {
                a_lanes[comp] = comp == 0 ? row_base : ctx.OpBitwiseOr(u32ty, row_base, imm(comp));
                b_lanes0[comp] =
                    comp == 0 ? col_base0 : ctx.OpBitwiseOr(u32ty, col_base0, imm(comp));
                b_lanes1[comp] =
                    comp == 0 ? col_base1 : ctx.OpBitwiseOr(u32ty, col_base1, imm(comp));
            }

            const u32 num_groups = variant ? 2U : 1U;
            for (u32 group = 0; group < num_groups; ++group) {
                const Id b = b_regs[group];
                for (u32 comp = 0; comp < 4; ++comp) {
                    const std::array<Id, 2> bs{
                        unpack(ctx.OpGroupNonUniformShuffle(u32ty, imm(3), b, b_lanes0[comp])),
                        unpack(ctx.OpGroupNonUniformShuffle(u32ty, imm(3), b, b_lanes1[comp])),
                    };
                    const std::array<Id, 2> b0{
                        ctx.OpCompositeExtract(f32ty, bs[0], 0),
                        ctx.OpCompositeExtract(f32ty, bs[0], 1),
                    };
                    const std::array<Id, 2> b1{
                        ctx.OpCompositeExtract(f32ty, bs[1], 0),
                        ctx.OpCompositeExtract(f32ty, bs[1], 1),
                    };
                    for (u32 row = 0; row < 2; ++row) {
                        const Id a = a_regs[group * 2 + row];
                        const Id as =
                            unpack(ctx.OpGroupNonUniformShuffle(u32ty, imm(3), a, a_lanes[comp]));
                        const Id a0 = ctx.OpCompositeExtract(f32ty, as, 0);
                        const Id a1 = ctx.OpCompositeExtract(f32ty, as, 1);
                        acc[row * 2 + 0] = ctx.OpFma(
                            f32ty, a1, b0[1], ctx.OpFma(f32ty, a0, b0[0], acc[row * 2 + 0]));
                        acc[row * 2 + 1] = ctx.OpFma(
                            f32ty, a1, b1[1], ctx.OpFma(f32ty, a0, b1[0], acc[row * 2 + 1]));
                    }
                }
                if (variant && group == 0) {
                    // Preserve Ampere HMMA.F16 midpoint K=8 IEEE-754 RTE FP16 rounding via
                    // hardware float16 conversion (lossy narrowing is never folded away by NIR).
                    for (u32 row = 0; row < 2; ++row) {
                        const Id vec_f32 =
                            ctx.OpCompositeConstruct(ctx.F32[2], acc[row * 2], acc[row * 2 + 1]);
                        const Id rounded =
                            ctx.OpFConvert(ctx.F32[2], ctx.OpFConvert(ctx.F16[2], vec_f32));
                        acc[row * 2 + 0] = ctx.OpCompositeExtract(f32ty, rounded, 0);
                        acc[row * 2 + 1] = ctx.OpCompositeExtract(f32ty, rounded, 1);
                    }
                }
            }

            std::array<Id, 2> packed;
            for (u32 row = 0; row < 2; ++row) {
                const Id vec_f32 =
                    ctx.OpCompositeConstruct(ctx.F32[2], acc[row * 2], acc[row * 2 + 1]);
                packed[row] = ctx.OpBitcast(u32ty, ctx.OpFConvert(ctx.F16[2], vec_f32));
            }
            ctx.OpReturnValue(ctx.OpCompositeConstruct(ctx.U32[2], packed[0], packed[1]));
            ctx.OpFunctionEnd();
            continue;
        }
        const auto round_half = [&](Id value) {
            // Do not express the intermediate rounding as unpack(pack(value)): drivers may
            // eliminate that round trip. Round normal significands explicitly, ties to even;
            // half subnormals lie on the exactly representable 2^-24 grid.
            const Id bits = ctx.OpBitcast(u32ty, value);
            const Id magnitude = and_(bits, 0x7fffffffU);
            const Id tie = and_(ctx.OpShiftRightLogical(u32ty, magnitude, imm(13)), 1);
            const Id rounded = and_(add(add(magnitude, imm(0xfff)), tie), ~0x1fffU);
            const Id normal = ctx.OpSelect(u32ty,
                ctx.OpUGreaterThanEqual(ctx.U1, rounded, imm(0x47800000)), imm(0x7f800000), rounded);
            const Id scaled = ctx.OpFMul(f32ty, ctx.OpBitcast(f32ty, magnitude), ctx.Const(16777216.0f));
            const Id subnormal = ctx.OpBitcast(u32ty,
                ctx.OpFMul(f32ty, ctx.OpRoundEven(f32ty, scaled), ctx.Const(0x1p-24f)));
            const Id finite = ctx.OpSelect(u32ty,
                ctx.OpUGreaterThanEqual(ctx.U1, magnitude, imm(0x38800000)), normal, subnormal);
            const Id result = ctx.OpSelect(u32ty,
                ctx.OpUGreaterThanEqual(ctx.U1, magnitude, imm(0x7f800000)), magnitude, finite);
            return ctx.OpBitcast(f32ty, ctx.OpBitwiseOr(u32ty, result, and_(bits, 0x80000000)));
        };
        const Id type = ctx.TypeFunction(ctx.U32[2], ctx.U32[4], ctx.U32[2], ctx.U32[2]);
        const Id fn = ctx.OpFunction(ctx.U32[2], spv::FunctionControlMask::DontInline, type);
        ctx.software_matrix_functions[variant] = fn;
        const Id matrix_a = ctx.OpFunctionParameter(ctx.U32[4]);
        const Id matrix_b = ctx.OpFunctionParameter(ctx.U32[2]);
        const Id matrix_c = ctx.OpFunctionParameter(ctx.U32[2]);
        ctx.AddLabel();
        const Id counter = ctx.AddLocalVariable(ctx.TypePointer(spv::StorageClass::Function, u32ty),
                                           spv::StorageClass::Function);
        std::array<Id, 4> accumulators;
        for (auto& acc : accumulators)
            acc = ctx.AddLocalVariable(ctx.TypePointer(spv::StorageClass::Function, f32ty),
                                 spv::StorageClass::Function);
        std::array<Id, 8> inputs;
        for (u32 i = 0; i < 4; ++i)
            inputs[i] = ctx.OpCompositeExtract(u32ty, matrix_a, i);
        for (u32 i = 0; i < 2; ++i) {
            inputs[4 + i] = ctx.OpCompositeExtract(u32ty, matrix_b, i);
            inputs[6 + i] = ctx.OpCompositeExtract(u32ty, matrix_c, i);
        }
        for (u32 row = 0; row < 2; ++row) {
            const Id c = unpack(inputs[6 + row]);
            for (u32 col = 0; col < 2; ++col)
                ctx.OpStore(accumulators[row * 2 + col], ctx.OpCompositeExtract(f32ty, c, col));
        }
        ctx.OpStore(counter, imm(0));
        const Id lane = ctx.OpLoad(u32ty, ctx.subgroup_local_invocation_id);
        const Id warp_base = and_(lane, ~31U);
        const Id row_base = and_(lane, 28);
        const Id col_base = ctx.OpShiftLeftLogical(u32ty, and_(lane, 3), imm(3));
        const Id header = ctx.OpLabel(), body = ctx.OpLabel(), next = ctx.OpLabel(), end = ctx.OpLabel();
        ctx.OpBranch(header);
        ctx.AddLabel(header);
        const Id pair = ctx.OpLoad(u32ty, counter);
        const Id more = ctx.OpULessThan(ctx.U1, pair, imm(variant ? 8 : 4));
        ctx.OpLoopMerge(end, next, spv::LoopControlMask::DontUnroll);
        ctx.OpBranchConditional(more, body, end);
        ctx.AddLabel(body);
        const Id upper = ctx.OpUGreaterThanEqual(ctx.U1, pair, imm(4));
        const Id component = and_(pair, 3);
        const Id a_lane = add(warp_base, add(row_base, component));
        const Id b_lane = add(warp_base, add(col_base, component));
        const Id b = ctx.OpSelect(u32ty, upper, inputs[5], inputs[4]);
        std::array<Id, 2> bs;
        for (u32 col = 0; col < 2; ++col)
            bs[col] = unpack(ctx.OpGroupNonUniformShuffle(u32ty, imm(3), b,
                                                          add(b_lane, imm(col * 4))));
        const Id round_now = ctx.OpIEqual(ctx.U1, component, imm(3));
        for (u32 row = 0; row < 2; ++row) {
            const Id a = ctx.OpSelect(u32ty, upper, inputs[row + 2], inputs[row]);
            const Id as = unpack(ctx.OpGroupNonUniformShuffle(u32ty, imm(3), a, a_lane));
            for (u32 col = 0; col < 2; ++col) {
                Id value = ctx.OpLoad(f32ty, accumulators[row * 2 + col]);
                for (u32 half = 0; half < 2; ++half) {
                    value = ctx.OpFma(f32ty, ctx.OpCompositeExtract(f32ty, as, half),
                                      ctx.OpCompositeExtract(f32ty, bs[col], half), value);
                    ctx.Decorate(value, spv::Decoration::NoContraction);
                }
                ctx.OpStore(accumulators[row * 2 + col], value);
            }
        }
        // HMMA.F16 rounds after each K=8 group, including the midpoint of K=16.
        // OpSelect evaluates both operands: keep the expensive half conversion in
        // a branch so the other three pairs do no rounding work. The counter is
        // identical in every participating lane and all shuffles precede this branch.
        const Id round_block = ctx.OpLabel(), round_merge = ctx.OpLabel();
        ctx.OpSelectionMerge(round_merge, spv::SelectionControlMask::DontFlatten);
        ctx.OpBranchConditional(round_now, round_block, round_merge);
        ctx.AddLabel(round_block);
        for (const Id accumulator : accumulators) {
            ctx.OpStore(accumulator, round_half(ctx.OpLoad(f32ty, accumulator)));
        }
        ctx.OpBranch(round_merge);
        ctx.AddLabel(round_merge);
        ctx.OpBranch(next);
        ctx.AddLabel(next);
        ctx.OpStore(counter, add(pair, imm(1)));
        ctx.OpBranch(header);
        ctx.AddLabel(end);
        std::array<Id, 2> packed;
        for (u32 row = 0; row < 2; ++row)
            packed[row] = ctx.OpPackHalf2x16(u32ty, ctx.OpCompositeConstruct(ctx.F32[2],
                ctx.OpLoad(f32ty, accumulators[row * 2]), ctx.OpLoad(f32ty, accumulators[row * 2 + 1])));
        ctx.OpReturnValue(ctx.OpCompositeConstruct(ctx.U32[2], packed[0], packed[1]));
        ctx.OpFunctionEnd();
    }
}

Id EmitTensorMulAdd(EmitContext& ctx, IR::Inst* inst, Id a, Id b, Id c) {
    const u32 depth = inst->Flags<u32>();
    if (depth != 8 && depth != 16) {
        throw NotImplementedException("Unsupported matrix multiply depth");
    }
    return ctx.OpFunctionCall(ctx.U32[2], ctx.software_matrix_functions[depth == 16], a, b, c);
}

} // namespace Shader::Backend::SPIRV
