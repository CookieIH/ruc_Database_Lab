#include "buffer_pool_manager.h"
#include "storage/disk_manager.h" 
#include "storage/page.h"
#include <cstring>

/**
 * @description: 从free_list或replacer中得到可淘汰帧页的 *frame_id
 */
bool BufferPoolManager::find_victim_page(frame_id_t* frame_id) {
    // 1.1 未满，直接拿空闲帧
    if (!free_list_.empty()) {
        *frame_id = free_list_.front();
        free_list_.pop_front();
        return true;
    }
    
    // 1.2 已满，用 LRU 找淘汰者
    if (replacer_->victim(frame_id)) {
        return true;
    }
    return false;
}

/**
 * @description: 更新页面数据, 如果为脏页则需写入磁盘，再更新为新页面
 */
void BufferPoolManager::update_page(Page* page, PageId new_page_id, frame_id_t new_frame_id) {
    // 1. 如果是脏页，写回磁盘
    if (page->is_dirty_ && page->id_.page_no != INVALID_PAGE_ID) {
        disk_manager_->write_page(page->id_.fd, page->id_.page_no, page->data_, PAGE_SIZE); 
    }
    
    // 2. 重置 page 的元数据
    page->id_ = new_page_id;
    page->pin_count_ = 0;
    page->is_dirty_ = false;
    
    // 使用 Page 类自带的函数清空内存
    page->reset_memory();
}

/**
 * @description: 从buffer pool获取需要的页
 */
Page* BufferPoolManager::fetch_page(PageId page_id) {
    std::scoped_lock lock(latch_); 

    // 1. 从 page_table_ 搜寻
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        // 1.1 找到了，直接固定并返回
        frame_id_t frame_id = it->second;
        Page* page = &pages_[frame_id];
        page->pin_count_++;
        replacer_->pin(frame_id); 
        return page;
    }

    // 1.2 没找到，找个空闲 frame
    frame_id_t frame_id;
    if (!find_victim_page(&frame_id)) {
        return nullptr;
    }

    Page* page = &pages_[frame_id];
    
    // 2. 替换旧页（如果旧页在页表里，先移除）
    if (page->id_.page_no != INVALID_PAGE_ID) {
        page_table_.erase(page->id_);
    }

    // 3. 更新页的内容
    update_page(page, page_id, frame_id);
    
    // 4. 从磁盘读入新页
    disk_manager_->read_page(page_id.fd, page_id.page_no, page->data_, PAGE_SIZE); 

    // 5. 更新页表！
    page_table_[page_id] = frame_id;

    // 6. 固定新页
    page->pin_count_ = 1;
    replacer_->pin(frame_id); 
    return page;
}

/**
 * @description: 取消固定 pin_count>0 的 page
 */
bool BufferPoolManager::unpin_page(PageId page_id, bool is_dirty) {
    std::scoped_lock lock(latch_); 

    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) return false;

    Page* page = &pages_[it->second];
    if (page->pin_count_ <= 0) return false;

    page->pin_count_--;
    if (page->pin_count_ == 0) {
        replacer_->unpin(it->second); 
    }
    if (is_dirty) {
        page->is_dirty_ = true;
    }

    return true;
}

/**
 * @description: 将目标页写回磁盘
 */
bool BufferPoolManager::flush_page(PageId page_id) {
    std::scoped_lock lock(latch_); 

    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) return false;

    Page* page = &pages_[it->second];
    
    // 强制写回磁盘
    disk_manager_->write_page(page->id_.fd, page->id_.page_no, page->data_, PAGE_SIZE); 
    page->is_dirty_ = false;
    
    return true;
}

/**
 * @description: 创建一个新的page
 */
Page* BufferPoolManager::new_page(PageId* page_id) {
    std::scoped_lock lock(latch_); 

    frame_id_t frame_id;
    if (!find_victim_page(&frame_id)) return nullptr;

    // 【核心修复】保留测试传入的 fd，只分配新的 page_no
    PageId new_id;
    new_id.fd = page_id->fd; 
    new_id.page_no = disk_manager_->allocate_page(page_id->fd); 
    *page_id = new_id;

    Page* page = &pages_[frame_id];
    
    // 移除旧的页表记录
    if (page->id_.page_no != INVALID_PAGE_ID) {
        page_table_.erase(page->id_);
    }

    // 更新页的内容
    update_page(page, *page_id, frame_id);

    // 更新页表！
    page_table_[*page_id] = frame_id;

    page->pin_count_ = 1;
    replacer_->pin(frame_id); 
    
    return page;
}

/**
 * @description: 从buffer_pool删除目标页
 */
bool BufferPoolManager::delete_page(PageId page_id) {
    std::scoped_lock lock(latch_); 

    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) return true;

    frame_id_t frame_id = it->second;
    Page* page = &pages_[frame_id];

    // 正在被使用，不能删
    if (page->pin_count_ > 0) return false;

    // 写回并清理
    if (page->is_dirty_) {
        disk_manager_->write_page(page->id_.fd, page->id_.page_no, page->data_, PAGE_SIZE); 
    }

    page_table_.erase(page_id);
    
    // 重置元数据（fd保留，page_no设为无效）
    page->id_.page_no = INVALID_PAGE_ID;
    page->is_dirty_ = false;
    page->pin_count_ = 0;
    
    // 加入 free_list_ 并解除 LRU 关联
    free_list_.push_back(frame_id);
    replacer_->unpin(frame_id);
    
    return true;
}

/**
 * @description: 将buffer_pool中的所有页写回到磁盘
 */
void BufferPoolManager::flush_all_pages(int fd) {
    std::scoped_lock lock(latch_); 

    for (size_t i = 0; i < pool_size_; ++i) {
        if (pages_[i].id_.page_no != INVALID_PAGE_ID) {
            disk_manager_->write_page(pages_[i].id_.fd, pages_[i].id_.page_no, pages_[i].data_, PAGE_SIZE);
            pages_[i].is_dirty_ = false;
        }
    }
}