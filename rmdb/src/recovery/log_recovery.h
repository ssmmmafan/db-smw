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

#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "log_manager.h"
#include "storage/disk_manager.h"
#include "system/sm_manager.h"

class RecoveryManager {
   public:
    RecoveryManager(DiskManager *disk_manager, BufferPoolManager *buffer_pool_manager, SmManager *sm_manager) {
        disk_manager_ = disk_manager;
        buffer_pool_manager_ = buffer_pool_manager;
        sm_manager_ = sm_manager;
    }

    void analyze();
    void redo();
    void undo();

    lsn_t get_next_lsn() const { return static_cast<lsn_t>(logs_.size()); }

    txn_id_t get_next_txn_id() const {
        return max_txn_id_ == INVALID_TXN_ID ? 0 : max_txn_id_ + 1;
    }

   private:
    std::map<txn_id_t, lsn_t> att_;
    std::unordered_set<txn_id_t> committed_txns_;
    std::unordered_set<txn_id_t> aborted_txns_;
    std::unordered_set<std::string> tables_with_dml_;
    std::vector<std::shared_ptr<LogRecord>> logs_;
    txn_id_t max_txn_id_ = INVALID_TXN_ID;
    LogBuffer buffer_;
    DiskManager *disk_manager_;
    BufferPoolManager *buffer_pool_manager_;
    SmManager *sm_manager_;

    void rollback(bool redo_phase);
    void rebuild_indexes_from_table();
};
