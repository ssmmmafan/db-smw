/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include <limits>

#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "executor_utils.h"
#include "index/ix.h"
#include "system/sm.h"

class IndexScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;                      // 表名称
    TabMeta tab_;                               // 表的元数据
    std::vector<Condition> conds_;              // 扫描条件
    RmFileHandle *fh_;                          // 表的数据文件句柄
    std::vector<ColMeta> cols_;                 // 需要读取的字段
    size_t len_;                                // 选取出来的一条记录的长度
    std::vector<Condition> fed_conds_;          // 扫描条件，和conds_字段相同

    std::vector<std::string> index_col_names_;  // index scan涉及到的索引包含的字段
    IndexMeta index_meta_;                      // index scan涉及到的索引元数据

    Rid rid_;
    std::unique_ptr<RecScan> scan_;

    SmManager *sm_manager_;

    void fill_min(char *buf, const ColMeta &col) {
        if (col.type == TYPE_INT) {
            int v = std::numeric_limits<int>::min();
            memcpy(buf, &v, sizeof(int));
        } else if (col.type == TYPE_FLOAT) {
            float v = -std::numeric_limits<float>::max();
            memcpy(buf, &v, sizeof(float));
        } else if (col.type == TYPE_BIGINT || col.type == TYPE_DATETIME) {
            int64_t v = std::numeric_limits<int64_t>::min();
            memcpy(buf, &v, sizeof(int64_t));
        } else {
            memset(buf, 0, col.len);
        }
    }

    void fill_max(char *buf, const ColMeta &col) {
        if (col.type == TYPE_INT) {
            int v = std::numeric_limits<int>::max();
            memcpy(buf, &v, sizeof(int));
        } else if (col.type == TYPE_FLOAT) {
            float v = std::numeric_limits<float>::max();
            memcpy(buf, &v, sizeof(float));
        } else if (col.type == TYPE_BIGINT || col.type == TYPE_DATETIME) {
            int64_t v = std::numeric_limits<int64_t>::max();
            memcpy(buf, &v, sizeof(int64_t));
        } else {
            memset(buf, 0xff, col.len);
        }
    }

   public:
    IndexScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, std::vector<std::string> index_col_names,
                    Context *context) {
        sm_manager_ = sm_manager;
        context_ = context;
        tab_name_ = std::move(tab_name);
        tab_ = sm_manager_->db_.get_table(tab_name_);
        conds_ = std::move(conds);
        // index_no_ = index_no;
        index_col_names_ = index_col_names; 
        index_meta_ = *(tab_.get_index_meta(index_col_names_));
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab_.cols;
        len_ = cols_.back().offset + cols_.back().len;
        std::map<CompOp, CompOp> swap_op = {
            {OP_EQ, OP_EQ}, {OP_NE, OP_NE}, {OP_LT, OP_GT}, {OP_GT, OP_LT}, {OP_LE, OP_GE}, {OP_GE, OP_LE},
        };

        for (auto &cond : conds_) {
            if (cond.lhs_col.tab_name != tab_name_) {
                // lhs is on other table, now rhs must be on this table
                assert(!cond.is_rhs_val && cond.rhs_col.tab_name == tab_name_);
                // swap lhs and rhs
                std::swap(cond.lhs_col, cond.rhs_col);
                cond.op = swap_op.at(cond.op);
            }
        }
        fed_conds_ = conds_;
    }

    void beginTuple() override {
        auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index_meta_.cols)).get();
        std::vector<char> lower(index_meta_.col_tot_len);
        std::vector<char> upper(index_meta_.col_tot_len);
        int offset = 0;
        bool stop_prefix = false;
        bool lower_open = false;
        bool upper_open = false;
        for (const auto &col : index_meta_.cols) {
            fill_min(lower.data() + offset, col);
            fill_max(upper.data() + offset, col);
            if (!stop_prefix) {
                const Condition *eq = nullptr;
                const Condition *lower_cond = nullptr;
                const Condition *upper_cond = nullptr;
                for (const auto &cond : fed_conds_) {
                    if (!cond.is_rhs_val || cond.lhs_col.col_name != col.name || cond.lhs_col.tab_name != tab_name_) {
                        continue;
                    }
                    if (cond.op == OP_EQ) {
                        eq = &cond;
                    } else if (cond.op == OP_GT || cond.op == OP_GE) {
                        lower_cond = &cond;
                    } else if (cond.op == OP_LT || cond.op == OP_LE) {
                        upper_cond = &cond;
                    }
                }
                if (eq != nullptr) {
                    memcpy(lower.data() + offset, eq->rhs_val.raw->data, col.len);
                    memcpy(upper.data() + offset, eq->rhs_val.raw->data, col.len);
                } else {
                    if (lower_cond != nullptr) {
                        memcpy(lower.data() + offset, lower_cond->rhs_val.raw->data, col.len);
                        lower_open = lower_cond->op == OP_GT;
                    }
                    if (upper_cond != nullptr) {
                        memcpy(upper.data() + offset, upper_cond->rhs_val.raw->data, col.len);
                        upper_open = upper_cond->op == OP_LT;
                    }
                    stop_prefix = true;
                }
            }
            offset += col.len;
        }
        if (context_ != nullptr && context_->txn_ != nullptr && context_->lock_mgr_ != nullptr &&
            context_->txn_->get_txn_mode()) {
            std::vector<ColType> col_types;
            std::vector<int> col_lens;
            index_col_meta(index_meta_, col_types, col_lens);
            context_->lock_mgr_->lock_shared_on_gap_range(context_->txn_, ih->get_fd(), col_types, col_lens,
                                                          lower.data(), upper.data(), lower_open, upper_open);
        }
        Iid lower_iid = lower_open ? ih->upper_bound(lower.data()) : ih->lower_bound(lower.data());
        Iid upper_iid = upper_open ? ih->lower_bound(upper.data()) : ih->upper_bound(upper.data());
        scan_ = std::make_unique<IxScan>(ih, lower_iid, upper_iid, sm_manager_->get_bpm());
        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            auto rec = fh_->get_record(rid_, context_);
            if (eval_conditions(tab_, *rec, fed_conds_)) {
                return;
            }
            scan_->next();
        }
    }

    void nextTuple() override {
        if (scan_ == nullptr || scan_->is_end()) {
            return;
        }
        scan_->next();
        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            auto rec = fh_->get_record(rid_, context_);
            if (eval_conditions(tab_, *rec, fed_conds_)) {
                return;
            }
            scan_->next();
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        return fh_->get_record(rid_, context_);
    }

    bool is_end() const override { return scan_ == nullptr || scan_->is_end(); }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    size_t tupleLen() const override { return len_; }

    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }

    Rid &rid() override { return rid_; }
};
