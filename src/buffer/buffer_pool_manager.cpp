#include "buffer/buffer_pool_manager.h"

#include "glog/logging.h"
#include "page/bitmap_page.h"

static const char EMPTY_PAGE_DATA[PAGE_SIZE] = {0};

BufferPoolManager::BufferPoolManager(size_t pool_size, DiskManager *disk_manager)
    : pool_size_(pool_size), disk_manager_(disk_manager) {
  pages_ = new Page[pool_size_];
  replacer_ = new LRUReplacer(pool_size_); // 使用传入的 pool_size 初始化 LRUReplacer
  for (size_t i = 0; i < pool_size_; i++) {
    free_list_.emplace_back(i); // 将所有帧ID添加到空闲列表
  }
}

BufferPoolManager::~BufferPoolManager() {
  // 析构时，将缓冲池中所有脏页刷新到磁盘。
  // 注意：这里遍历的是 page_table_，它只包含当前在缓冲池中的有效页面。
  for (auto page_entry : page_table_) { // 使用 auto page_entry : page_table_ 遍历
    FlushPage(page_entry.first);
  }
  delete[] pages_;    // 释放 pages_ 数组内存
  delete replacer_;   // 释放 replacer 对象内存
}

/**
 * FetchPage函数：从缓冲池中获取指定page_id的数据页。
 * @param page_id 请求获取的页ID。
 * @return 指向Page对象的指针，如果无法获取则返回nullptr。
 */
Page *BufferPoolManager::FetchPage(page_id_t page_id) {
  std::scoped_lock<std::recursive_mutex> lock(latch_); // 获取互斥锁以保证线程安全。

  // 1. 在页表中查找请求的页 (P)。
  auto it = page_table_.find(page_id);
  // 1.1 如果 P 存在，说明该页已经在缓冲池中。
  if (it != page_table_.end()) {
    frame_id_t frame_id = it->second; // 获取对应的帧ID。
    Page *page = &pages_[frame_id];    // 获取 Page 对象指针。
    page->pin_count_++;                 // 增加页面的固定计数。
    replacer_->Pin(frame_id);           // 通知 replacer 该帧被固定，使其不能被替换。
    return page;                        // 返回获取到的页面指针。
  }

  // 1.2 如果 P 不存在（即不在缓冲池中），需要从磁盘读取到缓冲池。
  frame_id_t frame_id = INVALID_FRAME_ID; // 声明一个变量来存储找到的空闲帧ID。
  Page *page_to_fetch = nullptr;         // 声明一个指针来指向要获取的页面对象。

  // 2. 找到一个可用的空闲帧。
  // 2.1 如果空闲列表不为空，直接从空闲列表中获取一个帧。
  if (!free_list_.empty()) {
    frame_id = free_list_.front(); // 获取空闲列表的第一个帧ID。
    free_list_.pop_front();        // 从空闲列表中移除该帧。
    page_to_fetch = &pages_[frame_id]; // 获取对应的Page对象指针。
  } else {
    // 2.2 如果空闲列表为空，需要从 replacer 中牺牲一个页面。
    if (!replacer_->Victim(&frame_id)) {
      // 如果 replacer 无法找到一个可牺牲的页面（所有页面都被固定），则返回nullptr。
      return nullptr;
    }
    page_to_fetch = &pages_[frame_id]; // 获取被牺牲的Page对象指针。

    // 3. 如果被牺牲的页面是脏页，需要先将其写回磁盘。
    // 这里确保页ID有效，防止写入 INVALID_PAGE_ID (-1)。
    if (page_to_fetch->IsDirty() && page_to_fetch->GetPageId() != INVALID_PAGE_ID) {
      disk_manager_->WritePage(page_to_fetch->GetPageId(), page_to_fetch->GetData());
    }
    // 4. 从页表中移除被牺牲的页面（如果它有有效的页ID）。
    if (page_to_fetch->GetPageId() != INVALID_PAGE_ID) {
      page_table_.erase(page_to_fetch->GetPageId());
    }
  }

  // 5. 设置新获取或被替换的页面的元数据。
  page_to_fetch->page_id_ = page_id;     // 设置页ID为请求的页ID。
  page_to_fetch->pin_count_ = 1;         // 固定计数设为1（当前被引用）。
  page_to_fetch->is_dirty_ = false;      // 从磁盘读取的页面初始不是脏的。
  page_to_fetch->ResetMemory();          // 清空内存（以防有旧数据干扰，虽然ReadPage会覆盖）。

  // 6. 从磁盘读取请求的页面数据到缓冲池中。
  disk_manager_->ReadPage(page_id, page_to_fetch->GetData());

  // 7. 更新页表，将新页帧的映射关系加入。
  page_table_[page_id] = frame_id;
  // 8. 通知 replacer 该帧被固定，使其不能被替换。
  replacer_->Pin(frame_id);

  return page_to_fetch; // 返回新获取的页面指针。
}

