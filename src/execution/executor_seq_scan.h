// Copyright (c) 2023-2027 Renmin University of China
// SPDX-License-Identifier: MulanPSL-2.0

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/common.h"
#include "common/config.h"
#include "common/defs.h"
#include "common/errors.h"
#include "executor_abstract.h"
#include "record/rm_defs.h"
#include "record/rm_file_handle.h"
#include "record/rm_scan.h" // 引入 RmScan
#include "system/sm_manager.h"
#include "system/sm_meta.h"

class SeqScanExecutor : public AbstractExecutor {
private:
    std::string tab_name_;              // 表的名称
    std::vector<Condition> conds_;      // scan的条件
    RmFileHandle* fh_;                  // 表的数据文件句柄
    std::vector<ColMeta> cols_;         // scan后生成的记录的字段
    size_t len_;                        // scan后生成的每条记录的长度
    std::vector<Condition> fed_conds_;  // 同conds_，两个字段相同

    Rid rid_{.page_no = INVALID_PAGE_ID, .slot_no = -1};
    std::unique_ptr<RecScan> scan_;  // table_iterator

    SmManager* sm_manager_;

public:
    SeqScanExecutor(SmManager* sm_manager, std::string tab_name, std::vector<Condition> conds, Context* context) {
        sm_manager_ = sm_manager;
        tab_name_ = std::move(tab_name);
        conds_ = std::move(conds);
        TabMeta& tab = sm_manager_->db_.get_table(tab_name_);
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab.cols;
        len_ = cols_.back().offset + cols_.back().len;

        context_ = context;

        fed_conds_ = conds_;
    }

    /**
     * @brief 构建表迭代器scan_,并开始迭代扫描,直到扫描到第一个满足谓词条件的元组停止,并赋值给rid_
     *
     */
    void begin_tuple() override {
        // 1. 从表的表头开始创建扫描器
        scan_ = std::make_unique<RmScan>(fh_);
        // 2. 找一个满足条件的记录
        next_tuple(); 
    }

    /**
     * @brief 从当前scan_指向的记录开始迭代扫描,直到扫描到第一个满足谓词条件的元组停止,并赋值给rid_
     *
     */
    void next_tuple() override {
        if (scan_ == nullptr) return;

        for (; !scan_->is_end(); scan_->next()) {
            Rid current_rid = scan_->rid();
            auto record = fh_->get_record(current_rid, context_);
            
            bool match = true;
            for (const auto& cond : fed_conds_) {
                auto col_it = std::find_if(cols_.begin(), cols_.end(), [&](const ColMeta& c) {
                    return c.name == cond.lhs_col.col_name;
                });
                if (col_it == cols_.end()) continue;

                char* rec_val = record->data + col_it->offset;
                int cmp = 0;
                
                if (col_it->type == TYPE_INT) {
                    int val;
                    memcpy(&val, rec_val, sizeof(int));
                    int cond_val;
                    if (cond.is_rhs_val) {
                        memcpy(&cond_val, cond.rhs_val.raw->data, sizeof(int));
                    } else {
                        auto rhs_col_it = std::find_if(cols_.begin(), cols_.end(), [&](const ColMeta& c) {
                            return c.name == cond.rhs_col.col_name;
                        });
                        memcpy(&cond_val, record->data + rhs_col_it->offset, sizeof(int));
                    }
                    cmp = (val < cond_val) ? -1 : ((val > cond_val) ? 1 : 0);
                } else if (col_it->type == TYPE_FLOAT) {
                    float val, cond_val;
                    memcpy(&val, rec_val, sizeof(float));
                    if (cond.is_rhs_val) {
                        memcpy(&cond_val, cond.rhs_val.raw->data, sizeof(float));
                    } else {
                        auto rhs_col_it = std::find_if(cols_.begin(), cols_.end(), [&](const ColMeta& c) {
                            return c.name == cond.rhs_col.col_name;
                        });
                        memcpy(&cond_val, record->data + rhs_col_it->offset, sizeof(float));
                    }
                    cmp = (val < cond_val) ? -1 : ((val > cond_val) ? 1 : 0);
                } else if (col_it->type == TYPE_STRING) {
                    std::string val(rec_val, col_it->len);
                    std::string cond_val;
                    if (cond.is_rhs_val) {
                        cond_val.assign(cond.rhs_val.raw->data, cond.rhs_val.raw->size);
                    } else {
                        auto rhs_col_it = std::find_if(cols_.begin(), cols_.end(), [&](const ColMeta& c) {
                            return c.name == cond.rhs_col.col_name;
                        });
                        cond_val.assign(record->data + rhs_col_it->offset, rhs_col_it->len);
                    }
                    val = val.c_str();
                    cond_val = cond_val.c_str();
                    cmp = val.compare(cond_val);
                }

                bool this_match = false;
                switch (cond.op) {
                    case OP_EQ: this_match = (cmp == 0); break;
                    case OP_NE: this_match = (cmp != 0); break;
                    case OP_LT: this_match = (cmp < 0); break;
                    case OP_GT: this_match = (cmp > 0); break;
                    case OP_LE: this_match = (cmp <= 0); break;
                    case OP_GE: this_match = (cmp >= 0); break;
                }
                if (!this_match) {
                    match = false;
                    break; 
                }
            }

            if (match) {
                rid_ = current_rid;
                scan_->next(); 
                return;
            }
        }
        
        rid_ = {.page_no = INVALID_PAGE_ID, .slot_no = -1};
    }

    /**
     * @brief 返回下一个满足扫描条件的记录
     *
     * @return std::unique_ptr<RmRecord>
     */
    

    /**
     * @brief 返回下一个满足扫描条件的记录
     *
     * @return std::unique_ptr<RmRecord>
     */
    std::unique_ptr<RmRecord> next() override { 
        if (is_end()) return nullptr;
        return fh_->get_record(rid_, context_); 
    }

    size_t tuple_len() const override { return len_;  }

    const std::vector<ColMeta>& cols() const override { return cols_;  }

    ColMeta get_col_offset(const TabCol& target) override {
         auto it = std::find_if(cols_.begin(), cols_.end(), [&](const ColMeta& c) {
            return c.name == target.col_name;
        });
        if (it == cols_.end()) {
            throw ColumnNotFoundError(target.col_name);
        }
        return *it;
    }

    bool is_end() const override {
        return rid_.page_no == INVALID_PAGE_ID;
    }

    Rid& rid() override { return rid_; }
};
