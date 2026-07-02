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
#include "executor_utils.h"
#include "index/ix.h"
#include "record/rm_scan.h"
#include "system/sm.h"

class SeqScanExecutor : public AbstractExecutor {
   private:
    std::string tab_name_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<ColMeta> cols_;
    size_t len_;
    std::vector<Condition> fed_conds_;

    Rid rid_;
    std::unique_ptr<RmScan> scan_;
    std::unique_ptr<RmRecord> record_;
    bool is_end_;

    SmManager *sm_manager_;

    void find_next() {
        is_end_ = true;
        if (!record_) {
            record_ = std::make_unique<RmRecord>(len_);
        }
        while (!scan_->is_end()) {
            rid_ = scan_->rid();
            fh_->get_record(rid_, record_.get(), context_);
            if (eval_conds(fed_conds_, record_.get(), cols_)) {
                is_end_ = false;
                return;
            }
            scan_->next();
        }
    }

   public:
    SeqScanExecutor(SmManager *sm_manager, std::string tab_name, std::vector<Condition> conds, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = std::move(tab_name);
        conds_ = std::move(conds);
        TabMeta &tab = sm_manager_->db_.get_table(tab_name_);
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab.cols;
        len_ = cols_.back().offset + cols_.back().len;
        context_ = context;
        fed_conds_ = conds_;
        is_end_ = true;
    }

    size_t tupleLen() const override { return len_; }

    const std::vector<ColMeta> &cols() const override { return cols_; }

    bool is_end() const override { return is_end_; }

    void beginTuple() override {
        if (context_ != nullptr && context_->txn_ != nullptr && context_->lock_mgr_ != nullptr &&
            context_->txn_->get_txn_mode()) {
            context_->lock_mgr_->lock_shared_on_table(context_->txn_, fh_->GetFd());
        }
        scan_ = std::make_unique<RmScan>(fh_);
        find_next();
    }

    void nextTuple() override {
        if (is_end_) {
            return;
        }
        scan_->next();
        find_next();
    }

    std::unique_ptr<RmRecord> Next() override {
        if (is_end_) {
            return nullptr;
        }
        return std::make_unique<RmRecord>(*record_);
    }

    const RmRecord *peekTuple() const override { return is_end_ ? nullptr : record_.get(); }

    ColMeta get_col_offset(const TabCol &target) override { return *get_col(cols_, target); }

    Rid &rid() override { return rid_; }
};
