/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "lock_manager.h"

#include <cstring>

namespace {

constexpr char kGapRangeTag = 'R';
constexpr char kGapPointTag = 'P';

std::string encode_gap_range(const char *lower, const char *upper, int key_len, bool lower_open, bool upper_open) {
    std::string key;
    key.push_back(kGapRangeTag);
    key.append(lower, key_len);
    key.push_back('\0');
    key.append(upper, key_len);
    key.push_back(static_cast<char>(lower_open ? 1 : 0));
    key.push_back(static_cast<char>(upper_open ? 1 : 0));
    return key;
}

std::string encode_gap_point(const char *point_key, int key_len) {
    std::string key;
    key.push_back(kGapPointTag);
    key.append(point_key, key_len);
    return key;
}

}  // namespace

LockManager::LockMode LockManager::combine_table_lock(LockMode held, LockMode req) {
    if (held == LockMode::EXLUCSIVE || req == LockMode::EXLUCSIVE) {
        return LockMode::EXLUCSIVE;
    }
    if (held == LockMode::S_IX || req == LockMode::S_IX) {
        return LockMode::S_IX;
    }
    if ((held == LockMode::SHARED && req == LockMode::INTENTION_EXCLUSIVE) ||
        (held == LockMode::INTENTION_EXCLUSIVE && req == LockMode::SHARED)) {
        return LockMode::S_IX;
    }
    if (held == LockMode::SHARED || req == LockMode::SHARED) {
        return LockMode::SHARED;
    }
    if (held == LockMode::INTENTION_EXCLUSIVE || req == LockMode::INTENTION_EXCLUSIVE) {
        return LockMode::INTENTION_EXCLUSIVE;
    }
    return LockMode::INTENTION_SHARED;
}

void LockManager::decode_gap_range(const std::string &gap_key, std::string &lower, std::string &upper, bool &lower_open,
                                   bool &upper_open) {
    lower.clear();
    upper.clear();
    lower_open = false;
    upper_open = false;
    if (gap_key.empty() || gap_key[0] != kGapRangeTag) {
        return;
    }
    auto split = gap_key.find('\0', 1);
    if (split == std::string::npos || gap_key.size() < split + 3) {
        return;
    }
    lower = gap_key.substr(1, split - 1);
    upper = gap_key.substr(split + 1, gap_key.size() - split - 3);
    lower_open = gap_key[gap_key.size() - 2] != 0;
    upper_open = gap_key[gap_key.size() - 1] != 0;
}

bool LockManager::key_in_gap_range(const char *key, const char *lower, const char *upper, bool lower_open,
                                   bool upper_open, const std::vector<ColType> &col_types,
                                   const std::vector<int> &col_lens) {
    int cmp_lower = ix_compare(lower, key, col_types, col_lens);
    int cmp_upper = ix_compare(key, upper, col_types, col_lens);
    bool ge_lower = lower_open ? cmp_lower < 0 : cmp_lower <= 0;
    bool lt_upper = upper_open ? cmp_upper < 0 : cmp_upper <= 0;
    return ge_lower && lt_upper;
}

bool LockManager::gap_lock_conflict(LockMode request, LockMode held, bool req_is_range, const char *req_lower,
                                    const char *req_upper, bool req_lower_open, bool req_upper_open,
                                    bool held_is_range, const std::string &held_gap_key,
                                    const std::vector<ColType> &col_types, const std::vector<int> &col_lens) {
    if (request == LockMode::SHARED && held == LockMode::SHARED) {
        return false;
    }

    if (req_is_range && !held_is_range) {
        const char *point = held_gap_key.c_str() + 1;
        return key_in_gap_range(point, req_lower, req_upper, req_lower_open, req_upper_open, col_types, col_lens);
    }
    if (!req_is_range && held_is_range) {
        std::string held_lower;
        std::string held_upper;
        bool held_lower_open = false;
        bool held_upper_open = false;
        decode_gap_range(held_gap_key, held_lower, held_upper, held_lower_open, held_upper_open);
        return key_in_gap_range(req_lower, held_lower.c_str(), held_upper.c_str(), held_lower_open, held_upper_open,
                                col_types, col_lens);
    }
    if (!req_is_range && !held_is_range) {
        const char *req_point = req_lower;
        const char *held_point = held_gap_key.c_str() + 1;
        return ix_compare(req_point, held_point, col_types, col_lens) == 0;
    }
    return false;
}

