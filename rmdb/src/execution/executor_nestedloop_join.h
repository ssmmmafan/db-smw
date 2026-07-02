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

#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/config.h"
#include "execution_defs.h"
#include "executor_abstract.h"
#include "index/ix_index_handle.h"

class NestedLoopJoinExecutor : public AbstractExecutor {
   private:
    struct JoinCondSpec {
        int lhs_offset;
        int rhs_offset;
        bool lhs_on_left;
        bool rhs_on_left;
        bool is_rhs_val;
        Value rhs_val;
        ColType lhs_type;
        int lhs_len;
        CompOp op;
    };

    std::unique_ptr<AbstractExecutor> left_;
    std::unique_ptr<AbstractExecutor> right_;
    size_t len_;
    size_t left_tuple_len_;
    size_t right_tuple_len_;
    size_t join_buffer_capacity_;
    std::vector<ColMeta> cols_;
    std::vector<ColMeta> left_cols_;
    std::vector<ColMeta> right_cols_;
    std::vector<JoinCondSpec> cond_specs_;

    std::vector<char> left_block_buf_;
    size_t left_block_count_;
    size_t block_idx_;
    std::unique_ptr<RmRecord> record_;
    bool is_end_;

    bool hash_probe_enabled_;
    std::unordered_map<std::string, std::vector<size_t>> eq_hash_;
    bool hash_block_ready_;
    std::vector<size_t> hash_match_indices_;
    size_t hash_match_pos_;

    static bool is_pure_equi_join(const std::vector<JoinCondSpec> &specs) {
        if (specs.empty()) {
            return false;
        }
        for (auto &spec : specs) {
            if (spec.op != OP_EQ || spec.is_rhs_val) {
                return false;
            }
        }
        return true;
    }

    void build_cond_specs(const std::vector<Condition> &conds) {
        for (auto &cond : conds) {
            JoinCondSpec spec{};
            const ColMeta *lhs_col = find_col(cols_, cond.lhs_col);
            if (lhs_col->offset < static_cast<int>(left_tuple_len_)) {
                spec.lhs_on_left = true;
                spec.lhs_offset = lhs_col->offset;
            } else {
                spec.lhs_on_left = false;
                spec.lhs_offset = lhs_col->offset - static_cast<int>(left_tuple_len_);
            }
            spec.lhs_type = lhs_col->type;
            spec.lhs_len = lhs_col->len;
            spec.op = cond.op;
            spec.is_rhs_val = cond.is_rhs_val;
            if (cond.is_rhs_val) {
                spec.rhs_val = cond.rhs_val;
            } else {
                const ColMeta *rhs_col = find_col(cols_, cond.rhs_col);
                if (rhs_col->offset < static_cast<int>(left_tuple_len_)) {
                    spec.rhs_on_left = true;
                    spec.rhs_offset = rhs_col->offset;
                } else {
                    spec.rhs_on_left = false;
                    spec.rhs_offset = rhs_col->offset - static_cast<int>(left_tuple_len_);
                }
            }
            cond_specs_.push_back(spec);
        }
    }

    void append_left_key(std::string &key, const char *left_data) const {
        for (auto &spec : cond_specs_) {
            int off = spec.lhs_on_left ? spec.lhs_offset : spec.rhs_offset;
            key.append(left_data + off, spec.lhs_len);
        }
    }

    void append_right_key(std::string &key, const char *right_data) const {
        for (auto &spec : cond_specs_) {
            int off = spec.lhs_on_left ? spec.rhs_offset : spec.lhs_offset;
            key.append(right_data + off, spec.lhs_len);
        }
    }

    bool eval_join_conds(const char *left_data, const char *right_data) const {
        for (auto &spec : cond_specs_) {
            const char *lhs_data =
                spec.lhs_on_left ? left_data + spec.lhs_offset : right_data + spec.lhs_offset;
            const char *rhs_data;
            if (spec.is_rhs_val) {
                rhs_data = spec.rhs_val.raw->data;
            } else {
                rhs_data = spec.rhs_on_left ? left_data + spec.rhs_offset : right_data + spec.rhs_offset;
            }
            int cmp = ix_compare(lhs_data, rhs_data, spec.lhs_type, spec.lhs_len);
            switch (spec.op) {
                case OP_EQ:
                    if (cmp != 0) return false;
                    break;
                case OP_NE:
                    if (cmp == 0) return false;
                    break;
                case OP_LT:
                    if (cmp >= 0) return false;
                    break;
                case OP_GT:
                    if (cmp <= 0) return false;
                    break;
                case OP_LE:
                    if (cmp > 0) return false;
                    break;
                case OP_GE:
                    if (cmp < 0) return false;
                    break;
                default:
                    throw InternalError("Unexpected comparison operator");
            }
        }
        return true;
    }

    const char *left_tuple_at(size_t idx) const {
        return left_block_buf_.data() + idx * left_tuple_len_;
    }

    std::unique_ptr<RmRecord> join_records(const char *lrec, const char *rrec) {
        auto rec = std::make_unique<RmRecord>(len_);
        memcpy(rec->data, lrec, left_tuple_len_);
        memcpy(rec->data + left_tuple_len_, rrec, right_tuple_len_);
        return rec;
    }

