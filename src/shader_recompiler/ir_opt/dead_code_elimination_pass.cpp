// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/value.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace Shader::Optimization {

void DeadCodeEliminationPass(IR::Program& program) {
    // We iterate over the instructions in reverse order in a fixed-point loop.
    // Removing an instruction reduces the use count of earlier instructions,
    // which in turn may become dead and eligible for elimination across block boundaries.
    bool changed{true};
    size_t pass{0};
    while (changed && pass < 4) {
        changed = false;
        ++pass;
        for (IR::Block* const block : program.post_order_blocks) {
            auto it{block->end()};
            while (it != block->begin()) {
                --it;
                if (!it->HasUses() && !it->MayHaveSideEffects()) {
                    it->Invalidate();
                    it = block->Instructions().erase(it);
                    changed = true;
                }
            }
        }
    }
}

} // namespace Shader::Optimization
