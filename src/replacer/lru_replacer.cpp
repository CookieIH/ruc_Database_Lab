// Copyright (c) 2023-2027 Renmin University of China
// SPDX-License-Identifier: MulanPSL-2.0

#include "lru_replacer.h"

LRUReplacer::LRUReplacer(size_t num_pages) { max_size_ = num_pages; }

LRUReplacer::~LRUReplacer() = default;

/**
 * @description: 使用LRU策略删除一个victim frame，并返回该frame的id
 * @param {frame_id_t*} frame_id 被移除的frame的id，如果没有frame被移除返回nullptr
 * @return {bool} 如果成功淘汰了一个页面则返回true，否则返回false
 */
bool LRUReplacer::victim(frame_id_t* frame_id) {
    // std::scoped_lock 同时保护容量和链表状态。
    // 它能够避免死锁发生，其构造函数能够自动进行上锁操作，析构函数会对互斥量进行解锁操作，保证线程安全。
    std::scoped_lock lock{latch_};  //  如果编译报错可以替换成其他lock

    // 链表为空：没有可淘汰的帧
    if (LRUlist_.empty()) {
        return false;
    }
    // 尾部 = 最久未被使用的帧
    *frame_id = LRUlist_.back();
    LRUlist_.pop_back();
    LRUhash_.erase(*frame_id);
    return true;
}

/**
 * @description: 固定指定的frame，即该页面无法被淘汰
 * @param {frame_id_t} 需要固定的frame的id
 */
void LRUReplacer::pin(frame_id_t frame_id) {
    std::scoped_lock lock{latch_};
    // 只有当前处于「可淘汰」状态（在链表里）的帧才需要处理
    auto it = LRUhash_.find(frame_id);
    if (it == LRUhash_.end()) {
        return;
    }
    // 从链表和哈希表中同时移除，表示它已被固定、不可淘汰
    LRUlist_.erase(it->second);
    LRUhash_.erase(it);
}

/**
 * @description: 取消固定一个frame，代表该页面可以被淘汰
 * @param {frame_id_t} frame_id 取消固定的frame的id
 */
void LRUReplacer::unpin(frame_id_t frame_id) {
    std::scoped_lock lock{latch_};
    // 已经在可淘汰集合里：不重复插入，也不能挪到链表头部
    if (LRUhash_.find(frame_id) != LRUhash_.end()) {
        return;
    }
    // 达到容量上限：放弃记录
    if (LRUlist_.size() >= max_size_) {
        return;
    }
    // 新解除固定的帧放到头部（最近使用端）
    LRUlist_.push_front(frame_id);
    LRUhash_[frame_id] = LRUlist_.begin();
}

/**
 * @description: 获取当前replacer中可以被淘汰的页面数量
 */
size_t LRUReplacer::Size() { return LRUlist_.size(); }
