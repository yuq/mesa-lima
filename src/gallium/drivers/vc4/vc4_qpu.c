/*
 * Copyright © 2014 Broadcom
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <stdbool.h>
#include "util/ralloc.h"
#include "vc4_qir.h"
#include "vc4_qpu.h"

static uint64_t
set_src_raddr(uint64_t inst, struct qpu_reg src)
{
        if (src.mux == QPU_MUX_A) {
                assert(QPU_GET_FIELD(inst, QPU_RADDR_A) == QPU_R_NOP ||
                       QPU_GET_FIELD(inst, QPU_RADDR_A) == src.addr);
                return ((inst & ~QPU_RADDR_A_MASK) |
                        QPU_SET_FIELD(src.addr, QPU_RADDR_A));
        }

        if (src.mux == QPU_MUX_B) {
                assert(QPU_GET_FIELD(inst, QPU_RADDR_B) == QPU_R_NOP ||
                       QPU_GET_FIELD(inst, QPU_RADDR_B) == src.addr);
                return ((inst & ~QPU_RADDR_B_MASK) |
                        QPU_SET_FIELD(src.addr, QPU_RADDR_B));
        }

        return inst;
}

uint64_t
qpu_NOP()
{
        uint64_t inst = 0;

        inst |= QPU_SET_FIELD(QPU_A_NOP, QPU_OP_ADD);
        inst |= QPU_SET_FIELD(QPU_M_NOP, QPU_OP_MUL);

        /* Note: These field values are actually non-zero */
        inst |= QPU_SET_FIELD(QPU_W_NOP, QPU_WADDR_ADD);
        inst |= QPU_SET_FIELD(QPU_W_NOP, QPU_WADDR_MUL);
        inst |= QPU_SET_FIELD(QPU_R_NOP, QPU_RADDR_A);
        inst |= QPU_SET_FIELD(QPU_R_NOP, QPU_RADDR_B);
        inst |= QPU_SET_FIELD(QPU_SIG_NONE, QPU_SIG);

        return inst;
}

static uint64_t
qpu_a_dst(struct qpu_reg dst)
{
        uint64_t inst = 0;

        if (dst.mux <= QPU_MUX_R5) {
                /* Translate the mux to the ACCn values. */
                inst |= QPU_SET_FIELD(32 + dst.mux, QPU_WADDR_ADD);
        } else {
                inst |= QPU_SET_FIELD(dst.addr, QPU_WADDR_ADD);
                if (dst.mux == QPU_MUX_B)
                        inst |= QPU_WS;
        }

        return inst;
}

static uint64_t
qpu_m_dst(struct qpu_reg dst)
{
        uint64_t inst = 0;

        if (dst.mux <= QPU_MUX_R5) {
                /* Translate the mux to the ACCn values. */
                inst |= QPU_SET_FIELD(32 + dst.mux, QPU_WADDR_MUL);
        } else {
                inst |= QPU_SET_FIELD(dst.addr, QPU_WADDR_MUL);
                if (dst.mux == QPU_MUX_A)
                        inst |= QPU_WS;
        }

        return inst;
}

uint64_t
qpu_a_MOV(struct qpu_reg dst, struct qpu_reg src)
{
        uint64_t inst = 0;

        inst |= QPU_SET_FIELD(QPU_A_OR, QPU_OP_ADD);
        inst |= QPU_SET_FIELD(QPU_R_NOP, QPU_RADDR_A);
        inst |= QPU_SET_FIELD(QPU_R_NOP, QPU_RADDR_B);
        inst |= qpu_a_dst(dst);
        inst |= QPU_SET_FIELD(QPU_COND_ALWAYS, QPU_COND_ADD);
        inst |= QPU_SET_FIELD(src.mux, QPU_ADD_A);
        inst |= QPU_SET_FIELD(src.mux, QPU_ADD_B);
        inst = set_src_raddr(inst, src);
        inst |= QPU_SET_FIELD(QPU_SIG_NONE, QPU_SIG);
        inst |= QPU_SET_FIELD(QPU_W_NOP, QPU_WADDR_MUL);

        return inst;
}

