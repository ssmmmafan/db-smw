/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "log_recovery.h"

#include <algorithm>
#include <cassert>
#include <string>
#include <unordered_set>

#include "errors.h"
#include "execution/executor_utils.h"
#include "record/rm_scan.h"

void RecoveryManager::analyze() {
    int off = 0;
    std::unordered_set<std::string> tables;
    while (true) {
        int len = disk_manager_->read_log(buffer_.buffer_, LOG_BUFFER_SIZE, off);
        if (len <= 0) {
            break;
        }
        int offset = 0;
        while (offset < len) {
            if (offset + OFFSET_LOG_TOT_LEN > len) {
                break;
            }
            auto log_tot_len = *reinterpret_cast<uint32_t *>(buffer_.buffer_ + offset + OFFSET_LOG_TOT_LEN);
            if (offset + static_cast<int>(log_tot_len) > len) {
                break;
            }
            LogType log_type = *reinterpret_cast<LogType *>(buffer_.buffer_ + offset);
            std::shared_ptr<LogRecord> log;
            switch (log_type) {
                case LogType::begin:
                    log = std::make_shared<BeginLogRecord>();
                    break;
                case LogType::commit:
                    log = std::make_shared<CommitLogRecord>();
                    break;
                case LogType::ABORT:
                    log = std::make_shared<AbortLogRecord>();
                    break;
                case LogType::INSERT:
                    log = std::make_shared<InsertLogRecord>();
                    break;
                case LogType::DELETE:
                    log = std::make_shared<DeleteLogRecord>();
                    break;
                case LogType::UPDATE:
                    log = std::make_shared<UpdateLogRecord>();
                    break;
                case LogType::INDEX_INSERT:
                    log = std::make_shared<IndexInsertLogRecord>();
                    break;
                case LogType::INDEX_DELETE:
                    log = std::make_shared<IndexDeleteLogRecord>();
                    break;
                default:
                    return;
            }
            log->deserialize(buffer_.buffer_ + offset);
            offset += static_cast<int>(log->log_tot_len_);
            logs_.push_back(log);
            max_txn_id_ = std::max(max_txn_id_, log->log_tid_);

            if (log_type == LogType::begin) {
                att_[log->log_tid_] = log->lsn_;
            } else if (log_type == LogType::commit || log_type == LogType::ABORT) {
                assert(att_.count(log->log_tid_));
                att_[log->log_tid_] = log->lsn_;
            } else {
                assert(att_.count(log->log_tid_));
                att_[log->log_tid_] = log->lsn_;
                if (log_type == LogType::INSERT) {
                    auto insert_log = std::dynamic_pointer_cast<InsertLogRecord>(log);
                    tables.insert(std::string(insert_log->table_name_));
                } else if (log_type == LogType::DELETE) {
                    auto delete_log = std::dynamic_pointer_cast<DeleteLogRecord>(log);
                    tables.insert(std::string(delete_log->table_name_));
                } else if (log_type == LogType::UPDATE) {
                    auto update_log = std::dynamic_pointer_cast<UpdateLogRecord>(log);
                    tables.insert(std::string(update_log->table_name_));
                }
            }
        }
        off += offset;
    }

    for (const auto &entry : att_) {
        if (entry.second >= 0 && static_cast<size_t>(entry.second) < logs_.size()) {
            const auto &last_log = logs_[entry.second];
            if (std::dynamic_pointer_cast<AbortLogRecord>(last_log)) {
                aborted_txns_.insert(entry.first);
            } else if (std::dynamic_pointer_cast<CommitLogRecord>(last_log)) {
                committed_txns_.insert(entry.first);
            }
        }
    }

    if (logs_.empty()) {
        return;
    }

    tables_with_dml_ = std::move(tables);
    for (const auto &tab_name : tables_with_dml_) {
        auto &tab = sm_manager_->db_.get_table(tab_name);
        for (const auto &index : tab.indexes) {
            auto ix_name = sm_manager_->get_ix_manager()->get_index_name(tab.name, index.cols);
            if (sm_manager_->ihs_.count(ix_name)) {
                sm_manager_->get_ix_manager()->close_index(sm_manager_->ihs_[ix_name].get());
                sm_manager_->ihs_.erase(ix_name);
            }
            sm_manager_->get_ix_manager()->destroy_index(tab.name, index.cols);
            sm_manager_->get_ix_manager()->create_index(tab.name, index.cols);
            sm_manager_->ihs_.emplace(ix_name, sm_manager_->get_ix_manager()->open_index(tab.name, index.cols));
        }
    }
}

void RecoveryManager::redo() {
    rollback(true);
    for (const auto &log_ : logs_) {
        if (aborted_txns_.count(log_->log_tid_) || !committed_txns_.count(log_->log_tid_)) {
            continue;
        }
        if (auto log = std::dynamic_pointer_cast<InsertLogRecord>(log_)) {
            const std::string table_name(log->table_name_);
            if (!sm_manager_->fhs_.count(table_name)) {
                continue;
            }
            auto rfh = sm_manager_->fhs_[table_name].get();
            try {
                rfh->insert_record(log->rid_, log->insert_value_.data);
            } catch (RMDBError &) {
                auto new_rid = rfh->insert_record(log->insert_value_.data, nullptr);
                assert(new_rid == log->rid_);
            }
        } else if (auto log = std::dynamic_pointer_cast<UpdateLogRecord>(log_)) {
            const std::string table_name(log->table_name_);
            if (!sm_manager_->fhs_.count(table_name)) {
                continue;
            }
            auto rfh = sm_manager_->fhs_[table_name].get();
            rfh->update_record(log->rid_, log->now_value_.data, nullptr);
        } else if (auto log = std::dynamic_pointer_cast<DeleteLogRecord>(log_)) {
            const std::string table_name(log->table_name_);
            if (!sm_manager_->fhs_.count(table_name)) {
                continue;
            }
            auto rfh = sm_manager_->fhs_[table_name].get();
            rfh->delete_record(log->rid_, nullptr);
        }
    }
}