void LockManager::update_group_lock_mode(LockRequestQueue &queue) {
    queue.group_lock_mode_ = GroupLockMode::NON_LOCK;
    for (auto &req : queue.request_queue_) {
        if (!req.granted_) {
            continue;
        }
        switch (req.lock_mode_) {
            case LockMode::SHARED:
                if (queue.group_lock_mode_ == GroupLockMode::NON_LOCK) {
                    queue.group_lock_mode_ = GroupLockMode::S;
                } else if (queue.group_lock_mode_ == GroupLockMode::IS) {
                    queue.group_lock_mode_ = GroupLockMode::S;
                }
                break;
            case LockMode::EXLUCSIVE:
                queue.group_lock_mode_ = GroupLockMode::X;
                return;
            case LockMode::INTENTION_SHARED:
                if (queue.group_lock_mode_ == GroupLockMode::NON_LOCK) {
                    queue.group_lock_mode_ = GroupLockMode::IS;
                }
                break;
            case LockMode::INTENTION_EXCLUSIVE:
                if (queue.group_lock_mode_ == GroupLockMode::NON_LOCK) {
                    queue.group_lock_mode_ = GroupLockMode::IX;
                } else if (queue.group_lock_mode_ == GroupLockMode::IS) {
                    queue.group_lock_mode_ = GroupLockMode::IX;
                }
                break;
            case LockMode::S_IX:
                queue.group_lock_mode_ = GroupLockMode::SIX;
                return;
        }
    }
}

void LockManager::check_growing(Transaction *txn) {
    if (txn->get_state() == TransactionState::SHRINKING) {
        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::LOCK_ON_SHIRINKING);
    }
}

bool LockManager::table_conflict(LockMode request, GroupLockMode held) {
    switch (request) {
        case LockMode::INTENTION_SHARED:
            return held == GroupLockMode::X;
        case LockMode::INTENTION_EXCLUSIVE:
            return held == GroupLockMode::S || held == GroupLockMode::X || held == GroupLockMode::SIX;
        case LockMode::SHARED:
            return held == GroupLockMode::IX || held == GroupLockMode::X || held == GroupLockMode::SIX;
        case LockMode::EXLUCSIVE:
            return held != GroupLockMode::NON_LOCK;
        case LockMode::S_IX:
            return held == GroupLockMode::IS || held == GroupLockMode::IX || held == GroupLockMode::S ||
                   held == GroupLockMode::X || held == GroupLockMode::SIX;
    }
    return true;
}

bool LockManager::record_conflict(LockMode request, LockMode held) {
    if (request == LockMode::SHARED) {
        return held == LockMode::EXLUCSIVE;
    }
    return held == LockMode::SHARED || held == LockMode::EXLUCSIVE;
}

bool LockManager::lock_on_table(Transaction *txn, int tab_fd, LockMode lock_mode) {
    check_growing(txn);
    std::lock_guard<std::mutex> guard(latch_);
    txn->set_state(TransactionState::GROWING);

    LockDataId lock_data_id(tab_fd, LockDataType::TABLE);
    auto &queue = lock_table_[lock_data_id];

    for (auto &req : queue.request_queue_) {
        if (req.txn_id_ == txn->get_transaction_id() && req.granted_) {
            if (lock_mode == LockMode::EXLUCSIVE) {
                for (auto &other : queue.request_queue_) {
                    if (other.granted_ && other.txn_id_ != txn->get_transaction_id()) {
                        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
                    }
                }
                req.lock_mode_ = LockMode::EXLUCSIVE;
                queue.group_lock_mode_ = GroupLockMode::X;
                txn->get_lock_set()->insert(lock_data_id);
                return true;
            }
            LockMode combined = combine_table_lock(req.lock_mode_, lock_mode);
            req.lock_mode_ = combined;
            update_group_lock_mode(queue);
            txn->get_lock_set()->insert(lock_data_id);
            return true;
        }
    }

    for (auto &req : queue.request_queue_) {
        if (!req.granted_ && req.txn_id_ != txn->get_transaction_id()) {
            throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
        }
        if (req.granted_ && req.txn_id_ != txn->get_transaction_id() &&
            table_conflict(lock_mode, queue.group_lock_mode_)) {
            throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
        }
    }

    bool found = false;
    for (auto &req : queue.request_queue_) {
        if (req.txn_id_ == txn->get_transaction_id()) {
            req.lock_mode_ = combine_table_lock(req.lock_mode_, lock_mode);
            req.granted_ = true;
            found = true;
            break;
        }
    }
    if (!found) {
        queue.request_queue_.emplace_back(txn->get_transaction_id(), lock_mode);
        queue.request_queue_.back().granted_ = true;
    }
    update_group_lock_mode(queue);
    txn->get_lock_set()->insert(lock_data_id);
    return true;
}