uint64_t
qpu_m_MOV(struct qpu_reg dst, struct qpu_reg src)
{
        uint64_t inst = 0;

        inst |= QPU_SET_FIELD(QPU_M_V8MIN, QPU_OP_MUL);
        inst |= QPU_SET_FIELD(QPU_R_NOP, QPU_RADDR_A);
        inst |= QPU_SET_FIELD(QPU_R_NOP, QPU_RADDR_B);
        inst |= qpu_m_dst(dst);
        inst |= QPU_SET_FIELD(QPU_COND_ALWAYS, QPU_COND_MUL);
        inst |= QPU_SET_FIELD(src.mux, QPU_MUL_A);
        inst |= QPU_SET_FIELD(src.mux, QPU_MUL_B);
        inst = set_src_raddr(inst, src);
        inst |= QPU_SET_FIELD(QPU_SIG_NONE, QPU_SIG);
        inst |= QPU_SET_FIELD(QPU_W_NOP, QPU_WADDR_ADD);

        return inst;
}

uint64_t
qpu_load_imm_ui(struct qpu_reg dst, uint32_t val)
{
        uint64_t inst = 0;

        inst |= qpu_a_dst(dst);
        inst |= QPU_SET_FIELD(QPU_W_NOP, QPU_WADDR_MUL);
        inst |= QPU_SET_FIELD(QPU_COND_ALWAYS, QPU_COND_ADD);
        inst |= QPU_SET_FIELD(QPU_COND_ALWAYS, QPU_COND_MUL);
        inst |= QPU_SET_FIELD(QPU_SIG_LOAD_IMM, QPU_SIG);
        inst |= val;

        return inst;
}

uint64_t
qpu_a_alu2(enum qpu_op_add op,
           struct qpu_reg dst, struct qpu_reg src0, struct qpu_reg src1)
{
        uint64_t inst = 0;

        inst |= QPU_SET_FIELD(op, QPU_OP_ADD);
        inst |= QPU_SET_FIELD(QPU_R_NOP, QPU_RADDR_A);
        inst |= QPU_SET_FIELD(QPU_R_NOP, QPU_RADDR_B);
        inst |= qpu_a_dst(dst);
        inst |= QPU_SET_FIELD(QPU_COND_ALWAYS, QPU_COND_ADD);
        inst |= QPU_SET_FIELD(src0.mux, QPU_ADD_A);
        inst = set_src_raddr(inst, src0);
        inst |= QPU_SET_FIELD(src1.mux, QPU_ADD_B);
        inst = set_src_raddr(inst, src1);
        inst |= QPU_SET_FIELD(QPU_SIG_NONE, QPU_SIG);
        inst |= QPU_SET_FIELD(QPU_W_NOP, QPU_WADDR_MUL);

        return inst;
}

uint64_t
qpu_m_alu2(enum qpu_op_mul op,
           struct qpu_reg dst, struct qpu_reg src0, struct qpu_reg src1)
{
        uint64_t inst = 0;

        inst |= QPU_SET_FIELD(op, QPU_OP_MUL);
        inst |= QPU_SET_FIELD(QPU_R_NOP, QPU_RADDR_A);
        inst |= QPU_SET_FIELD(QPU_R_NOP, QPU_RADDR_B);
        inst |= qpu_m_dst(dst);
        inst |= QPU_SET_FIELD(QPU_COND_ALWAYS, QPU_COND_MUL);
        inst |= QPU_SET_FIELD(src0.mux, QPU_MUL_A);
        inst = set_src_raddr(inst, src0);
        inst |= QPU_SET_FIELD(src1.mux, QPU_MUL_B);
        inst = set_src_raddr(inst, src1);
        inst |= QPU_SET_FIELD(QPU_SIG_NONE, QPU_SIG);
        inst |= QPU_SET_FIELD(QPU_W_NOP, QPU_WADDR_ADD);