void RecoveryManager::undo() {
    rollback(false);
    rebuild_indexes_from_table();
}

void RecoveryManager::rebuild_indexes_from_table() {
    for (const auto &tab_name : tables_with_dml_) {
        if (!sm_manager_->fhs_.count(tab_name)) {
            continue;
        }
        auto &tab = sm_manager_->db_.get_table(tab_name);
        auto rfh = sm_manager_->fhs_[tab_name].get();
        for (const auto &index : tab.indexes) {
            auto ix_name = sm_manager_->get_ix_manager()->get_index_name(tab.name, index.cols);
            if (sm_manager_->ihs_.count(ix_name)) {
                sm_manager_->get_ix_manager()->close_index(sm_manager_->ihs_[ix_name].get());
                sm_manager_->ihs_.erase(ix_name);
            }
            sm_manager_->get_ix_manager()->destroy_index(tab.name, index.cols);
            sm_manager_->get_ix_manager()->create_index(tab.name, index.cols);
            sm_manager_->ihs_.emplace(ix_name, sm_manager_->get_ix_manager()->open_index(tab.name, index.cols));
            auto ih = sm_manager_->ihs_.at(ix_name).get();
            for (RmScan scan(rfh); !scan.is_end(); scan.next()) {
                Rid rid = scan.rid();
                auto rec = rfh->get_record(rid, nullptr);
                auto key = build_index_key(index, rec->data);
                ih->insert_entry(key.get(), rid, nullptr);
            }
        }
    }
}

void RecoveryManager::rollback(bool redo_phase) {
    for (auto it = att_.rbegin(); it != att_.rend(); ++it) {
        lsn_t now = it->second;
        while (now != INVALID_LSN) {
            if (now < 0 || static_cast<size_t>(now) >= logs_.size()) {
                break;
            }
            const auto &log_ = logs_[now];
            if (auto log = std::dynamic_pointer_cast<InsertLogRecord>(log_)) {
                const std::string table_name(log->table_name_);
                if (!sm_manager_->fhs_.count(table_name)) {
                    now = log->prev_lsn_;
                    continue;
                }
                auto rfh = sm_manager_->fhs_[table_name].get();
                try {
                    rfh->delete_record(log->rid_, nullptr);
                } catch (RMDBError &) {
                }
                now = log->prev_lsn_;
            } else if (auto log = std::dynamic_pointer_cast<UpdateLogRecord>(log_)) {
                const std::string table_name(log->table_name_);
                if (!sm_manager_->fhs_.count(table_name)) {
                    now = log->prev_lsn_;
                    continue;
                }
                auto rfh = sm_manager_->fhs_[table_name].get();
                try {
                    rfh->update_record(log->rid_, log->update_value_.data, nullptr);
                } catch (RMDBError &) {
                }
                now = log->prev_lsn_;
            } else if (auto log = std::dynamic_pointer_cast<DeleteLogRecord>(log_)) {
                const std::string table_name(log->table_name_);
                if (!sm_manager_->fhs_.count(table_name)) {
                    now = log->prev_lsn_;
                    continue;
                }
                auto rfh = sm_manager_->fhs_[table_name].get();
                try {
                    rfh->insert_record(log->rid_, log->delete_value_.data);
                } catch (RMDBError &) {
                }
                now = log->prev_lsn_;
            } else if (auto log = std::dynamic_pointer_cast<IndexInsertLogRecord>(log_)) {
                if (!redo_phase) {
                    const std::string ix_name(log->ix_name_);
                    if (sm_manager_->ihs_.count(ix_name)) {
                        auto ih = sm_manager_->ihs_.at(ix_name).get();
                        try {
                            ih->delete_entry(log->key_, nullptr);
                        } catch (RMDBError &) {
                        }
                    }
                }
                now = log->prev_lsn_;
            } else if (auto log = std::dynamic_pointer_cast<IndexDeleteLogRecord>(log_)) {
                if (!redo_phase) {
                    const std::string ix_name(log->ix_name_);
                    if (sm_manager_->ihs_.count(ix_name)) {
                        auto ih = sm_manager_->ihs_.at(ix_name).get();
                        try {
                            ih->insert_entry(log->key_, log->rid_, nullptr);
                        } catch (RMDBError &) {
                        }
                    }
                }
                now = log->prev_lsn_;
            } else if (auto log = std::dynamic_pointer_cast<BeginLogRecord>(log_)) {
                now = log->prev_lsn_;
            } else if (auto log = std::dynamic_pointer_cast<CommitLogRecord>(log_)) {
                if (redo_phase) {
                    now = log->prev_lsn_;
                } else {
                    break;
                }
            } else if (auto log = std::dynamic_pointer_cast<AbortLogRecord>(log_)) {
                if (redo_phase) {
                    now = log->prev_lsn_;
                } else {
                    break;
                }
            } else {
                break;
            }
        }
    }
}
