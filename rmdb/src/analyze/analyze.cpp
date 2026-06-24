/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "analyze.h"
#include "common/datetime_util.h"
#include <climits>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace {

int compare_pos_int_string(const std::string &a, const std::string &b) {
    if (a.length() != b.length()) {
        return a.length() < b.length() ? -1 : 1;
    }
    return a.compare(b);
}

int compare_int_string(const std::string &a, const std::string &b) {
    if (a.empty() || b.empty()) {
        return a.compare(b);
    }
    if (a[0] == '-' && b[0] != '-') {
        return -1;
    }
    if (a[0] != '-' && b[0] == '-') {
        return 1;
    }
    if (a[0] == '-' && b[0] == '-') {
        return -compare_pos_int_string(a.substr(1), b.substr(1));
    }
    const std::string &pa = (a[0] == '+') ? a.substr(1) : a;
    const std::string &pb = (b[0] == '+') ? b.substr(1) : b;
    return compare_pos_int_string(pa, pb);
}

template <typename T>
bool no_overflow(const std::string &value) {
    return compare_int_string(value, std::to_string(std::numeric_limits<T>::max())) <= 0 &&
           compare_int_string(value, std::to_string(std::numeric_limits<T>::lowest())) >= 0;
}

}  // namespace

/**
 * @description: 分析器，进行语义分析和查询重写，需要检查不符合语义规定的部分
 * @param {shared_ptr<ast::TreeNode>} parse parser生成的结果集
 * @return {shared_ptr<Query>} Query 
 */
std::shared_ptr<Query> Analyze::do_analyze(std::shared_ptr<ast::TreeNode> parse)
{
    std::shared_ptr<Query> query = std::make_shared<Query>();
    if (auto x = std::dynamic_pointer_cast<ast::SelectStmt>(parse))
    {
        // 处理表名
        query->tables = std::move(x->tabs);
        for (auto &tab : query->tables) {
            if (!sm_manager_->db_.is_table(tab)) {
                throw TableNotFoundError(tab);
            }
        }

        // 处理target list
        std::vector<ColMeta> all_cols;
        get_all_cols(query->tables, all_cols);
        if (x->select_items.empty()) {
            for (auto &col : all_cols) {
                TabCol sel_col = {.tab_name = col.tab_name, .col_name = col.name};
                query->cols.push_back(sel_col);
            }
        } else {
            for (auto &sv_item : x->select_items) {
                SelItem item;
                if (auto sv_col = std::dynamic_pointer_cast<ast::Col>(sv_item->expr)) {
                    item.kind = SEL_COL;
                    item.col = {.tab_name = sv_col->tab_name, .col_name = sv_col->col_name};
                    item.col = check_column(all_cols, item.col);
                    item.alias = sv_item->alias.empty() ? item.col.col_name : sv_item->alias;
                    query->cols.push_back(item.col);
                } else if (auto sv_agg = std::dynamic_pointer_cast<ast::AggExpr>(sv_item->expr)) {
                    item.kind = SEL_AGG;
                    item.agg.type = static_cast<AggType>(sv_agg->type);
                    item.agg.is_star = (sv_agg->col == nullptr);
                    if (!item.agg.is_star) {
                        item.agg.col = {.tab_name = sv_agg->col->tab_name, .col_name = sv_agg->col->col_name};
                        item.agg.col = check_column(all_cols, item.agg.col);
                        auto col_meta = get_col_meta(all_cols, item.agg.col);
                        if (item.agg.type == AGG_SUM) {
                            if (col_meta.type != TYPE_INT && col_meta.type != TYPE_FLOAT) {
                                throw IncompatibleTypeError(coltype2str(col_meta.type), "SUM");
                            }
                        } else if (item.agg.type == AGG_MAX || item.agg.type == AGG_MIN) {
                            if (col_meta.type != TYPE_INT && col_meta.type != TYPE_FLOAT &&
                                col_meta.type != TYPE_STRING) {
                                throw IncompatibleTypeError(coltype2str(col_meta.type), coltype2str(TYPE_STRING));
                            }
                        }
                    }
                    item.alias = sv_item->alias.empty() ? default_agg_alias(item.agg) : sv_item->alias;
                } else {
                    throw InternalError("Unexpected select item type");
                }
                query->sel_items.push_back(item);
            }
        }
        //处理where条件
        get_clause(x->conds, query->conds);
        check_clause(query->tables, query->conds);
    } else if (auto x = std::dynamic_pointer_cast<ast::UpdateStmt>(parse)) {
        if (!sm_manager_->db_.is_table(x->tab_name)) {
            throw TableNotFoundError(x->tab_name);
        }
        std::vector<ColMeta> all_cols;
        get_all_cols({x->tab_name}, all_cols);
        for (auto &sv_set_clause : x->set_clauses) {
            TabCol lhs = {.tab_name = x->tab_name, .col_name = sv_set_clause->col_name};
            lhs = check_column(all_cols, lhs);
            TabMeta &tab = sm_manager_->db_.get_table(lhs.tab_name);
            auto col = tab.get_col(lhs.col_name);
            Value rhs = convert_sv_value(sv_set_clause->val, &(*col));
            coerce_value_to_col_type(rhs, col->type);
            if (rhs.type != col->type) {
                throw IncompatibleTypeError(coltype2str(col->type), coltype2str(rhs.type));
            }
            rhs.init_raw(col->len);
            query->set_clauses.push_back(SetClause{lhs, rhs});
        }
        get_clause(x->conds, query->conds);
        check_clause({x->tab_name}, query->conds);

    } else if (auto x = std::dynamic_pointer_cast<ast::DeleteStmt>(parse)) {
        if (!sm_manager_->db_.is_table(x->tab_name)) {
            throw TableNotFoundError(x->tab_name);
        }
        //处理where条件
        get_clause(x->conds, query->conds);
        check_clause({x->tab_name}, query->conds);        
    } else if (auto x = std::dynamic_pointer_cast<ast::InsertStmt>(parse)) {
        if (!sm_manager_->db_.is_table(x->tab_name)) {
            throw TableNotFoundError(x->tab_name);
        }
        TabMeta &tab = sm_manager_->db_.get_table(x->tab_name);
        if (x->vals.size() != tab.cols.size()) {
            throw InvalidValueCountError();
        }
        for (size_t i = 0; i < x->vals.size(); i++) {
            query->values.push_back(convert_sv_value(x->vals[i], &tab.cols[i]));
        }
    } else {
        // do nothing
    }
    query->parse = std::move(parse);
    return query;
}