        return inst;
}

static bool
merge_fields(uint64_t *merge,
             uint64_t a, uint64_t b,
             uint64_t mask, uint64_t ignore)
{
        if ((a & mask) == ignore) {
                *merge = (*merge & ~mask) | (b & mask);
        } else if ((b & mask) == ignore) {
                *merge = (*merge & ~mask) | (a & mask);
        } else {
                if ((a & mask) != (b & mask))
                        return false;
        }

        return true;
}

int
qpu_num_sf_accesses(uint64_t inst)
{
        int accesses = 0;
        static const uint32_t specials[] = {
                QPU_W_TLB_COLOR_MS,
                QPU_W_TLB_COLOR_ALL,
                QPU_W_TLB_Z,
                QPU_W_TMU0_S,
                QPU_W_TMU0_T,
                QPU_W_TMU0_R,
                QPU_W_TMU0_B,
                QPU_W_TMU1_S,
                QPU_W_TMU1_T,
                QPU_W_TMU1_R,
                QPU_W_TMU1_B,
                QPU_W_SFU_RECIP,
                QPU_W_SFU_RECIPSQRT,
                QPU_W_SFU_EXP,
                QPU_W_SFU_LOG,
        };
        uint32_t waddr_add = QPU_GET_FIELD(inst, QPU_WADDR_ADD);
        uint32_t waddr_mul = QPU_GET_FIELD(inst, QPU_WADDR_MUL);
        uint32_t raddr_a = QPU_GET_FIELD(inst, QPU_RADDR_A);
        uint32_t raddr_b = QPU_GET_FIELD(inst, QPU_RADDR_B);

        for (int j = 0; j < ARRAY_SIZE(specials); j++) {
                if (waddr_add == specials[j])
                        accesses++;
                if (waddr_mul == specials[j])
                        accesses++;
        }

        if (raddr_a == QPU_R_MUTEX_ACQUIRE)
                accesses++;
        if (raddr_b == QPU_R_MUTEX_ACQUIRE)
                accesses++;

        /* XXX: semaphore, combined color read/write? */
        switch (QPU_GET_FIELD(inst, QPU_SIG)) {
        case QPU_SIG_COLOR_LOAD:
        case QPU_SIG_COLOR_LOAD_END:
        case QPU_SIG_LOAD_TMU0:
        case QPU_SIG_LOAD_TMU1:
                accesses++;
        }

        return accesses;
}

static bool
qpu_waddr_ignores_ws(uint32_t waddr)
{
        switch(waddr) {
        case QPU_W_ACC0:
        case QPU_W_ACC1:
        case QPU_W_ACC2:
        case QPU_W_ACC3:
        case QPU_W_TLB_Z:
        case QPU_W_TLB_COLOR_MS:
        case QPU_W_TLB_COLOR_ALL:
        case QPU_W_TLB_ALPHA_MASK:
        case QPU_W_VPM:
        case QPU_W_SFU_RECIP:
        case QPU_W_SFU_RECIPSQRT:
        case QPU_W_SFU_EXP:
        case QPU_W_SFU_LOG:
        case QPU_W_TMU0_S:
        case QPU_W_TMU0_T:
        case QPU_W_TMU0_R:
        case QPU_W_TMU0_B:
        case QPU_W_TMU1_S:
        case QPU_W_TMU1_T:
        case QPU_W_TMU1_R:
        case QPU_W_TMU1_B:
                return true;
        }

        return false;
}

static void
swap_ra_file_mux_helper(uint64_t *merge, uint64_t *a, uint32_t mux_shift)
{
        uint64_t mux_mask = (uint64_t)0x7 << mux_shift;
        uint64_t mux_a_val = (uint64_t)QPU_MUX_A << mux_shift;
        uint64_t mux_b_val = (uint64_t)QPU_MUX_B << mux_shift;

        if ((*a & mux_mask) == mux_a_val) {
                *a = (*a & ~mux_mask) | mux_b_val;
                *merge = (*merge & ~mux_mask) | mux_b_val;
        }
}

