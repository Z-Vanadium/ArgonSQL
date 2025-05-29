#include "buffer/buffer_pool_manager.h"

#include "glog/logging.h"
#include "page/bitmap_page.h"

static const char EMPTY_PAGE_DATA[PAGE_SIZE] = {0};

BufferPoolManager::BufferPoolManager(size_t pool_size, DiskManager *disk_manager)
    : pool_size_(pool_size), disk_manager_(disk_manager) {
  pages_ = new Page[pool_size_];
  replacer_ = new LRUReplacer(pool_size_);
  for (size_t i = 0; i < pool_size_; i++) {
    free_list_.emplace_back(i);
  }
}

BufferPoolManager::~BufferPoolManager() {
  for (auto page : page_table_) {
    FlushPage(page.first);
  }
  delete[] pages_;
  delete replacer_;
}

/**
 * TODO: Student Implement
 */
Page *BufferPoolManager::FetchPage(page_id_t page_id) {
  std::scoped_lock<std::recursive_mutex> lock(latch_); // 获取互斥锁以保证线程安全。

  // 1.     在页表中查找请求的页 (P)。
  auto it = page_table_.find(page_id);
  // 1.1    如果 P 存在，固定它并立即返回。
  if (it != page_table_.end()) {
    frame_id_t frame_id = it->second; // 获取帧ID。
    Page *page = &pages_[frame_id];    // 获取 Page 对象指针。
    page->pin_count_++;                // 增加引用计数。
    replacer_->Pin(frame_id);          // 在替换器中固定该帧。
    return page;
  }

  // 1.2    如果 P 不存在，从空闲列表或替换器中找到一个替换页 (R)。
  //        注意，总是优先从空闲列表中查找。
  frame_id_t victim_frame_id = INVALID_FRAME_ID;
  if (!free_list_.empty()) {
    victim_frame_id = free_list_.front(); // 从空闲列表获取。
    free_list_.pop_front();
  } else {
    // 如果空闲列表为空，尝试从替换器中获取一个牺牲页。
    if (!replacer_->Victim(&victim_frame_id)) {
      // 如果没有找到牺牲页（所有页都被固定），返回 nullptr。
      return nullptr;
    }
  }

  // 获取牺牲 Page 对象的指针。
  Page *victim_page = &pages_[victim_frame_id];

  // 2.     如果 R 是脏页，将其写回磁盘。
  if (victim_page->is_dirty_) {
    disk_manager_->WritePage(victim_page->page_id_, victim_page->GetData());
  }

  // 3.     从页表中删除 R 并插入 P。
  // 如果牺牲页有一个有效的 page_id（即它管理着一个磁盘页），则将其从页表中删除。
  if (victim_page->page_id_ != INVALID_PAGE_ID) {
    page_table_.erase(victim_page->page_id_);
  }
  page_table_[page_id] = victim_frame_id; // 将新的 page_id 插入到页表中。

  // 4.     更新 P 的元数据，从磁盘读取页内容，然后返回 P 的指针。
  victim_page->page_id_ = page_id;       // 设置新的 page_id。
  victim_page->pin_count_ = 1;           // 固定新页，引用计数设为1。
  victim_page->is_dirty_ = false;       // 新页初始不是脏页。
  victim_page->ResetMemory();            // 读取前清空内存。

  disk_manager_->ReadPage(page_id, victim_page->GetData()); // 从磁盘读取内容。
  replacer_->Pin(victim_frame_id);                           // 在替换器中固定新页。

  return victim_page;
}


/**
 * TODO: Student Implement
 */
Page *BufferPoolManager::NewPage(page_id_t &page_id) {
  std::scoped_lock<std::recursive_mutex> lock(latch_); // 获取互斥锁以保证线程安全。

  // 1.   如果缓冲池中所有页面都被固定，返回 nullptr。
  //      这通过牺牲页选择逻辑隐式处理。如果找不到牺牲页，我们将返回 nullptr。

  // 2.   从空闲列表或替换器中选择一个牺牲页 P。总是优先从空闲列表中选择。
  frame_id_t victim_frame_id = INVALID_FRAME_ID;
  if (!free_list_.empty()) {
    victim_frame_id = free_list_.front(); // 从空闲列表获取。
    free_list_.pop_front();
  } else {
    // 如果空闲列表为空，尝试从替换器中获取一个牺牲页。
    if (!replacer_->Victim(&victim_frame_id)) {
      // 如果没有找到牺牲页（所有页都被固定），返回 nullptr。
      return nullptr;
    }
  }

  // 获取牺牲 Page 对象的指针。
  Page *victim_page = &pages_[victim_frame_id];

  // 如果牺牲页是脏页，将其写回磁盘。
  // 这对于正确性至关重要，如果我们重用一个脏页。
  if (victim_page->is_dirty_) {
    disk_manager_->WritePage(victim_page->page_id_, victim_page->GetData());
  }

  // 从页表中删除 R（如果它管理着一个有效页）。
  if (victim_page->page_id_ != INVALID_PAGE_ID) {
    page_table_.erase(victim_page->page_id_);
  }

  // 0.   确保调用 AllocatePage!
  page_id = disk_manager_->AllocatePage(); // 从磁盘管理器分配一个新的页ID。

  // 3.   更新 P 的元数据，清零内存并将 P 添加到页表。
  victim_page->page_id_ = page_id;     // 分配新的页ID。
  victim_page->pin_count_ = 1;         // 固定新页。
  victim_page->is_dirty_ = false;     // 新页初始不是脏页。
  victim_page->ResetMemory();          // 清空页面内存。

  page_table_[page_id] = victim_frame_id; // 将新页添加到页表。
  replacer_->Pin(victim_frame_id);         // 在替换器中固定该帧。

  // 4.   设置页ID输出参数。返回 P 的指针。
  return victim_page;
}

