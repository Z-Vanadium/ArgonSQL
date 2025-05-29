#ifndef MINISQL_LRU_REPLACER_H
#define MINISQL_LRU_REPLACER_H

#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "buffer/replacer.h"
#include "common/config.h"

using namespace std;

/**
 * LRUReplacer implements the Least Recently Used replacement policy.
 */
class LRUReplacer : public Replacer {
 public:
  /**
   * Create a new LRUReplacer.
   * @param num_pages the maximum number of pages the LRUReplacer will be required to store
   */
  explicit LRUReplacer(size_t num_pages);

  /**
   * Destroys the LRUReplacer.
   */
  ~LRUReplacer() override;

  bool Victim(frame_id_t *frame_id) override;

  void Pin(frame_id_t frame_id) override;

  void Unpin(frame_id_t frame_id) override;

  size_t Size() override;

private:
  // add your own private member variables here
  // lru_list_：双向链表，用于存储可以被替换的页帧ID。
  //            链表头部是最近访问的页帧 (MRU)，尾部是最近最少访问的页帧 (LRU)。
  std::list<frame_id_t> lru_list_;
  
  // frame_map_：哈希表，用于快速查找页帧ID在lru_list_中的位置（迭代器）。
  //             key为frame_id，value为lru_list_中对应元素的迭代器。
  std::unordered_map<frame_id_t, std::list<frame_id_t>::iterator> frame_map_;
  
  // latch_：互斥锁，用于保护lru_list_和frame_map_在多线程环境下的并发访问。
  mutable std::mutex latch_; 
  
  // capacity_：LRUReplacer可以存储的最大页帧数量。
  size_t capacity_;
};

#endif  // MINISQL_LRU_REPLACER_H