    void load_left_block() {
        left_block_buf_.clear();
        left_block_count_ = 0;
        if (left_block_buf_.capacity() < join_buffer_capacity_) {
            left_block_buf_.reserve(join_buffer_capacity_);
        }
        size_t used = 0;
        while (!left_->is_end()) {
            if (left_block_count_ > 0 && used + left_tuple_len_ > join_buffer_capacity_) {
                break;
            }
            const RmRecord *rec = left_->peekTuple();
            if (rec == nullptr) {
                break;
            }
            size_t old_size = left_block_buf_.size();
            left_block_buf_.resize(old_size + left_tuple_len_);
            memcpy(left_block_buf_.data() + old_size, rec->data, left_tuple_len_);
            left_block_count_++;
            used += left_tuple_len_;
            left_->nextTuple();
        }
    }

    void build_eq_hash() {
        eq_hash_.clear();
        std::string key;
        key.reserve(64);
        for (size_t idx = 0; idx < left_block_count_; ++idx) {
            key.clear();
            append_left_key(key, left_tuple_at(idx));
            eq_hash_[key].push_back(idx);
        }
    }

    void reset_hash_block() {
        hash_block_ready_ = false;
        eq_hash_.clear();
        hash_match_indices_.clear();
        hash_match_pos_ = 0;
        left_block_buf_.clear();
        left_block_count_ = 0;
    }

    bool emit_hash_match() {
        if (hash_match_pos_ >= hash_match_indices_.size()) {
            return false;
        }
        const RmRecord *right_rec = right_->peekTuple();
        if (right_rec == nullptr) {
            return false;
        }
        size_t left_idx = hash_match_indices_[hash_match_pos_++];
        record_ = join_records(left_tuple_at(left_idx), right_rec->data);
        is_end_ = false;
        return true;
    }

    void find_next_hash() {
        is_end_ = true;
        record_.reset();

        while (true) {
            if (emit_hash_match()) {
                return;
            }

            if (!hash_match_indices_.empty()) {
                hash_match_indices_.clear();
                hash_match_pos_ = 0;
                if (hash_block_ready_ && !right_->is_end()) {
                    right_->nextTuple();
                }
            }

            if (!hash_block_ready_) {
                load_left_block();
                if (left_block_count_ == 0) {
                    return;
                }
                build_eq_hash();
                right_->beginTuple();
                hash_block_ready_ = true;
            }

            if (right_->is_end()) {
                reset_hash_block();
                continue;
            }

            const RmRecord *right_rec = right_->peekTuple();
            if (right_rec == nullptr) {
                right_->nextTuple();
                continue;
            }

            std::string key;
            key.reserve(64);
            append_right_key(key, right_rec->data);
            auto it = eq_hash_.find(key);
            if (it != eq_hash_.end() && !it->second.empty()) {
                hash_match_indices_ = it->second;
                hash_match_pos_ = 0;
                continue;
            }
            right_->nextTuple();
        }
    }

    bool try_match() {
        if (block_idx_ >= left_block_count_ || right_->is_end()) {
            return false;
        }
        const RmRecord *right_rec = right_->peekTuple();
        if (right_rec == nullptr) {
            return false;
        }
        const char *left_data = left_tuple_at(block_idx_);
        if (cond_specs_.empty() || eval_join_conds(left_data, right_rec->data)) {
            record_ = join_records(left_data, right_rec->data);
            is_end_ = false;
            return true;
        }
        right_->nextTuple();
        return false;
    }

    void find_next_nested() {
        is_end_ = true;
        record_.reset();

        while (true) {
            if (left_block_count_ == 0) {
                load_left_block();
                if (left_block_count_ == 0) {
                    return;
                }
                block_idx_ = 0;
                right_->beginTuple();
            }

            while (block_idx_ < left_block_count_) {
                while (!right_->is_end()) {
                    if (try_match()) {
                        return;
                    }
                }
                block_idx_++;
                right_->beginTuple();
            }

            left_block_buf_.clear();
            left_block_count_ = 0;
            block_idx_ = 0;
        }
    }

    void find_next() {
        if (hash_probe_enabled_) {
            find_next_hash();
        } else {
            find_next_nested();
        }
    }

   public:
    NestedLoopJoinExecutor(std::unique_ptr<AbstractExecutor> left, std::unique_ptr<AbstractExecutor> right,
                           std::vector<Condition> conds) {
        left_ = std::move(left);
        right_ = std::move(right);
        left_tuple_len_ = left_->tupleLen();
        right_tuple_len_ = right_->tupleLen();
        len_ = left_tuple_len_ + right_tuple_len_;
        join_buffer_capacity_ = JOIN_BUFFER_SIZE;
        left_cols_ = left_->cols();
        right_cols_ = right_->cols();
        cols_ = left_cols_;
        for (auto col : right_cols_) {
            col.offset += static_cast<int>(left_tuple_len_);
            cols_.push_back(col);
        }
        build_cond_specs(conds);
        hash_probe_enabled_ = is_pure_equi_join(cond_specs_);
        block_idx_ = 0;
        left_block_count_ = 0;
        hash_block_ready_ = false;
        hash_match_pos_ = 0;
        is_end_ = true;
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    bool is_end() const override { return is_end_; }

    void beginTuple() override {
        left_->beginTuple();
        left_block_buf_.clear();
        left_block_count_ = 0;
        block_idx_ = 0;
        reset_hash_block();
        find_next();
    }

    void nextTuple() override {
        if (is_end_) {
            return;
        }
        if (!hash_probe_enabled_) {
            right_->nextTuple();
        }
        find_next();
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end_) {
            return nullptr;
        }
        return std::move(record_);
    }

    Rid &rid() override { return _abstract_rid; }
};