static bool
try_swap_ra_file(uint64_t *merge, uint64_t *a, uint64_t *b)
{
        uint32_t raddr_a_a = QPU_GET_FIELD(*a, QPU_RADDR_A);
        uint32_t raddr_a_b = QPU_GET_FIELD(*a, QPU_RADDR_B);
        uint32_t raddr_b_a = QPU_GET_FIELD(*b, QPU_RADDR_A);
        uint32_t raddr_b_b = QPU_GET_FIELD(*b, QPU_RADDR_B);

        if (raddr_a_b != QPU_R_NOP)
                return false;

        switch (raddr_a_a) {
        case QPU_R_UNIF:
        case QPU_R_VARY:
                break;
        default:
                return false;
        }

        if (raddr_b_b != QPU_R_NOP &&
            raddr_b_b != raddr_a_a)
                return false;

        /* Move raddr A to B in instruction a. */
        *a = (*a & ~QPU_RADDR_A_MASK) | QPU_SET_FIELD(QPU_R_NOP, QPU_RADDR_A);
        *a = (*a & ~QPU_RADDR_B_MASK) | QPU_SET_FIELD(raddr_a_a, QPU_RADDR_B);
        *merge = ((*merge & ~QPU_RADDR_A_MASK) | QPU_SET_FIELD(raddr_b_a, QPU_RADDR_A));
        *merge = ((*merge & ~QPU_RADDR_B_MASK) | QPU_SET_FIELD(raddr_a_a, QPU_RADDR_B));
        swap_ra_file_mux_helper(merge, a, QPU_ADD_A_SHIFT);
        swap_ra_file_mux_helper(merge, a, QPU_ADD_B_SHIFT);
        swap_ra_file_mux_helper(merge, a, QPU_MUL_A_SHIFT);
        swap_ra_file_mux_helper(merge, a, QPU_MUL_B_SHIFT);

        return true;
}

uint64_t
qpu_merge_inst(uint64_t a, uint64_t b)
{
        uint64_t merge = a | b;
        bool ok = true;

        if (QPU_GET_FIELD(a, QPU_OP_ADD) != QPU_A_NOP &&
            QPU_GET_FIELD(b, QPU_OP_ADD) != QPU_A_NOP)
                return 0;

        if (QPU_GET_FIELD(a, QPU_OP_MUL) != QPU_M_NOP &&
            QPU_GET_FIELD(b, QPU_OP_MUL) != QPU_M_NOP)
                return 0;

        if (qpu_num_sf_accesses(a) && qpu_num_sf_accesses(b))
                return 0;

        if (QPU_GET_FIELD(a, QPU_SIG) == QPU_SIG_LOAD_IMM ||
            QPU_GET_FIELD(b, QPU_SIG) == QPU_SIG_LOAD_IMM) {
                return 0;
        }

        ok = ok && merge_fields(&merge, a, b, QPU_SIG_MASK,
                                QPU_SET_FIELD(QPU_SIG_NONE, QPU_SIG));

        /* Misc fields that have to match exactly. */
        ok = ok && merge_fields(&merge, a, b, QPU_SF | QPU_PM,
                                ~0);

        if (!merge_fields(&merge, a, b, QPU_RADDR_A_MASK,
                          QPU_SET_FIELD(QPU_R_NOP, QPU_RADDR_A))) {
                /* Since we tend to use regfile A by default both for register
                 * allocation and for our special values (uniforms and
                 * varyings), try swapping uniforms and varyings to regfile B
                 * to resolve raddr A conflicts.
                 */
                if (!try_swap_ra_file(&merge, &a, &b) &&
                    !try_swap_ra_file(&merge, &b, &a)) {
                        return 0;
                }
        }

        ok = ok && merge_fields(&merge, a, b, QPU_RADDR_B_MASK,
                                QPU_SET_FIELD(QPU_R_NOP, QPU_RADDR_B));

        ok = ok && merge_fields(&merge, a, b, QPU_WADDR_ADD_MASK,
                                QPU_SET_FIELD(QPU_W_NOP, QPU_WADDR_ADD));
        ok = ok && merge_fields(&merge, a, b, QPU_WADDR_MUL_MASK,
                                QPU_SET_FIELD(QPU_W_NOP, QPU_WADDR_MUL));

        /* Allow disagreement on WS (swapping A vs B physical reg file as the
         * destination for ADD/MUL) if one of the original instructions
         * ignores it (probably because it's just writing to accumulators).
         */
        if (qpu_waddr_ignores_ws(QPU_GET_FIELD(a, QPU_WADDR_ADD)) &&
            qpu_waddr_ignores_ws(QPU_GET_FIELD(a, QPU_WADDR_MUL))) {
                merge = (merge & ~QPU_WS) | (b & QPU_WS);
        } else if (qpu_waddr_ignores_ws(QPU_GET_FIELD(b, QPU_WADDR_ADD)) &&
                   qpu_waddr_ignores_ws(QPU_GET_FIELD(b, QPU_WADDR_MUL))) {
                merge = (merge & ~QPU_WS) | (a & QPU_WS);
        } else {
                if ((a & QPU_WS) != (b & QPU_WS))
                        return 0;
        }

        if (ok)
                return merge;
        else
                return 0;
}

