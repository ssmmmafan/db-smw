/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2. */

#pragma once

#include <memory>
#include <vector>

#include "common/common.h"
#include "index/ix_index_handle.h"
#include "record/rm_defs.h"
#include "system/sm_meta.h"

inline int compare_raw_value(const char *lhs, const char *rhs, ColType type, int len) {
    return ix_compare(lhs, rhs, type, len);
}

inline bool eval_compare(int cmp, CompOp op) {
    switch (op) {
        case OP_EQ:
            return cmp == 0;
        case OP_NE:
            return cmp != 0;
        case OP_LT:
            return cmp < 0;
        case OP_GT:
            return cmp > 0;
        case OP_LE:
            return cmp <= 0;
        case OP_GE:
            return cmp >= 0;
    }
    return false;
}

inline bool eval_condition(const TabMeta &tab, const RmRecord &rec, const Condition &cond) {
    auto lhs_col = tab.get_col(cond.lhs_col.col_name);
    const char *lhs = rec.data + lhs_col->offset;
    const char *rhs = nullptr;
    int rhs_len = lhs_col->len;
    if (cond.is_rhs_val) {
        rhs = cond.rhs_val.raw->data;
    } else {
        auto rhs_col = tab.get_col(cond.rhs_col.col_name);
        rhs = rec.data + rhs_col->offset;
        rhs_len = rhs_col->len;
    }
    int cmp = compare_raw_value(lhs, rhs, lhs_col->type, rhs_len);
    return eval_compare(cmp, cond.op);
}

inline bool eval_conditions(const TabMeta &tab, const RmRecord &rec, const std::vector<Condition> &conds) {
    for (const auto &cond : conds) {
        if (!eval_condition(tab, rec, cond)) {
            return false;
        }
    }
    return true;
}

inline std::unique_ptr<char[]> build_index_key(const IndexMeta &index, const char *record_data) {
    auto key = std::make_unique<char[]>(index.col_tot_len);
    int offset = 0;
    for (const auto &col : index.cols) {
        memcpy(key.get() + offset, record_data + col.offset, col.len);
        offset += col.len;
    }
    return key;
}