bool LockManager::lock_on_record(Transaction *txn, const Rid &rid, int tab_fd, LockMode lock_mode) {
    check_growing(txn);
    std::lock_guard<std::mutex> guard(latch_);
    txn->set_state(TransactionState::GROWING);

    LockDataId table_id(tab_fd, LockDataType::TABLE);
    auto &table_queue = lock_table_[table_id];
    if (table_queue.group_lock_mode_ == GroupLockMode::X) {
        for (auto &req : table_queue.request_queue_) {
            if (req.granted_ && req.txn_id_ != txn->get_transaction_id()) {
                throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
            }
        }
    }
    if (lock_mode == LockMode::EXLUCSIVE &&
        (table_queue.group_lock_mode_ == GroupLockMode::S || table_queue.group_lock_mode_ == GroupLockMode::SIX)) {
        for (auto &req : table_queue.request_queue_) {
            if (req.granted_ && req.txn_id_ != txn->get_transaction_id()) {
                throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
            }
        }
    }

    LockDataId lock_data_id(tab_fd, rid, LockDataType::RECORD);
    auto &queue = lock_table_[lock_data_id];

    for (auto &req : queue.request_queue_) {
        if (req.txn_id_ == txn->get_transaction_id() && req.granted_) {
            if (lock_mode == LockMode::EXLUCSIVE && req.lock_mode_ == LockMode::SHARED) {
                for (auto &other : queue.request_queue_) {
                    if (other.granted_ && other.txn_id_ != txn->get_transaction_id()) {
                        throw TransactionAbortException(txn->get_transaction_id(), AbortReason::UPGRADE_CONFLICT);
                    }
                }
                req.lock_mode_ = LockMode::EXLUCSIVE;
                queue.group_lock_mode_ = GroupLockMode::X;
                txn->get_lock_set()->insert(lock_data_id);
                return true;
            }
            if (req.lock_mode_ == lock_mode) {
                txn->get_lock_set()->insert(lock_data_id);
                return true;
            }
        }
    }

    for (auto &req : queue.request_queue_) {
        if (!req.granted_ && req.txn_id_ != txn->get_transaction_id()) {
            throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
        }
        if (req.granted_ && req.txn_id_ != txn->get_transaction_id() &&
            record_conflict(lock_mode, req.lock_mode_)) {
            throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
        }
    }

    bool found = false;
    for (auto &req : queue.request_queue_) {
        if (req.txn_id_ == txn->get_transaction_id()) {
            req.lock_mode_ = lock_mode;
            req.granted_ = true;
            found = true;
            break;
        }
    }
    if (!found) {
        queue.request_queue_.emplace_back(txn->get_transaction_id(), lock_mode);
        queue.request_queue_.back().granted_ = true;
    }
    queue.group_lock_mode_ = (lock_mode == LockMode::SHARED) ? GroupLockMode::S : GroupLockMode::X;
    txn->get_lock_set()->insert(lock_data_id);
    return true;
}

bool LockManager::lock_shared_on_record(Transaction *txn, const Rid &rid, int tab_fd) {
    return lock_on_record(txn, rid, tab_fd, LockMode::SHARED);
}

bool LockManager::lock_exclusive_on_record(Transaction *txn, const Rid &rid, int tab_fd) {
    return lock_on_record(txn, rid, tab_fd, LockMode::EXLUCSIVE);
}

bool LockManager::lock_shared_on_table(Transaction *txn, int tab_fd) {
    return lock_on_table(txn, tab_fd, LockMode::SHARED);
}