uint64_t
qpu_set_sig(uint64_t inst, uint32_t sig)
{
        assert(QPU_GET_FIELD(inst, QPU_SIG) == QPU_SIG_NONE);
        return (inst & ~QPU_SIG_MASK) | QPU_SET_FIELD(sig, QPU_SIG);
}

uint64_t
qpu_set_cond_add(uint64_t inst, uint32_t sig)
{
        assert(QPU_GET_FIELD(inst, QPU_COND_ADD) == QPU_COND_ALWAYS);
        return (inst & ~QPU_COND_ADD_MASK) | QPU_SET_FIELD(sig, QPU_COND_ADD);
}

uint64_t
qpu_set_cond_mul(uint64_t inst, uint32_t sig)
{
        assert(QPU_GET_FIELD(inst, QPU_COND_MUL) == QPU_COND_ALWAYS);
        return (inst & ~QPU_COND_MUL_MASK) | QPU_SET_FIELD(sig, QPU_COND_MUL);
}

bool
qpu_waddr_is_tlb(uint32_t waddr)
{
        switch (waddr) {
        case QPU_W_TLB_COLOR_ALL:
        case QPU_W_TLB_COLOR_MS:
        case QPU_W_TLB_Z:
                return true;
        default:
                return false;
        }
}

bool
qpu_inst_is_tlb(uint64_t inst)
{
        uint32_t sig = QPU_GET_FIELD(inst, QPU_SIG);

        return (qpu_waddr_is_tlb(QPU_GET_FIELD(inst, QPU_WADDR_ADD)) ||
                qpu_waddr_is_tlb(QPU_GET_FIELD(inst, QPU_WADDR_MUL)) ||
                sig == QPU_SIG_COLOR_LOAD ||
                sig == QPU_SIG_WAIT_FOR_SCOREBOARD);
}

void
qpu_serialize_one_inst(struct vc4_compile *c, uint64_t inst)
{
        if (c->qpu_inst_count >= c->qpu_inst_size) {
                c->qpu_inst_size = MAX2(16, c->qpu_inst_size * 2);
                c->qpu_insts = reralloc(c, c->qpu_insts,
                                        uint64_t, c->qpu_inst_size);
        }
        c->qpu_insts[c->qpu_inst_count++] = inst;
}
