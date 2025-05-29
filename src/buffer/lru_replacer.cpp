#include "buffer/lru_replacer.h"

LRUReplacer::LRUReplacer(size_t num_pages){}

LRUReplacer::~LRUReplacer() = default;

/**
 * TODO: Student Implement
 */
// Victim函数：替换（即删除）与所有被跟踪的页相比最近最少被访问的页。
// 将其页帧号存储在输出参数frame_id中输出并返回true，如果当前没有可以替换的元素则返回false。
bool LRUReplacer::Victim(frame_id_t *frame_id) {
  std::scoped_lock<std::mutex> lock(latch_); // 使用 scoped_lock 自动管理锁的获取和释放，保证线程安全。

  // 如果lru_list_为空，表示当前没有可以替换的页帧。
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
 * TODO: Student Implement
 */
// Pin函数：将数据页固定使之不能被Replacer替换，即从lru_list_中移除该数据页对应的页帧。
// Pin函数应当在一个数据页被Buffer Pool Manager固定时被调用。
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
 * TODO: Student Implement
 */
// Unpin函数：将数据页解除固定，放入lru_list_中，使之可以在必要时被Replacer替换掉。
// Unpin函数应当在一个数据页的引用计数变为0时被Buffer Pool Manager调用，使页帧对应的数据页能够在必要时被替换。
void LRUReplacer::Unpin(frame_id_t frame_id) {
  std::scoped_lock<std::mutex> lock(latch_); // 加锁保护并发访问。

  // 检查该页帧是否已经存在于frame_map_中。如果已经存在，说明它已经在lru_list_中，无需再次添加。
  // 这种情况可能发生在页帧被Pin后又被Unpin，但在此期间没有被Victim。
  if (frame_map_.find(frame_id) == frame_map_.end()) {
    // 将页帧添加到lru_list_的头部，表示它最近被访问过（即成为MRU）。
    lru_list_.push_front(frame_id);
    // 在frame_map_中存储页帧ID及其在lru_list_中的迭代器，以便后续快速查找和移除。
    frame_map_[frame_id] = lru_list_.begin();
  }
}

/**
 * TODO: Student Implement
 */
// Size函数：此方法返回当前LRUReplacer中能够被替换的数据页的数量。
size_t LRUReplacer::Size() {
  std::scoped_lock<std::mutex> lock(latch_); // 加锁保护并发访问。
  return lru_list_.size(); // 返回lru_list_中元素的数量，即当前可被替换的页帧数量。
}