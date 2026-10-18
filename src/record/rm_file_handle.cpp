// Copyright (c) 2023-2027 Renmin University of China
// SPDX-License-Identifier: MulanPSL-2.0

#include "rm_file_handle.h"

/**
 * @description: 获取当前表中记录号为rid的记录
 * @param {Rid&} rid 记录号，指定记录的位置
 * @param {Context*} context
 * @return {unique_ptr<RmRecord>} rid对应的记录对象指针
 */
std::unique_ptr<RmRecord> RmFileHandle::get_record(const Rid& rid, Context* context) const {
    // Todo:
    // 1. 获取指定记录所在的page handle
    // 2. 初始化一个指向RmRecord的指针（赋值其内部的data和size）

    // 1. 获取指定记录所在的 page handle
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    PageId page_id{.fd = fd_, .page_no = rid.page_no};

    // 2. 校验该 slot 上确实有记录
    if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        buffer_pool_manager_->unpin_page(page_id, false);
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }

    // 3. 把记录拷贝一份返回；RmRecord(size, data) 会复制 data 的内容
    auto record = std::make_unique<RmRecord>(file_hdr_.record_size,
                                             page_handle.get_slot(rid.slot_no));

    buffer_pool_manager_->unpin_page(page_id, false);
    return record;
}

/**
 * @description: 在当前表中插入一条记录，不指定插入位置
 * @param {const char*} buf 要插入的记录数据，只读取 record_size 字节
 * @param {Context*} context
 * @return {Rid} 插入的记录的记录号（位置）
 */
Rid RmFileHandle::insert_record(const char* buf, Context* context) {
    // Todo:
    // 1. 获取当前未满的page handle
    // 2. 在page handle中找到空闲slot位置
    // 3. 将buf复制到空闲slot位置
    // 4. 更新page_handle.page_hdr中的数据结构
    // 注意考虑插入一条记录后页面已满的情况，需要更新file_hdr_.first_free_page_no

    // 1. 获取当前未满的 page handle（可能新建页，也可能取空闲页）
    RmPageHandle page_handle = create_page_handle();
    int page_no = page_handle.page->get_page_id().page_no;

    // 2. 在 bitmap 中找第一个空槽
    int slot_no = Bitmap::first_bit(false, page_handle.bitmap, file_hdr_.num_records_per_page);
    if (slot_no >= file_hdr_.num_records_per_page) {
        // 正常不会发生：create_page_handle 返回的页一定未满
        buffer_pool_manager_->unpin_page(PageId{.fd = fd_, .page_no = page_no}, false);
        throw InternalError("RmFileHandle::insert_record: no free slot");
    }

    // 3. 复制记录并更新 bitmap / 页头
    memcpy(page_handle.get_slot(slot_no), buf, file_hdr_.record_size);
    Bitmap::set(page_handle.bitmap, slot_no);
    page_handle.page_hdr->num_records++;

    // 4. 插入后页满，从空闲链表摘除并写回文件头
    if (page_handle.page_hdr->num_records == file_hdr_.num_records_per_page) {
        file_hdr_.first_free_page_no = page_handle.page_hdr->next_free_page_no;
        disk_manager_->write_page(fd_, RM_FILE_HDR_PAGE,
                                  reinterpret_cast<char *>(&file_hdr_),
                                  sizeof(file_hdr_));
    }

    // 页面被修改，unpin 时标记 dirty
    buffer_pool_manager_->unpin_page(PageId{.fd = fd_, .page_no = page_no}, true);

    return Rid{.page_no = page_no, .slot_no = slot_no};
}

/**
 * @description: 在当前表中的指定位置插入一条记录
 * @param {Rid&} rid 要插入记录的位置
 * @param {const char*} buf 要插入的记录数据，只读取 record_size 字节
 */
void RmFileHandle::insert_record(const Rid& rid, const char* buf) {
    // 后续恢复/回滚实验使用的扩展接口，不属于 Lab1 评分任务。
}

/**
 * @description: 删除记录文件中记录号为rid的记录
 * @param {Rid&} rid 要删除的记录的记录号（位置）
 * @param {Context*} context
 */
void RmFileHandle::delete_record(const Rid& rid, Context* context) {
    // Todo:
    // 1. 获取指定记录所在的page handle
    // 2. 更新page_handle.page_hdr中的数据结构
    // 注意考虑删除一条记录后页面未满的情况，需要调用release_page_handle()

     // 1. 获取指定记录所在的 page handle
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    PageId page_id{.fd = fd_, .page_no = rid.page_no};

    // 2. 校验该 slot 上确实有记录
    if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        buffer_pool_manager_->unpin_page(page_id, false);
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }

    // 3. 记录删除前是否已满，用于判断删除后是否需要挂回空闲链表
    bool was_full = (page_handle.page_hdr->num_records == file_hdr_.num_records_per_page);

    // 4. 清 bitmap 对应位，减少记录数
    Bitmap::reset(page_handle.bitmap, rid.slot_no);
    page_handle.page_hdr->num_records--;

    // 5. 从满变未满，重新挂回空闲链表
    if (was_full) {
        release_page_handle(page_handle);
    }

    // 页面被修改，unpin 时标记 dirty
    buffer_pool_manager_->unpin_page(page_id, true);
}

