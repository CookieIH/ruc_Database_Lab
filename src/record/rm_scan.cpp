// Copyright (c) 2023-2027 Renmin University of China
// SPDX-License-Identifier: MulanPSL-2.0

#include "rm_scan.h"

#include "rm_file_handle.h"

/**
 * @brief 初始化file_handle和rid
 * @param file_handle
 */
RmScan::RmScan(const RmFileHandle* file_handle) : file_handle_(file_handle) {
    // Todo:
    // 初始化file_handle和rid（指向第一个存放了记录的位置）

    // file_handle_ 已在初始化列表里初始化
    // 把 rid_ 放到第一个数据页的前一个槽位，再推进到第一条记录
    rid_ = Rid{RM_FIRST_RECORD_PAGE, -1};
    next();
}

/**
 * @brief 找到文件中下一个存放了记录的位置
 */
void RmScan::next() {
    // Todo:
    // 找到文件中下一个存放了记录的非空闲位置，用rid_来指向这个位置

    while (rid_.page_no < file_handle_->file_hdr_.num_pages) {
        RmPageHandle page_handle = file_handle_->fetch_page_handle(rid_.page_no);
        PageId page_id{.fd = file_handle_->fd_, .page_no = rid_.page_no};

        int slot_no = Bitmap::next_bit(true, page_handle.bitmap,
                                       file_handle_->file_hdr_.num_records_per_page,
                                       rid_.slot_no);

        if (slot_no < file_handle_->file_hdr_.num_records_per_page) {
            rid_.slot_no = slot_no;
            file_handle_->buffer_pool_manager_->unpin_page(page_id, false);
            return;
        }

        // 本页没有更多记录，翻到下一页前先 unpin
        file_handle_->buffer_pool_manager_->unpin_page(page_id, false);
        rid_.page_no++;
        rid_.slot_no = -1;
    }

    // 所有数据页扫描完毕
    rid_ = Rid{INVALID_PAGE_ID, -1};
}

/**
 * @brief ​ 判断是否到达文件末尾
 */
bool RmScan::is_end() const {
    // Todo: 修改返回值

    return rid_.page_no == INVALID_PAGE_ID;
}

/**
 * @brief RmScan内部存放的rid
 */
Rid RmScan::rid() const { return rid_; }