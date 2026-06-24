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
#include "execution_defs.h"
#include "execution_manager.h"
#include "executor_abstract.h"
#include "index/ix.h"
#include "system/sm.h"

class ProjectionExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<ColMeta> cols_;
    size_t len_;
    std::vector<size_t> sel_idxs_;
    std::unique_ptr<RmRecord> record_;
    bool is_end_;

    std::unique_ptr<RmRecord> project(std::unique_ptr<RmRecord> prev_rec) {
        auto rec = std::make_unique<RmRecord>(len_);
        auto &prev_cols = prev_->cols();
        for (size_t i = 0; i < sel_idxs_.size(); i++) {
            auto &src_col = prev_cols[sel_idxs_[i]];
            memcpy(rec->data + cols_[i].offset, prev_rec->data + src_col.offset, src_col.len);
        }
        return rec;
    }

   public:
    ProjectionExecutor(std::unique_ptr<AbstractExecutor> prev, const std::vector<TabCol> &sel_cols) {
        prev_ = std::move(prev);
        size_t curr_offset = 0;
        auto &prev_cols = prev_->cols();
        for (auto &sel_col : sel_cols) {
            auto pos = get_col(prev_cols, sel_col);
            sel_idxs_.push_back(pos - prev_cols.begin());
            auto col = *pos;
            col.offset = curr_offset;
            curr_offset += col.len;
            cols_.push_back(col);
        }
        len_ = curr_offset;
        is_end_ = true;
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    bool is_end() const override { return is_end_; }

    void beginTuple() override {
        prev_->beginTuple();
        is_end_ = prev_->is_end();
        if (!is_end_) {
            record_ = project(prev_->Next());
        }
    }

    void nextTuple() override {
        prev_->nextTuple();
        is_end_ = prev_->is_end();
        if (!is_end_) {
            record_ = project(prev_->Next());
        }
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end_) {
            return nullptr;
        }
        return std::make_unique<RmRecord>(*record_);
    }

    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }

    Rid &rid() override { return _abstract_rid; }
};