/**
 * @description: 更新记录文件中记录号为rid的记录
 * @param {Rid&} rid 要更新的记录的记录号（位置）
 * @param {const char*} buf 新记录的数据，只读取 record_size 字节
 * @param {Context*} context
 */
void RmFileHandle::update_record(const Rid& rid, const char* buf, Context* context) {
    // Todo:
    // 1. 获取指定记录所在的page handle
    // 2. 更新记录

    // 1. 获取指定记录所在的 page handle
    RmPageHandle page_handle = fetch_page_handle(rid.page_no);
    PageId page_id{.fd = fd_, .page_no = rid.page_no};

    // 2. 校验 slot 上确实有记录
    if (!Bitmap::is_set(page_handle.bitmap, rid.slot_no)) {
        buffer_pool_manager_->unpin_page(page_id, false);
        throw RecordNotFoundError(rid.page_no, rid.slot_no);
    }

    // 3. 直接覆盖记录内容
    memcpy(page_handle.get_slot(rid.slot_no), buf, file_hdr_.record_size);

    // 页面被修改，unpin 时标记 dirty
    buffer_pool_manager_->unpin_page(page_id, true);
}

/**
 * 以下函数为辅助函数，仅提供参考，可以选择完成如下函数，也可以删除如下函数，在单元测试中不涉及如下函数接口的直接调用
 */
/**
 * @description: 获取指定页面的页面句柄
 * @param {int} page_no 页面号
 * @return {RmPageHandle} 指定页面的句柄
 */
RmPageHandle RmFileHandle::fetch_page_handle(int page_no) const {
    // Todo:
    // 使用缓冲池获取指定页面，并生成page_handle返回给上层
    // if page_no is invalid, throw PageNotExistError exception

     // 非法页号：负数、文件头页、超出已分配页数
    if (page_no < RM_FIRST_RECORD_PAGE || page_no >= file_hdr_.num_pages) {
        throw PageNotExistError(disk_manager_->get_file_name(fd_), page_no);
    }

    Page *page = buffer_pool_manager_->fetch_page(PageId{.fd = fd_, .page_no = page_no});
    if (page == nullptr) {
        throw InternalError("RmFileHandle::fetch_page_handle: no free frame");
    }

    return RmPageHandle(&file_hdr_, page);
}

/**
 * @description: 创建一个新的page handle
 * @return {RmPageHandle} 新的PageHandle
 */
RmPageHandle RmFileHandle::create_new_page_handle() {
    // Todo:
    // 1.使用缓冲池来创建一个新page
    // 2.更新page handle中的相关信息
    // 3.更新file_hdr_

    // 1. 用缓冲池创建新页；fd 必须先设好，allocate_page 才知道分给哪个文件
    PageId page_id{.fd = fd_, .page_no = INVALID_PAGE_ID};
    Page *page = buffer_pool_manager_->new_page(&page_id);
    if (page == nullptr) {
        throw InternalError("RmFileHandle::create_new_page_handle: no free frame");
    }

    // 2. 用新 page 构造 page handle，初始化页头
    RmPageHandle page_handle(&file_hdr_, page);
    page_handle.page_hdr->num_records = 0;
    page_handle.page_hdr->next_free_page_no = RM_NO_PAGE;
    Bitmap::init(page_handle.bitmap, file_hdr_.bitmap_size);

    // 3. 更新 file_hdr_ 并挂到空闲链表；release_page_handle 内部会写回文件头页
    file_hdr_.num_pages++;
    release_page_handle(page_handle);

    return page_handle;
}

/**
 * @brief 创建或获取一个空闲的page handle
 *
 * @return RmPageHandle 返回生成的空闲page handle
 * @note pin the page, remember to unpin it outside!
 */
RmPageHandle RmFileHandle::create_page_handle() {
    // Todo:
    // 1. 判断file_hdr_中是否还有空闲页
    //     1.1 没有空闲页：使用缓冲池来创建一个新page；可直接调用create_new_page_handle()
    //     1.2 有空闲页：直接获取第一个空闲页
    // 2. 生成page handle并返回给上层

    // 1. 文件头里还有空闲页，直接取第一个空闲页
    if (file_hdr_.first_free_page_no != RM_NO_PAGE) {
        return fetch_page_handle(file_hdr_.first_free_page_no);
    }

    // 2. 没有空闲页，新建一个
    return create_new_page_handle();
}

/**
 * @description: 当一个页面从没有空闲空间的状态变为有空闲空间状态时，更新文件头和页头中空闲页面相关的元数据
 */
void RmFileHandle::release_page_handle(RmPageHandle& page_handle) {
    // Todo:
    // 当page从已满变成未满，考虑如何更新：
    // 1. page_handle.page_hdr->next_free_page_no
    // 2. file_hdr_.first_free_page_no

    // 1. 当前页的后继指向原来的链表头
    page_handle.page_hdr->next_free_page_no = file_hdr_.first_free_page_no;

    // 2. 文件头的链表头指向当前页
    file_hdr_.first_free_page_no = page_handle.page->get_page_id().page_no;

    // 3. 文件头改了，写回磁盘
    disk_manager_->write_page(fd_, RM_FILE_HDR_PAGE,
                              reinterpret_cast<char *>(&file_hdr_),
                              sizeof(file_hdr_));
}