TabCol Analyze::check_column(const std::vector<ColMeta> &all_cols, TabCol target) {
    if (target.tab_name.empty()) {
        // Table name not specified, infer table name from column name
        std::string tab_name;
        for (auto &col : all_cols) {
            if (col.name == target.col_name) {
                if (!tab_name.empty()) {
                    throw AmbiguousColumnError(target.col_name);
                }
                tab_name = col.tab_name;
            }
        }
        if (tab_name.empty()) {
            throw ColumnNotFoundError(target.col_name);
        }
        target.tab_name = tab_name;
    } else {
        bool found = false;
        for (auto &col : all_cols) {
            if (col.tab_name == target.tab_name && col.name == target.col_name) {
                found = true;
                break;
            }
        }
        if (!found) {
            throw ColumnNotFoundError(target.col_name);
        }
    }
    return target;
}

void Analyze::get_all_cols(const std::vector<std::string> &tab_names, std::vector<ColMeta> &all_cols) {
    for (auto &sel_tab_name : tab_names) {
        // 这里db_不能写成get_db(), 注意要传指针
        const auto &sel_tab_cols = sm_manager_->db_.get_table(sel_tab_name).cols;
        all_cols.insert(all_cols.end(), sel_tab_cols.begin(), sel_tab_cols.end());
    }
}

void Analyze::get_clause(const std::vector<std::shared_ptr<ast::BinaryExpr>> &sv_conds, std::vector<Condition> &conds) {
    conds.clear();
    for (auto &expr : sv_conds) {
        Condition cond;
        cond.lhs_col = {.tab_name = expr->lhs->tab_name, .col_name = expr->lhs->col_name};
        cond.op = convert_sv_comp_op(expr->op);
        if (auto rhs_val = std::dynamic_pointer_cast<ast::Value>(expr->rhs)) {
            cond.is_rhs_val = true;
            cond.rhs_sv = rhs_val;
        } else if (auto rhs_col = std::dynamic_pointer_cast<ast::Col>(expr->rhs)) {
            cond.is_rhs_val = false;
            cond.rhs_col = {.tab_name = rhs_col->tab_name, .col_name = rhs_col->col_name};
        }
        conds.push_back(cond);
    }
}

