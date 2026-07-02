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
#include "recovery/log_manager.h"
#include "system/sm.h"

class DeleteExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    SmManager *sm_manager_;
    size_t idx_;

    void delete_index(const RmRecord *rec, const Rid &rid) {
        for (auto &index : tab_.indexes) {
            auto ix_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols);
            auto ih = sm_manager_->ihs_.at(ix_name).get();
            auto key = build_index_key(index, rec->data);
            auto *index_log = new IndexDeleteLogRecord(context_->txn_->get_transaction_id(), key.get(), rid, ix_name,
                                                       index.col_tot_len);
            index_log->prev_lsn_ = context_->txn_->get_prev_lsn();
            context_->log_mgr_->add_log_to_buffer(index_log);
            context_->txn_->set_prev_lsn(index_log->lsn_);
            ih->delete_entry(key.get(), context_->txn_);
        }
    }

   public:
    DeleteExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<Condition> conds,
                   std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = std::move(conds);
        rids_ = std::move(rids);
        context_ = context;
        idx_ = 0;
    }

    std::unique_ptr<RmRecord> Next() override {
        while (idx_ < rids_.size()) {
            Rid rid = rids_[idx_++];
            auto rec = fh_->get_record(rid, context_);
            lock_exclusive_gap_for_record(context_, sm_manager_, tab_name_, tab_, rec->data);
            ensure_begin_logged(context_);

            auto *log_record = new DeleteLogRecord(context_->txn_->get_transaction_id(), *rec, rid, tab_name_);
            log_record->prev_lsn_ = context_->txn_->get_prev_lsn();
            context_->log_mgr_->add_log_to_buffer(log_record);
            context_->txn_->set_prev_lsn(log_record->lsn_);

            fh_->delete_record(rid, context_);
            delete_index(rec.get(), rid);
            if (context_->txn_ != nullptr) {
                context_->txn_->append_write_record(new WriteRecord(WType::DELETE_TUPLE, tab_name_, rid, *rec));
            }
        }
        persist_wal(context_);
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
