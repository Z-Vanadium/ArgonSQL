#include "buffer/lru_replacer.h"

// 构造函数：初始化 replacer 的最大容量。
// num_pages 参数通常用于表示缓冲池的大小，虽然 LRUReplacer 内部不直接用它限制 lru_list_ 的大小，
// 但它是作为容量信息提供给 BufferPoolManager 的。
LRUReplacer::LRUReplacer(size_t num_pages) : capacity_(num_pages) {}

// 析构函数：默认实现。
LRUReplacer::~LRUReplacer() = default;

/**
 * Victim函数：替换（即删除）与所有被跟踪的页相比最近最少被访问的页。
 * 将其页帧号存储在输出参数frame_id中输出并返回true，如果当前没有可以替换的元素则返回false。
 */
bool LRUReplacer::Victim(frame_id_t *frame_id) {
  std::scoped_lock<std::mutex> lock(latch_); // 使用 scoped_lock 自动管理锁的获取和释放，保证线程安全。

  // 如果lru_list_为空，表示当前没有可以替换的页帧，返回false。
  if (lru_list_.empty()) {
    return false;
  }

  // 最近最少使用的页帧在lru_list_的尾部。
  *frame_id = lru_list_.back(); // 获取lru_list_尾部的页帧ID。
  lru_list_.pop_back();        // 从lru_list_中移除该页帧。

  // 从frame_map_中移除对应的页帧记录，因为该页帧已经被替换。
  frame_map_.erase(*frame_id);

  return true;
}

/**
 * Pin函数：将数据页固定使之不能被Replacer替换，即从lru_list_中移除该数据页对应的页帧。
 * Pin函数应当在一个数据页被Buffer Pool Manager固定时被调用。
 */
void LRUReplacer::Pin(frame_id_t frame_id) {
  std::scoped_lock<std::mutex> lock(latch_); // 加锁保护并发访问。

  // 在frame_map_中查找该页帧是否存在。
  auto it = frame_map_.find(frame_id);
  // 如果找到，说明该页帧当前在lru_list_中（即可以被替换）。
  if (it != frame_map_.end()) {
    lru_list_.erase(it->second); // 从lru_list_中移除该页帧，因为它现在被“固定”了。
    frame_map_.erase(it);        // 同时从frame_map_中移除对应的映射。
  }
}

/**
 * Unpin函数：将数据页解除固定，放入lru_list_中，使之可以在必要时被Replacer替换掉。
 * Unpin函数应当在一个数据页的引用计数变为0时被Buffer Pool Manager调用，使页帧对应的数据页能够在必要时被替换。
 * 此实现严格遵循 LRUReplacerTest 中 SampleTest 的期望：
 * 如果页帧已经存在于 replacer 中（即未被 Pin 移除），则再次 Unpin 不改变其位置。
 * 只有当页帧不在 replacer 中时（因为它被 Pin 了或者从未被 Unpin 过），才将其加入到 MRU 端。
 */
void LRUReplacer::Unpin(frame_id_t frame_id) {
  std::scoped_lock<std::mutex> lock(latch_); // 加锁保护并发访问。

  // 检查 frame_id 是否已经在 replacer 中（即在 frame_map_ 中）。
  // 如果 frame_id 已经存在于 replacer 中，根据 LRUReplacerTest 的要求，我们不应改变其位置。
  // 因此，只有当它不存在时，才将其添加到 lru_list_ 的 MRU 端。
  if (frame_map_.find(frame_id) == frame_map_.end()) {
    // 将 frame_id 插入到 lru_list_ 的头部 (MRU 端)。
    lru_list_.push_front(frame_id);
    // 在 frame_map_ 中存储新的映射，将 frame_id 映射到 lru_list_ 新的头部迭代器。
    frame_map_[frame_id] = lru_list_.begin();
  }
  // 如果 frame_id 已经存在，则不执行任何操作，保持其在 LRU 队列中的现有位置。
}

/**
 * Size函数：返回当前replacer中可以被替换的页帧的数量。
 */
size_t LRUReplacer::Size() {
  std::scoped_lock<std::mutex> lock(latch_); // 加锁保护并发访问。
  return lru_list_.size();                   // 返回lru_list_中元素的数量。
}