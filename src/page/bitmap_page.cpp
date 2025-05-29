#include "page/bitmap_page.h"

#include "glog/logging.h"

/**
 * TODO: Student Implement
 */
template <size_t PageSize>
bool BitmapPage<PageSize>::AllocatePage(uint32_t &page_offset) {
  // 计算最大页数（每个bit代表一页）
  const uint32_t max_pages = (PageSize - 8) * 8; // 减去两个uint32_t的成员变量

  // 快速失败：所有页已分配
  if (page_allocated_ >= max_pages) {
    return false;
  }

  // 环形搜索从next_free_page_开始
  for (uint32_t i = 0; i < max_pages; ++i) {
    uint32_t current = (next_free_page_ + i) % max_pages;
    uint32_t byte_pos = current / 8;
    uint8_t bit_pos = current % 8;

    if ((bytes[byte_pos] & (1 << bit_pos)) == 0) {
      // 设置bit位
      bytes[byte_pos] |= (1 << bit_pos);
      page_allocated_++;
      next_free_page_ = (current + 1) % max_pages;
      page_offset = current;
      return true;
    }
  }
return false;
}

/**
 * TODO: Student Implement
 */
template <size_t PageSize>
bool BitmapPage<PageSize>::DeAllocatePage(uint32_t page_offset) {
 const size_t max_pages = GetMaxSupportedSize();
  
  // 情况1：偏移越界 或 页本身未分配
  if (page_offset >= max_pages || IsPageFree(page_offset)) {
    return false;
  }

  const uint32_t byte_idx = page_offset / 8;   // 计算字节位置
  const uint8_t bit_idx = page_offset % 8;     // 计算bit位置
  
  bytes[byte_idx] &= ~(1 << bit_idx);          // 清除对应bit为0
  page_allocated_--;                           // 更新已分配页数
  next_free_page_ = page_offset;               // 优化下次搜索起点

  return true;
}

/**
 * TODO: Student Implement
 */
template <size_t PageSize>
bool BitmapPage<PageSize>::IsPageFree(uint32_t page_offset) const {
  // 检查偏移合法性
  if (page_offset >= GetMaxSupportedSize()) {
    return false;
  }
  
  // 计算索引并调用底层检查
  const uint32_t byte_idx = page_offset / 8;
  const uint8_t bit_idx = page_offset % 8;
  return IsPageFreeLow(byte_idx, bit_idx);
}

template <size_t PageSize>
bool BitmapPage<PageSize>::IsPageFreeLow(uint32_t byte_index, uint8_t bit_index) const {
  // 检查字节索引是否越界
  if (byte_index >= MAX_CHARS) {
    return false;
  }
  
  // 位运算检查bit是否为0
  return (bytes[byte_index] & (1 << bit_index)) == 0;
}

template class BitmapPage<64>;

template class BitmapPage<128>;

template class BitmapPage<256>;

template class BitmapPage<512>;

template class BitmapPage<1024>;

template class BitmapPage<2048>;

template class BitmapPage<4096>;