/**
 * TODO: Student Implement
 */
bool BufferPoolManager::DeletePage(page_id_t page_id) {
  std::scoped_lock<std::recursive_mutex> lock(latch_); // 获取互斥锁以保证线程安全。

  // 1.   在页表中查找请求的页 (P)。
  auto it = page_table_.find(page_id);

  // 1.   如果 P 不存在，返回 true。（它已经被删除或从未在缓冲池中）
  if (it == page_table_.end()) {
    disk_manager_->DeAllocatePage(page_id); // 确保即使不在缓冲池中，磁盘页也被释放。
    return true;
  }

  frame_id_t frame_id = it->second;
  Page *page_to_delete = &pages_[frame_id];

  // 2.   如果 P 存在，但引用计数不为零，返回 false。有人正在使用该页。
  if (page_to_delete->pin_count_ > 0) {
    return false;
  }

  // 3.   否则，P 可以被删除。
  // 如果是脏页，在删除前将其刷新到磁盘。
  if (page_to_delete->is_dirty_) {
    disk_manager_->WritePage(page_to_delete->page_id_, page_to_delete->GetData());
  }

  // 从页表中移除 P。
  page_table_.erase(it);

  // 重置其元数据。
  page_to_delete->page_id_ = INVALID_PAGE_ID;
  page_to_delete->pin_count_ = 0;
  page_to_delete->is_dirty_ = false;
  page_to_delete->ResetMemory(); // 清空内存。

  // 0.   确保调用 DeallocatePage!
  disk_manager_->DeAllocatePage(page_id); // 释放磁盘上的页。

  // 将其返回到空闲列表。
  free_list_.push_back(frame_id);

  // 从替换器中解除固定（如果它曾经在替换器中）。
  // 尽管 pin_count_ 为 0，但确保它从替换器跟踪中移除是一种良好的实践。
  replacer_->Pin(frame_id); // Pin 会从替换器中移除它（如果存在）。

  return true;
}


/**
 * TODO: Student Implement
 */
bool BufferPoolManager::UnpinPage(page_id_t page_id, bool is_dirty) {
  std::scoped_lock<std::recursive_mutex> lock(latch_); // 获取互斥锁以保证线程安全。

  // 在页表中查找请求的页。
  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    // 页面不在缓冲池中，无法解除固定。
    return false;
  }

  frame_id_t frame_id = it->second;
  Page *page = &pages_[frame_id];

  // 更新脏页状态。
  if (is_dirty) {
    page->is_dirty_ = true;
  }

  // 减少引用计数。
  if (page->pin_count_ > 0) {
    page->pin_count_--;
  } else {
    // 引用计数已为0，不应发生，除非存在逻辑错误。
    return false;
  }

  // 如果引用计数变为0，则从替换器中解除固定。
  if (page->pin_count_ == 0) {
    replacer_->Unpin(frame_id);
  }

  return true;
}


/**
 * TODO: Student Implement
 */
bool BufferPoolManager::FlushPage(page_id_t page_id) {
  std::scoped_lock<std::recursive_mutex> lock(latch_); // 获取互斥锁以保证线程安全。

  // 在页表中查找请求的页。
  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    // 页面不在缓冲池中，无法刷新。
    return false;
  }

  frame_id_t frame_id = it->second;
  Page *page_to_flush = &pages_[frame_id];

  // 将页面内容写回磁盘。
  disk_manager_->WritePage(page_to_flush->page_id_, page_to_flush->GetData());
  // 刷新后重置脏页标志。
  page_to_flush->is_dirty_ = false;

  return true;
}

// FlushAllPages函数：将所有的页面都转储到磁盘中。
// 这是在要求列表中提及但未在提供的代码中有TODO标记的函数。

void BufferPoolManager::FlushAllPages() {
  std::scoped_lock<std::recursive_mutex> lock(latch_); // 获取互斥锁以保证线程安全。
  for (auto const& [page_id, frame_id] : page_table_) { // 遍历页表中的所有页面。
    FlushPage(page_id); // 调用 FlushPage 来刷新每个页面。
  }
}
page_id_t BufferPoolManager::AllocatePage() {
  int next_page_id = disk_manager_->AllocatePage();
  return next_page_id;
}

void BufferPoolManager::DeallocatePage(__attribute__((unused)) page_id_t page_id) {
  disk_manager_->DeAllocatePage(page_id);
}

bool BufferPoolManager::IsPageFree(page_id_t page_id) {
  return disk_manager_->IsPageFree(page_id);
}

// Only used for debug
bool BufferPoolManager::CheckAllUnpinned() {
  bool res = true;
  for (size_t i = 0; i < pool_size_; i++) {
    if (pages_[i].pin_count_ != 0) {
      res = false;
      LOG(ERROR) << "page " << pages_[i].page_id_ << " pin count:" << pages_[i].pin_count_ << endl;
    }
  }
  return res;
}