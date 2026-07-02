/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "transaction_manager.h"

#include <cstring>
#include <vector>

#include "execution/executor_utils.h"
#include "index/ix.h"
#include "record/rm_file_handle.h"
#include "system/sm_manager.h"

std::unordered_map<txn_id_t, Transaction *> TransactionManager::txn_map = {};

static void undo_write_record(SmManager *sm_manager, WriteRecord *write_record) {
    const std::string &tab_name = write_record->GetTableName();
    Rid rid = write_record->GetRid();
    auto &tab = sm_manager->db_.get_table(tab_name);
    auto fh = sm_manager->fhs_.at(tab_name).get();

    switch (write_record->GetWriteType()) {
        case WType::INSERT_TUPLE: {
            auto rec = fh->get_record(rid, nullptr);
            for (auto &index : tab.indexes) {
                auto key = build_index_key(index, rec->data);
                auto ih = sm_manager->ihs_
                              .at(sm_manager->get_ix_manager()->get_index_name(tab_name, index.cols))
                              .get();
                ih->delete_entry(key.get(), nullptr);
            }
            fh->delete_record(rid, nullptr);
            break;
        }
        case WType::DELETE_TUPLE: {
            RmRecord &record = write_record->GetRecord();
            fh->insert_record(rid, record.data);
            for (auto &index : tab.indexes) {
                auto key = build_index_key(index, record.data);
                auto ih = sm_manager->ihs_
                              .at(sm_manager->get_ix_manager()->get_index_name(tab_name, index.cols))
                              .get();
                ih->insert_entry(key.get(), rid, nullptr);
            }
            break;
        }
        case WType::UPDATE_TUPLE: {
            RmRecord &old_rec = write_record->GetRecord();
            auto cur_rec = fh->get_record(rid, nullptr);
            for (auto &index : tab.indexes) {
                auto old_key = build_index_key(index, old_rec.data);
                auto new_key = build_index_key(index, cur_rec->data);
                auto ih = sm_manager->ihs_
                              .at(sm_manager->get_ix_manager()->get_index_name(tab_name, index.cols))
                              .get();
                if (memcmp(old_key.get(), new_key.get(), index.col_tot_len) != 0) {
                    ih->delete_entry(new_key.get(), nullptr);
                    ih->insert_entry(old_key.get(), rid, nullptr);
                }
            }
            fh->update_record(rid, old_rec.data, nullptr);
            break;
        }
    }
}

static void clear_write_set(const std::shared_ptr<std::deque<WriteRecord *>> &write_set) {
    while (!write_set->empty()) {
        delete write_set->back();
        write_set->pop_back();
    }
}

static void release_all_locks(LockManager *lock_manager, Transaction *txn) {
    if (lock_manager == nullptr || txn == nullptr) {
        return;
    }
    auto lock_set = txn->get_lock_set();
    std::vector<LockDataId> locks(lock_set->begin(), lock_set->end());
    for (auto &lock_id : locks) {
        lock_manager->unlock(txn, lock_id);
    }
    lock_set->clear();
}

Transaction *TransactionManager::begin(Transaction *txn, LogManager *log_manager) {
    if (txn == nullptr) {
        txn_id_t txn_id = next_txn_id_.fetch_add(1);
        txn = new Transaction(txn_id);
        txn->set_start_ts(next_timestamp_.fetch_add(1));
        std::lock_guard<std::mutex> lock(latch_);
        txn_map[txn_id] = txn;
    }
    txn->set_state(TransactionState::GROWING);
    return txn;
}

void TransactionManager::ensure_txn_logged(Transaction *txn, LogManager *log_manager) {
    if (txn == nullptr || log_manager == nullptr || txn->get_prev_lsn() != INVALID_LSN) {
        return;
    }
    auto *log = new BeginLogRecord(txn->get_transaction_id());
    log->prev_lsn_ = txn->get_prev_lsn();
    log_manager->add_log_to_buffer(log);
    txn->set_prev_lsn(log->lsn_);
}

void TransactionManager::commit(Transaction *txn, LogManager *log_manager) {
    if (txn == nullptr) {
        return;
    }
    bool had_writes = !txn->get_write_set()->empty();
    if (log_manager != nullptr && had_writes) {
        ensure_txn_logged(txn, log_manager);
        auto *log = new CommitLogRecord(txn->get_transaction_id());
        log->prev_lsn_ = txn->get_prev_lsn();
        log_manager->add_log_to_buffer(log);
        txn->set_prev_lsn(log->lsn_);
        log_manager->flush_log_to_disk();
    }
    clear_write_set(txn->get_write_set());
    release_all_locks(lock_manager_, txn);
    if (had_writes && sm_manager_ != nullptr && sm_manager_->get_bpm() != nullptr) {
        sm_manager_->get_bpm()->flush_all_dirty_pages();
    }
    txn->set_state(TransactionState::COMMITTED);
}

void TransactionManager::abort(Transaction *txn, LogManager *log_manager) {
    if (txn == nullptr) {
        return;
    }
    auto write_set = txn->get_write_set();
    bool had_writes = !write_set->empty();
    while (!write_set->empty()) {
        WriteRecord *write_record = write_set->back();
        write_set->pop_back();
        undo_write_record(sm_manager_, write_record);
        delete write_record;
    }
    if (log_manager != nullptr && had_writes) {
        auto *log = new AbortLogRecord(txn->get_transaction_id());
        log->prev_lsn_ = txn->get_prev_lsn();
        log_manager->add_log_to_buffer(log);
        txn->set_prev_lsn(log->lsn_);
        log_manager->flush_log_to_disk();
    }
    release_all_locks(lock_manager_, txn);
    txn->set_state(TransactionState::ABORTED);
}
