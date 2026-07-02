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

#include <condition_variable>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

#include "transaction/transaction.h"
#include "index/ix_index_handle.h"

static const std::string GroupLockModeStr[10] = {"NON_LOCK", "IS", "IX", "S", "X", "SIX"};

class LockManager {
    enum class LockMode { SHARED, EXLUCSIVE, INTENTION_SHARED, INTENTION_EXCLUSIVE, S_IX };

    enum class GroupLockMode { NON_LOCK, IS, IX, S, X, SIX};

    class LockRequest {
    public:
        LockRequest(txn_id_t txn_id, LockMode lock_mode)
            : txn_id_(txn_id), lock_mode_(lock_mode), granted_(false) {}

        txn_id_t txn_id_;
        LockMode lock_mode_;
        bool granted_;
    };

    class LockRequestQueue {
    public:
        std::list<LockRequest> request_queue_;
        std::condition_variable cv_;
        GroupLockMode group_lock_mode_ = GroupLockMode::NON_LOCK;
    };

    static void update_group_lock_mode(LockRequestQueue &queue);

    void check_growing(Transaction *txn);

    bool lock_on_table(Transaction *txn, int tab_fd, LockMode lock_mode);

    bool lock_on_record(Transaction *txn, const Rid &rid, int tab_fd, LockMode lock_mode);

    bool lock_on_gap(Transaction *txn, int index_fd, const std::string &gap_key, LockMode lock_mode,
                     const std::vector<ColType> &col_types, const std::vector<int> &col_lens, bool is_range,
                     const char *lower, const char *upper, bool lower_open, bool upper_open);

    static LockMode combine_table_lock(LockMode held, LockMode req);

    static bool gap_lock_conflict(LockMode request, LockMode held, bool req_is_range, const char *req_lower,
                                  const char *req_upper, bool req_lower_open, bool req_upper_open,
                                  bool held_is_range, const std::string &held_gap_key,
                                  const std::vector<ColType> &col_types, const std::vector<int> &col_lens);

    static bool key_in_gap_range(const char *key, const char *lower, const char *upper, bool lower_open,
                                 bool upper_open, const std::vector<ColType> &col_types,
                                 const std::vector<int> &col_lens);

    static void decode_gap_range(const std::string &gap_key, std::string &lower, std::string &upper, bool &lower_open,
                                 bool &upper_open);

    static bool table_conflict(LockMode request, GroupLockMode held);

    static bool record_conflict(LockMode request, LockMode held);

public:
    LockManager() {}

    ~LockManager() {}

    bool lock_shared_on_record(Transaction *txn, const Rid &rid, int tab_fd);

    bool lock_exclusive_on_record(Transaction *txn, const Rid &rid, int tab_fd);

    bool lock_shared_on_table(Transaction *txn, int tab_fd);

    bool lock_exclusive_on_table(Transaction *txn, int tab_fd);

    bool lock_IS_on_table(Transaction *txn, int tab_fd);

    bool lock_IX_on_table(Transaction *txn, int tab_fd);

    bool lock_shared_on_gap_range(Transaction *txn, int index_fd, const std::vector<ColType> &col_types,
                                  const std::vector<int> &col_lens, const char *lower, const char *upper,
                                  bool lower_open, bool upper_open);

    bool lock_exclusive_on_gap_point(Transaction *txn, int index_fd, const std::vector<ColType> &col_types,
                                     const std::vector<int> &col_lens, const char *key, int key_len);

    bool unlock(Transaction *txn, LockDataId lock_data_id);

private:
    std::mutex latch_;
    std::unordered_map<LockDataId, LockRequestQueue> lock_table_;
};
