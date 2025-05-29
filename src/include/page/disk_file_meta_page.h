#ifndef MINISQL_DISK_FILE_META_PAGE_H
#define MINISQL_DISK_FILE_META_PAGE_H

#include <cstdint>
#include <cstring> // For memset

#include "page/bitmap_page.h"
#include "common/config.h" // 包含 PAGE_SIZE 的定义，如果它不在 bitmap_page.h 中

// 仍然保留 MAX_VALID_PAGE_ID，因为它可能在其他地方被使用
static constexpr page_id_t MAX_VALID_PAGE_ID = (PAGE_SIZE - 8) / 4 * BitmapPage<PAGE_SIZE>::GetMaxSupportedSize();

class DiskFileMetaPage {
public:
  // 定义一个合理的最大分区数量
  // PAGE_SIZE 减去 num_allocated_pages_ (uint32_t) 和 num_extents_ (uint32_t) 的大小，
  // 剩余空间可以存储多少个 uint32_t 类型的 extent_used_page_ 数组元素。
  // 确保 MAX_EXTENT_NUM 小于或等于实际可用空间，以避免元数据页溢出。
  static constexpr uint32_t MAX_EXTENT_NUM = (PAGE_SIZE - sizeof(uint32_t) * 2) / sizeof(uint32_t);

  // Fills this header with initial information
  void Init() {
    num_allocated_pages_ = 0;
    num_extents_ = 0;
    memset(extent_used_page_, 0, sizeof(uint32_t) * MAX_EXTENT_NUM); // 初始化数组
  }

  uint32_t GetExtentNums() { return num_extents_; }

  uint32_t GetAllocatedPages() { return num_allocated_pages_; }

  uint32_t GetExtentUsedPage(uint32_t extent_id) {
    // 增加范围检查，确保访问有效索引
    if (extent_id >= num_extents_ || extent_id >= MAX_EXTENT_NUM) {
      return 0; // 或者抛出异常
    }
    return extent_used_page_[extent_id];
  }

public:
  uint32_t num_allocated_pages_{0};
  uint32_t num_extents_{0};  // each extent consists with a bit map and BIT_MAP_SIZE pages
  uint32_t extent_used_page_[MAX_EXTENT_NUM]; // <-- 修正此处，使其成为固定大小数组
};

#endif  // MINISQL_DISK_FILE_META_PAGE_H
