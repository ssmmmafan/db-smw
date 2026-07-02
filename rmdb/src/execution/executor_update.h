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

class UpdateExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;
    std::vector<Condition> conds_;
    RmFileHandle *fh_;
    std::vector<Rid> rids_;
    std::string tab_name_;
    std::vector<SetClause> set_clauses_;
    SmManager *sm_manager_;
    size_t idx_;

    void delete_index_entry(const RmRecord *rec, const Rid &rid, const IndexMeta &index) {
        auto ix_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols);
        auto ih = sm_manager_->ihs_.at(ix_name).get();
        auto key = build_index_key(index, rec->data);
        auto *index_log =
            new IndexDeleteLogRecord(context_->txn_->get_transaction_id(), key.get(), rid, ix_name, index.col_tot_len);
        index_log->prev_lsn_ = context_->txn_->get_prev_lsn();
        context_->log_mgr_->add_log_to_buffer(index_log);
        context_->txn_->set_prev_lsn(index_log->lsn_);
        ih->delete_entry(key.get(), context_->txn_);
    }

    void insert_index_entry(const RmRecord *rec, const Rid &rid, const IndexMeta &index) {
        auto ix_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols);
        auto ih = sm_manager_->ihs_.at(ix_name).get();
        auto key = build_index_key(index, rec->data);
        auto *index_log =
            new IndexInsertLogRecord(context_->txn_->get_transaction_id(), key.get(), rid, ix_name, index.col_tot_len);
        index_log->prev_lsn_ = context_->txn_->get_prev_lsn();
        context_->log_mgr_->add_log_to_buffer(index_log);
        context_->txn_->set_prev_lsn(index_log->lsn_);
        ih->insert_entry(key.get(), rid, context_->txn_);
    }

   public:
    UpdateExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<SetClause> set_clauses,
                   std::vector<Condition> conds, std::vector<Rid> rids, Context *context) {
        sm_manager_ = sm_manager;
        tab_name_ = tab_name;
        set_clauses_ = std::move(set_clauses);
        tab_ = sm_manager_->db_.get_table(tab_name);
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        conds_ = std::move(conds);
        rids_ = std::move(rids);
        context_ = context;
        idx_ = 0;
    }

    std::unique_ptr<RmRecord> Next() override {
        ensure_begin_logged(context_);
        std::vector<std::unique_ptr<RmRecord>> old_records;
        std::vector<std::unique_ptr<RmRecord>> new_records;
        old_records.reserve(rids_.size());
        new_records.reserve(rids_.size());

        for (const auto &rid : rids_) {
            auto old_rec = fh_->get_record(rid, context_);
            auto new_rec = std::make_unique<RmRecord>(old_rec->size);
            memcpy(new_rec->data, old_rec->data, old_rec->size);
            for (auto &set_clause : set_clauses_) {
                auto col = tab_.get_col(set_clause.lhs.col_name);
                memcpy(new_rec->data + col->offset, set_clause.rhs.raw->data, col->len);
            }
            old_records.push_back(std::move(old_rec));
            new_records.push_back(std::move(new_rec));
        }

        for (size_t rec_i = 0; rec_i < rids_.size(); rec_i++) {
            for (auto &index : tab_.indexes) {
                auto new_key = build_index_key(index, new_records[rec_i]->data);
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
                std::vector<Rid> result;
                if (ih->get_value(new_key.get(), &result, context_->txn_)) {
                    bool self = false;
                    for (const auto &rid : result) {
                        if (rid == rids_[rec_i]) {
                            self = true;
                        }
                    }
                    if (!self) {
                        std::vector<std::string> col_names;
                        for (const auto &col : index.cols) {
                            col_names.push_back(col.name);
                        }
                        throw IndexExistsError(tab_name_, col_names);
                    }
                }
            }
        }

        for (size_t rec_i = 0; rec_i < rids_.size(); rec_i++) {
            lock_exclusive_gap_for_record(context_, sm_manager_, tab_name_, tab_, old_records[rec_i]->data);
            lock_exclusive_gap_for_record(context_, sm_manager_, tab_name_, tab_, new_records[rec_i]->data);
            for (auto &index : tab_.indexes) {
                delete_index_entry(old_records[rec_i].get(), rids_[rec_i], index);
            }
            for (auto &index : tab_.indexes) {
                insert_index_entry(new_records[rec_i].get(), rids_[rec_i], index);
            }

            auto *log_record = new UpdateLogRecord(context_->txn_->get_transaction_id(), *old_records[rec_i],
                                                   rids_[rec_i], tab_name_, *new_records[rec_i]);
            log_record->prev_lsn_ = context_->txn_->get_prev_lsn();
            context_->log_mgr_->add_log_to_buffer(log_record);
            context_->txn_->set_prev_lsn(log_record->lsn_);

            if (context_->txn_ != nullptr) {
                context_->txn_->append_write_record(
                    new WriteRecord(WType::UPDATE_TUPLE, tab_name_, rids_[rec_i], *old_records[rec_i]));
            }
            fh_->update_record(rids_[rec_i], new_records[rec_i]->data, context_);
        }
        persist_wal(context_);
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