void Analyze::check_clause(const std::vector<std::string> &tab_names, std::vector<Condition> &conds) {
    // auto all_cols = get_all_cols(tab_names);
    std::vector<ColMeta> all_cols;
    get_all_cols(tab_names, all_cols);
    // Get raw values in where clause
    for (auto &cond : conds) {
        // Infer table name from column name
        cond.lhs_col = check_column(all_cols, cond.lhs_col);
        if (!cond.is_rhs_val) {
            cond.rhs_col = check_column(all_cols, cond.rhs_col);
        }
        TabMeta &lhs_tab = sm_manager_->db_.get_table(cond.lhs_col.tab_name);
        auto lhs_col = lhs_tab.get_col(cond.lhs_col.col_name);
        ColType lhs_type = lhs_col->type;
        ColType rhs_type;
        if (cond.is_rhs_val) {
            if (lhs_type == TYPE_BIGINT || lhs_type == TYPE_DATETIME) {
                cond.rhs_val = convert_sv_value(cond.rhs_sv, &(*lhs_col));
            } else {
                cond.rhs_val = convert_sv_value(cond.rhs_sv);
                coerce_value_to_col_type(cond.rhs_val, lhs_type);
                if (lhs_type != cond.rhs_val.type) {
                    throw IncompatibleTypeError(coltype2str(lhs_type), coltype2str(cond.rhs_val.type));
                }
            }
            cond.rhs_val.init_raw(lhs_col->len);
            cond.rhs_sv = nullptr;
            rhs_type = cond.rhs_val.type;
        } else {
            TabMeta &rhs_tab = sm_manager_->db_.get_table(cond.rhs_col.tab_name);
            auto rhs_col = rhs_tab.get_col(cond.rhs_col.col_name);
            rhs_type = rhs_col->type;
            if (lhs_type != rhs_type) {
                throw IncompatibleTypeError(coltype2str(lhs_type), coltype2str(rhs_type));
            }
        }
    }
}


Value Analyze::convert_sv_value(const std::shared_ptr<ast::Value> &sv_val, const ColMeta *col) {
    Value val;
    if (auto int_lit = std::dynamic_pointer_cast<ast::IntLit>(sv_val)) {
        const std::string &int_str = int_lit->val;
        if (col != nullptr && col->type == TYPE_BIGINT) {
            if (!no_overflow<int64_t>(int_str)) {
                throw IntegerOverflowError();
            }
            val.set_bigint(std::stoll(int_str));
        } else {
            if (!no_overflow<int>(int_str)) {
                throw IntegerOverflowError();
            }
            val.set_int(std::stoi(int_str));
        }
    } else if (auto float_lit = std::dynamic_pointer_cast<ast::FloatLit>(sv_val)) {
        val.set_float(float_lit->val);
        if (col != nullptr && col->type != TYPE_FLOAT) {
            throw IncompatibleTypeError(coltype2str(col->type), coltype2str(val.type));
        }
    } else if (auto str_lit = std::dynamic_pointer_cast<ast::StringLit>(sv_val)) {
        if (col != nullptr && col->type == TYPE_DATETIME) {
            val.set_datetime(parse_datetime(str_lit->val));
        } else {
            val.set_str(str_lit->val);
            if (col != nullptr && col->type != TYPE_STRING) {
                throw IncompatibleTypeError(coltype2str(col->type), coltype2str(val.type));
            }
        }
    } else {
        throw InternalError("Unexpected sv value type");
    }
    if (col != nullptr && val.type != col->type) {
        throw IncompatibleTypeError(coltype2str(col->type), coltype2str(val.type));
    }
    return val;
}

void Analyze::coerce_value_to_col_type(Value &val, ColType col_type) {
    if (val.type == col_type) {
        return;
    }
    if (col_type == TYPE_FLOAT && val.type == TYPE_INT) {
        int int_val = val.int_val;
        val.set_float(static_cast<float>(int_val));
    }
}

CompOp Analyze::convert_sv_comp_op(ast::SvCompOp op) {
    std::map<ast::SvCompOp, CompOp> m = {
        {ast::SV_OP_EQ, OP_EQ}, {ast::SV_OP_NE, OP_NE}, {ast::SV_OP_LT, OP_LT},
        {ast::SV_OP_GT, OP_GT}, {ast::SV_OP_LE, OP_LE}, {ast::SV_OP_GE, OP_GE},
    };
    return m.at(op);
}

ColMeta Analyze::get_col_meta(const std::vector<ColMeta> &all_cols, const TabCol &target) {
    for (auto &col : all_cols) {
        if (col.tab_name == target.tab_name && col.name == target.col_name) {
            return col;
        }
    }
    throw ColumnNotFoundError(target.col_name);
}

std::string Analyze::default_agg_alias(const AggSpec &agg) {
    switch (agg.type) {
        case AGG_COUNT:
            return agg.is_star ? "COUNT(*)" : "COUNT(" + agg.col.col_name + ")";
        case AGG_SUM:
            return "SUM(" + agg.col.col_name + ")";
        case AGG_MAX:
            return "MAX(" + agg.col.col_name + ")";
        case AGG_MIN:
            return "MIN(" + agg.col.col_name + ")";
        default:
            return "agg";
    }
}
