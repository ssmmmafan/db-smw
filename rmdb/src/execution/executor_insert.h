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

class InsertExecutor : public AbstractExecutor {
   private:
    TabMeta tab_;
    std::vector<Value> values_;
    RmFileHandle *fh_;
    std::string tab_name_;
    Rid rid_;
    SmManager *sm_manager_;

    void log_index_insert(const RmRecord &rec, const IndexMeta &index, const char *key) {
        auto ix_name = sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols);
        auto *index_log =
            new IndexInsertLogRecord(context_->txn_->get_transaction_id(), const_cast<char *>(key), rid_, ix_name,
                                     index.col_tot_len);
        index_log->prev_lsn_ = context_->txn_->get_prev_lsn();
        context_->log_mgr_->add_log_to_buffer(index_log);
        context_->txn_->set_prev_lsn(index_log->lsn_);
    }

   public:
    InsertExecutor(SmManager *sm_manager, const std::string &tab_name, std::vector<Value> values, Context *context) {
        sm_manager_ = sm_manager;
        tab_ = sm_manager_->db_.get_table(tab_name);
        values_ = values;
        tab_name_ = tab_name;
        if (values.size() != tab_.cols.size()) {
            throw InvalidValueCountError();
        }
        fh_ = sm_manager_->fhs_.at(tab_name).get();
        context_ = context;
    };

    std::unique_ptr<RmRecord> Next() override {
        RmRecord rec(fh_->get_file_hdr().record_size);
        for (size_t i = 0; i < values_.size(); i++) {
            auto &col = tab_.cols[i];
            auto &val = values_[i];
            if (col.type != val.type) {
                throw IncompatibleTypeError(coltype2str(col.type), coltype2str(val.type));
            }
            val.init_raw(col.len);
            memcpy(rec.data + col.offset, val.raw->data, col.len);
        }
        std::vector<std::unique_ptr<char[]>> keys;
        for (auto &index : tab_.indexes) {
            auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
            auto key = build_index_key(index, rec.data);
            std::vector<Rid> result;
            if (ih->get_value(key.get(), &result, context_->txn_)) {
                std::vector<std::string> col_names;
                for (const auto &col : index.cols) {
                    col_names.push_back(col.name);
                }
                throw IndexExistsError(tab_name_, col_names);
            }
            keys.push_back(std::move(key));
        }

        lock_exclusive_gap_for_record(context_, sm_manager_, tab_name_, tab_, rec.data);
        ensure_begin_logged(context_);
        rid_ = fh_->insert_record(rec.data, context_);

        auto *log_record = new InsertLogRecord(context_->txn_->get_transaction_id(), rec, rid_, tab_name_);
        log_record->prev_lsn_ = context_->txn_->get_prev_lsn();
        context_->log_mgr_->add_log_to_buffer(log_record);
        context_->txn_->set_prev_lsn(log_record->lsn_);

        for (size_t i = 0; i < tab_.indexes.size(); ++i) {
            auto &index = tab_.indexes[i];
            auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
            log_index_insert(rec, index, keys[i].get());
            ih->insert_entry(keys[i].get(), rid_, context_->txn_);
        }
        if (context_->txn_ != nullptr) {
            context_->txn_->append_write_record(new WriteRecord(WType::INSERT_TUPLE, tab_name_, rid_));
        }
        persist_wal(context_);
        return nullptr;
    }
    Rid &rid() override { return rid_; }
};