bool LockManager::lock_exclusive_on_table(Transaction *txn, int tab_fd) {
    return lock_on_table(txn, tab_fd, LockMode::EXLUCSIVE);
}

bool LockManager::lock_IS_on_table(Transaction *txn, int tab_fd) {
    return lock_on_table(txn, tab_fd, LockMode::INTENTION_SHARED);
}

bool LockManager::lock_IX_on_table(Transaction *txn, int tab_fd) {
    return lock_on_table(txn, tab_fd, LockMode::INTENTION_EXCLUSIVE);
}

bool LockManager::lock_on_gap(Transaction *txn, int index_fd, const std::string &gap_key, LockMode lock_mode,
                              const std::vector<ColType> &col_types, const std::vector<int> &col_lens, bool is_range,
                              const char *lower, const char *upper, bool lower_open, bool upper_open) {
    check_growing(txn);
    std::lock_guard<std::mutex> guard(latch_);
    txn->set_state(TransactionState::GROWING);

    LockDataId lock_data_id(index_fd, gap_key);
    auto &queue = lock_table_[lock_data_id];

    for (auto &req : queue.request_queue_) {
        if (req.txn_id_ == txn->get_transaction_id() && req.granted_) {
            txn->get_lock_set()->insert(lock_data_id);
            return true;
        }
    }

    for (auto &[other_id, other_queue] : lock_table_) {
        if (other_id.type_ != LockDataType::GAP || other_id.fd_ != index_fd) {
            continue;
        }
        for (auto &req : other_queue.request_queue_) {
            if (!req.granted_ || req.txn_id_ == txn->get_transaction_id()) {
                continue;
            }
            bool held_is_range = !other_id.gap_key_.empty() && other_id.gap_key_[0] == kGapRangeTag;
            if (gap_lock_conflict(lock_mode, req.lock_mode_, is_range, lower, upper, lower_open, upper_open,
                                  held_is_range, other_id.gap_key_, col_types, col_lens)) {
                throw TransactionAbortException(txn->get_transaction_id(), AbortReason::DEADLOCK_PREVENTION);
            }
        }
    }

    queue.request_queue_.emplace_back(txn->get_transaction_id(), lock_mode);
    queue.request_queue_.back().granted_ = true;
    queue.group_lock_mode_ = (lock_mode == LockMode::SHARED) ? GroupLockMode::S : GroupLockMode::X;
    txn->get_lock_set()->insert(lock_data_id);
    return true;
}

bool LockManager::lock_shared_on_gap_range(Transaction *txn, int index_fd, const std::vector<ColType> &col_types,
                                           const std::vector<int> &col_lens, const char *lower, const char *upper,
                                           bool lower_open, bool upper_open) {
    int key_len = 0;
    for (int len : col_lens) {
        key_len += len;
    }
    std::string gap_key = encode_gap_range(lower, upper, key_len, lower_open, upper_open);
    return lock_on_gap(txn, index_fd, gap_key, LockMode::SHARED, col_types, col_lens, true, lower, upper, lower_open,
                       upper_open);
}

bool LockManager::lock_exclusive_on_gap_point(Transaction *txn, int index_fd, const std::vector<ColType> &col_types,
                                              const std::vector<int> &col_lens, const char *key, int key_len) {
    std::string gap_key = encode_gap_point(key, key_len);
    return lock_on_gap(txn, index_fd, gap_key, LockMode::EXLUCSIVE, col_types, col_lens, false, key, key, false, false);
}

bool LockManager::unlock(Transaction *txn, LockDataId lock_data_id) {
    std::lock_guard<std::mutex> guard(latch_);
    txn->set_state(TransactionState::SHRINKING);

    auto iter = lock_table_.find(lock_data_id);
    if (iter == lock_table_.end()) {
        return false;
    }

    auto &queue = iter->second;
    for (auto it = queue.request_queue_.begin(); it != queue.request_queue_.end(); ++it) {
        if (it->txn_id_ == txn->get_transaction_id()) {
            queue.request_queue_.erase(it);
            update_group_lock_mode(queue);
            txn->get_lock_set()->erase(lock_data_id);
            if (queue.request_queue_.empty()) {
                lock_table_.erase(iter);
            }
            return true;
        }
    }
    return false;
}