/**
 * NewPage函数：创建一个新的数据页。
 * @param page_id 用于存储新创建页的ID的引用。
 * @return 指向Page对象的指针，如果无法创建则返回nullptr。
 */
Page *BufferPoolManager::NewPage(page_id_t &page_id) {
  std::scoped_lock<std::recursive_mutex> lock(latch_); // 获取互斥锁以保证线程安全。

  frame_id_t frame_id = INVALID_FRAME_ID; // 声明一个变量来存储找到的空闲帧ID。
  Page *new_page = nullptr;               // 声明一个指针来指向新页面对象。

  // 1. 找到一个可用的空闲帧。
  // 1.1 如果空闲列表不为空，直接从空闲列表中获取一个帧。
  if (!free_list_.empty()) {
    frame_id = free_list_.front();    // 获取空闲列表的第一个帧ID。
    free_list_.pop_front();           // 从空闲列表中移除该帧。
    new_page = &pages_[frame_id];     // 获取对应的Page对象指针。
  } else {
    // 1.2 如果空闲列表为空，需要从 replacer 中牺牲一个页面。
    if (!replacer_->Victim(&frame_id)) {
      // 如果 replacer 无法找到一个可牺牲的页面（所有页面都被固定），则返回nullptr。
      return nullptr;
    }
    new_page = &pages_[frame_id];     // 获取被牺牲的Page对象指针。

    // 2. 如果被牺牲的页面是脏页，需要先将其写回磁盘。
    if (new_page->IsDirty() && new_page->GetPageId() != INVALID_PAGE_ID) {
      disk_manager_->WritePage(new_page->GetPageId(), new_page->GetData());
    }
    // 3. 从页表中移除被牺牲的页面（如果它有有效的页ID）。
    if (new_page->GetPageId() != INVALID_PAGE_ID) {
      page_table_.erase(new_page->GetPageId());
    }
  }

  // 4. 分配一个新的逻辑页ID。
  page_id = disk_manager_->AllocatePage();
  // 关键修正：如果DiskManager无法分配新页面，则NewPage应返回nullptr。
  if (page_id == INVALID_PAGE_ID) {
    // 将获得的帧返回给 free_list_，并重置其状态
    // 如果是从 replacer 获得的，它在 Victim 中已经被 Pin 了
    // 此时它没有有效的 page_id 关联，不应该在 replacer 中
    // 把它放回 free_list_ 最简单且安全
    free_list_.push_front(frame_id);
    new_page->page_id_ = INVALID_PAGE_ID; // 重置页ID
    new_page->pin_count_ = 0;             // 重置固定计数
    new_page->is_dirty_ = false;          // 重置脏页标志
    new_page->ResetMemory();              // 清空内存
    return nullptr;
  }

  // 5. 更新新页面的元数据。
  new_page->page_id_ = page_id;     // 设置页ID为新分配的ID。
  new_page->pin_count_ = 1;         // 固定计数设为1（新页被创建后立即被引用）。
  new_page->is_dirty_ = true;      // 新创建的页面是脏的，因为它尚未写入磁盘。
  new_page->ResetMemory();          // 清空页面内存，初始化为0。

  // 6. 将新页的映射关系添加到页表。
  page_table_[page_id] = frame_id;
  // 7. 通知 replacer 该帧被固定，使其不能被替换。
  replacer_->Pin(frame_id);

  return new_page; // 返回新创建的页面指针。
}

/**
 * UnpinPage函数：解除指定页的固定。
 * @param page_id 要解除固定的页ID。
 * @param is_dirty 该页是否是脏页（如果为true，需要在写回磁盘时标记为脏）。
 * @return 如果页存在且解除固定成功，则返回true；否则返回false。
 */
bool BufferPoolManager::UnpinPage(page_id_t page_id, bool is_dirty) {
  std::scoped_lock<std::recursive_mutex> lock(latch_); // 获取互斥锁以保证线程安全。

  // 1. 在页表中查找请求的页。
  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    // 页面不在缓冲池中，无法解除固定。
    return false;
  }

  frame_id_t frame_id = it->second; // 获取帧ID。
  Page *page_to_unpin = &pages_[frame_id]; // 获取 Page 对象指针。

  // 2. 如果 pin_count_ 已经为0，说明该页没有被固定，无法再次解除固定。
  if (page_to_unpin->GetPinCount() == 0) {
    return false;
  }

  // 3. 减少页面的固定计数。
  page_to_unpin->pin_count_--;

  // 4. 如果 is_dirty 为 true，则标记页面为脏页。
  if (is_dirty) {
    page_to_unpin->is_dirty_ = true;
  }

  // 5. 如果固定计数变为0，通知 replacer 该帧现在可以被替换。
  if (page_to_unpin->GetPinCount() == 0) {
    replacer_->Unpin(frame_id);
  }

  return true;
}

