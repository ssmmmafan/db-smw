#pragma once

#include <cstring>
#include <vector>
#include "execution_defs.h"
#include "executor_abstract.h"
#include "index/ix_index_handle.h"

class AggregateExecutor : public AbstractExecutor {
   private:
    std::unique_ptr<AbstractExecutor> prev_;
    std::vector<SelItem> sel_items_;
    std::vector<ColMeta> cols_;
    size_t len_ = 0;
    std::unique_ptr<RmRecord> record_;
    bool is_end_ = true;
    bool computed_ = false;

    void build_output_schema() {
        cols_.clear();
        len_ = 0;
        auto &prev_cols = prev_->cols();
        for (auto &item : sel_items_) {
            ColMeta col;
            col.tab_name = "";
            col.name = item.alias;
            if (item.kind == SEL_AGG) {
                if (item.agg.is_star) {
                    col.type = TYPE_INT;
                    col.len = sizeof(int);
                } else {
                    auto src = get_col(prev_cols, item.agg.col);
                    if (item.agg.type == AGG_COUNT) {
                        col.type = TYPE_INT;
                        col.len = sizeof(int);
                    } else if (item.agg.type == AGG_SUM) {
                        col.type = src->type;
                        col.len = src->len;
                    } else {
                        col.type = src->type;
                        col.len = src->len;
                    }
                }
            } else {
                auto src = get_col(prev_cols, item.col);
                col.type = src->type;
                col.len = src->len;
            }
            col.offset = len_;
            len_ += col.len;
            cols_.push_back(col);
        }
    }

    static void update_max_min(char *cur, const char *val, ColType type, int len, bool is_max) {
        int cmp = ix_compare(val, cur, type, len);
        if ((is_max && cmp > 0) || (!is_max && cmp < 0)) {
            memcpy(cur, val, len);
        }
    }

    void compute() {
        build_output_schema();
        record_ = std::make_unique<RmRecord>(len_);
        memset(record_->data, 0, len_);

        std::vector<bool> has_value(sel_items_.size(), false);
        std::vector<int64_t> count_vals(sel_items_.size(), 0);
        std::vector<int64_t> sum_int_vals(sel_items_.size(), 0);
        std::vector<double> sum_float_vals(sel_items_.size(), 0.0);
        std::vector<std::vector<char>> extrema_bufs(sel_items_.size());

        auto &prev_cols = prev_->cols();
        for (size_t i = 0; i < sel_items_.size(); ++i) {
            if (sel_items_[i].kind == SEL_AGG && !sel_items_[i].agg.is_star &&
                (sel_items_[i].agg.type == AGG_MAX || sel_items_[i].agg.type == AGG_MIN)) {
                auto src = get_col(prev_cols, sel_items_[i].agg.col);
                extrema_bufs[i].assign(src->len, 0);
            }
        }

        prev_->beginTuple();
        while (!prev_->is_end()) {
            auto rec = prev_->Next();
            for (size_t i = 0; i < sel_items_.size(); ++i) {
                auto &item = sel_items_[i];
                if (item.kind != SEL_AGG) {
                    continue;
                }
                if (item.agg.type == AGG_COUNT) {
                    count_vals[i]++;
                    has_value[i] = true;
                    continue;
                }
                auto src = get_col(prev_cols, item.agg.col);
                char *val = rec->data + src->offset;
                switch (item.agg.type) {
                    case AGG_SUM:
                        if (src->type == TYPE_INT) {
                            sum_int_vals[i] += *(int *)val;
                        } else if (src->type == TYPE_FLOAT) {
                            sum_float_vals[i] += *(float *)val;
                        }
                        has_value[i] = true;
                        break;
                    case AGG_MAX:
                    case AGG_MIN: {
                        bool is_max = item.agg.type == AGG_MAX;
                        if (!has_value[i]) {
                            memcpy(extrema_bufs[i].data(), val, src->len);
                            has_value[i] = true;
                        } else {
                            update_max_min(extrema_bufs[i].data(), val, src->type, src->len, is_max);
                        }
                        break;
                    }
                    default:
                        break;
                }
            }
            prev_->nextTuple();
        }

        for (size_t i = 0; i < sel_items_.size(); ++i) {
            auto &item = sel_items_[i];
            if (item.kind != SEL_AGG) {
                continue;
            }
            char *out = record_->data + cols_[i].offset;
            switch (item.agg.type) {
                case AGG_COUNT:
                    *(int *)out = static_cast<int>(has_value[i] ? count_vals[i] : 0);
                    break;
                case AGG_SUM:
                    if (cols_[i].type == TYPE_INT) {
                        *(int *)out = static_cast<int>(sum_int_vals[i]);
                    } else {
                        *(float *)out = static_cast<float>(sum_float_vals[i]);
                    }
                    break;
                case AGG_MAX:
                case AGG_MIN:
                    memcpy(out, extrema_bufs[i].data(), cols_[i].len);
                    break;
                default:
                    break;
            }
        }
        computed_ = true;
        is_end_ = false;
    }

   public:
    AggregateExecutor(std::unique_ptr<AbstractExecutor> prev, std::vector<SelItem> sel_items)
        : prev_(std::move(prev)), sel_items_(std::move(sel_items)) {}

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    bool is_end() const override { return is_end_; }

    void beginTuple() override {
        if (!computed_) {
            compute();
        }
    }

    void nextTuple() override { is_end_ = true; }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end_) {
            return nullptr;
        }
        return std::make_unique<RmRecord>(*record_);
    }

    Rid &rid() override { return _abstract_rid; }
};
