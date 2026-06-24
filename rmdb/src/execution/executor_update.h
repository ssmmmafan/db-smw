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
                auto old_key = build_index_key(index, old_records[rec_i]->data);
                auto new_key = build_index_key(index, new_records[rec_i]->data);
                if (memcmp(old_key.get(), new_key.get(), index.col_tot_len) == 0) {
                    continue;
                }
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
            std::vector<std::unique_ptr<char[]>> old_keys;
            std::vector<std::unique_ptr<char[]>> new_keys;
            for (auto &index : tab_.indexes) {
                old_keys.push_back(build_index_key(index, old_records[rec_i]->data));
                new_keys.push_back(build_index_key(index, new_records[rec_i]->data));
            }
            for (size_t idx_i = 0; idx_i < tab_.indexes.size(); idx_i++) {
                auto &index = tab_.indexes[idx_i];
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
                if (memcmp(old_keys[idx_i].get(), new_keys[idx_i].get(), index.col_tot_len) != 0) {
                    ih->delete_entry(old_keys[idx_i].get(), context_->txn_);
                }
            }
            fh_->update_record(rids_[rec_i], new_records[rec_i]->data, context_);
            for (size_t idx_i = 0; idx_i < tab_.indexes.size(); idx_i++) {
                auto &index = tab_.indexes[idx_i];
                auto ih = sm_manager_->ihs_.at(sm_manager_->get_ix_manager()->get_index_name(tab_name_, index.cols)).get();
                if (memcmp(old_keys[idx_i].get(), new_keys[idx_i].get(), index.col_tot_len) != 0) {
                    ih->insert_entry(new_keys[idx_i].get(), rids_[rec_i], context_->txn_);
                }
            }
        }
        return nullptr;
    }

    Rid &rid() override { return _abstract_rid; }
};