/**
 * FlushPage函数：将指定页的内容强制写回磁盘。
 * @param page_id 要刷新的页ID。
 * @return 如果页存在并刷新成功，则返回true；否则返回false。
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
void BufferPoolManager::FlushAllPages() {
  std::scoped_lock<std::recursive_mutex> lock(latch_); // 获取互斥锁以保证线程安全。
  // 复制 page_table_ 的键，然后通过键调用 FlushPage，
  // 因为 FlushPage 不会改变 page_table_ 的结构，所以不需要复制整个 map。
  std::vector<page_id_t> pages_to_flush;
  for (auto const& [page_id, frame_id] : page_table_) {
      pages_to_flush.push_back(page_id);
  }
  for (page_id_t page_id : pages_to_flush) {
      FlushPage(page_id); // 调用 FlushPage 来刷新每个页面。
  }
}

/**
 * DeletePage函数：删除一个数据页。
 * @param page_id 要删除的页ID。
 * @return 如果页存在并成功删除，则返回true；否则返回false。
 */
bool BufferPoolManager::DeletePage(page_id_t page_id) {
  std::scoped_lock<std::recursive_mutex> lock(latch_); // 获取互斥锁以保证线程安全。

  // 1. 在页表中查找请求的页。
  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    // 页面不在缓冲池中，或者之前已经删除，返回true表示操作完成（幂等性）。
    return true;
  }

  frame_id_t frame_id = it->second; // 获取帧ID。
  Page *page_to_delete = &pages_[frame_id]; // 获取 Page 对象指针。

  // 2. 检查页面的固定计数。如果 pin_count_ 不为0，则无法删除。
  if (page_to_delete->GetPinCount() > 0) {
    return false;
  }

  // 3. 从页表中移除该页。
  page_table_.erase(page_id);
  // 4. 通知 replacer 该帧可以被重新使用（理论上 Victim 之前它应该已经被 Unpin 了）。
  // 明确地 Pin 一次，确保它从 replacer 中被移除，以免 Victim 错误地选中一个已删除的帧。
  replacer_->Pin(frame_id);

  // 5. 将帧返回到空闲列表。
  free_list_.push_back(frame_id);

  // 6. 重置页面的元数据，将页ID设置为INVALID_PAGE_ID，并将脏页标志设为false。
  page_to_delete->page_id_ = INVALID_PAGE_ID;
  page_to_delete->pin_count_ = 0;
  page_to_delete->is_dirty_ = false;
  page_to_delete->ResetMemory(); // 清空页面内存。

  // 7. 通知 DiskManager 释放该逻辑页ID。
  disk_manager_->DeAllocatePage(page_id);

  return true;
}

/**
 * IsPageFree函数（辅助函数）：检查一个逻辑页ID是否是空闲的。
 * @param page_id 要检查的逻辑页ID。
 * @return 如果页是空闲的则返回true，否则返回false。
 */
bool BufferPoolManager::IsPageFree(page_id_t page_id) {
  return disk_manager_->IsPageFree(page_id);
}

/**
 * CheckAllUnpinned函数（辅助函数）：检查缓冲池中是否所有页面都被解除固定。
 * 主要用于测试和调试。
 * @return 如果所有页面都解除固定，则返回true；否则返回false。
 */
bool BufferPoolManager::CheckAllUnpinned() {
  bool all_unpinned = true;
  for (size_t i = 0; i < pool_size_; i++) {
    if (pages_[i].pin_count_ != 0) {
      all_unpinned = false;
      LOG(ERROR) << "page " << pages_[i].page_id_ << " pin count:" << pages_[i].pin_count_ << endl;
    }
  }
  return all_unpinned;
}

// 私有辅助函数：分配一个新的逻辑页ID。
page_id_t BufferPoolManager::AllocatePage() {
  // 委托给 DiskManager 分配一个新的逻辑页ID。
  int next_page_id = disk_manager_->AllocatePage();
  return next_page_id;
}

// 私有辅助函数：释放一个逻辑页ID。
void BufferPoolManager::DeallocatePage(page_id_t page_id) {
  // 委托给 DiskManager 释放指定的逻辑页ID。
  disk_manager_->DeAllocatePage(page_id);
}

// 私有辅助函数：尝试找到一个空闲帧（不属于公共接口，只是内部辅助）。
frame_id_t BufferPoolManager::TryToFindFreePage() {
  // 这个函数在 FetchPage 和 NewPage 中已经内联处理，所以这里不实现。
  // 它通常会尝试从 free_list_ 获取或从 replacer 牺牲。
  return INVALID_FRAME_ID; // 占位符，不使用。
}