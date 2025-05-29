#include "storage/disk_manager.h"

#include <sys/stat.h>

#include <filesystem>
#include <stdexcept>
#include <cstring> // For memset, though Init() handles it now

#include "glog/logging.h"
#include "page/bitmap_page.h"
#include "page/disk_file_meta_page.h"

DiskManager::DiskManager(const std::string &db_file) : file_name_(db_file) {
  std::scoped_lock<std::recursive_mutex> lock(db_io_latch_);
  db_io_.open(db_file, std::ios::binary | std::ios::in | std::ios::out);
  // directory or file does not exist
  if (!db_io_.is_open()) {
    db_io_.clear();
    // create a new file
    std::filesystem::path p = db_file;
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
    db_io_.open(db_file, std::ios::binary | std::ios::trunc | std::ios::out);
    db_io_.close();
    // reopen with original mode
    db_io_.open(db_file, std::ios::binary | std::ios::in | std::ios::out);
    if (!db_io_.is_open()) {
      throw std::exception();
    }
    // New file created, initialize meta_data_ and write it to disk
    DiskFileMetaPage *meta_page = reinterpret_cast<DiskFileMetaPage *>(meta_data_); //
    meta_page->Init(); // Call Init to properly set up the metadata page
    WritePhysicalPage(META_PAGE_ID, meta_data_); // Write the initialized meta_data_ to disk
  } else {
    // Existing file, read meta_data_ from disk
    ReadPhysicalPage(META_PAGE_ID, meta_data_); //
  }
}

void DiskManager::Close() {
  std::scoped_lock<std::recursive_mutex> lock(db_io_latch_);
  WritePhysicalPage(META_PAGE_ID, meta_data_);
  if (!closed) {
    db_io_.close();
    closed = true;
  }
}

void DiskManager::ReadPage(page_id_t logical_page_id, char *page_data) {
  ASSERT(logical_page_id >= 0, "Invalid page id.");
  ReadPhysicalPage(MapPageId(logical_page_id), page_data);
}

void DiskManager::WritePage(page_id_t logical_page_id, const char *page_data) {
  ASSERT(logical_page_id >= 0, "Invalid page id.");
  WritePhysicalPage(MapPageId(logical_page_id), page_data);
}

/**
 * TODO: Student Implement
 */
/**
 * 从磁盘分配一个空闲页，返回逻辑页号
 * 实现步骤：
 * 1. 先在现有分区中查找有空闲页的分区
 * 2. 如果找到，则在该分区的位图页中分配页
 * 3. 如果所有分区都满，则创建新分区并分配页
 * 4. 更新元数据信息
 */
page_id_t DiskManager::AllocatePage() {
  // std::scoped_lock lock(db_io_latch_);
  ReadPhysicalPage(META_PAGE_ID, meta_data_);
  DiskFileMetaPage *meta_page = reinterpret_cast<DiskFileMetaPage *>(meta_data_);

  if(meta_page->GetAllocatedPages() >= MAX_VALID_PAGE_ID) {
    return INVALID_PAGE_ID;
  }

  // 遍历现有分区，查找有空闲页的分区
  for (page_id_t extent_id = 0; extent_id < meta_page->GetExtentNums(); extent_id++) { // 使用 GetExtentNums()
    // 检查当前分区是否还有空闲页
    // 现在我们可以信任 GetExtentUsedPage() 返回正确的值，因为它基于修正后的数组

    page_id_t bitmap_physical_page = META_PAGE_ID + 1 + extent_id * (1 + BITMAP_SIZE);
    char bitmap_buffer[PAGE_SIZE];
    ReadPhysicalPage(bitmap_physical_page, bitmap_buffer);
    auto *page_bitmap = reinterpret_cast<BitmapPage<PAGE_SIZE> *>(bitmap_buffer);
    uint32_t page_offset;
    if(page_bitmap->AllocatePage(page_offset)){
      WritePhysicalPage(bitmap_physical_page, bitmap_buffer);
      
      meta_page->num_allocated_pages_ ++;
      meta_page->extent_used_page_[extent_id] ++;

      WritePhysicalPage(META_PAGE_ID, meta_data_);
      return extent_id * BITMAP_SIZE + page_offset;
    }
  }

  // 所有分区都已满，需要创建新分区
  uint32_t new_extent_id = meta_page->GetExtentNums(); // 使用 GetExtentNums() 获取当前分区数量作为新分区ID

  // 检查是否超过最大分区限制
  // 现在我们可以使用 DiskFileMetaPage::MAX_EXTENT_NUM，因为它已被正确定义
  // if (new_extent_id >= DiskFileMetaPage::MAX_EXTENT_NUM) {
  //   return INVALID_PAGE_ID; // 达到最大分区限制
  // }

  // 初始化新分区的位图页
  // BitmapPage<PAGE_SIZE> new_bitmap_page;
  uint32_t page_offset;
  // if (!new_bitmap_page.AllocatePage(page_offset)) {
  //   // 理论上不会走到这里，除非BITMAP_SIZE为0或者BitmapPage实现有误
  //   return INVALID_PAGE_ID;
  // }

  page_id_t new_bitmap_physical_page = META_PAGE_ID + 1 + new_extent_id * (BITMAP_SIZE + 1);
  char new_bitmap_buffer[PAGE_SIZE] = {0};
  WritePhysicalPage(new_bitmap_physical_page, new_bitmap_buffer);
  for (uint32_t i = 1; i <= BITMAP_SIZE; i++) {
    WritePhysicalPage(new_bitmap_physical_page + i, new_bitmap_buffer);
  }

  auto new_page_bitmap = reinterpret_cast<BitmapPage<PAGE_SIZE>*>(new_bitmap_buffer);
  page_offset = 0;
  new_page_bitmap->AllocatePage(page_offset);
  WritePhysicalPage(new_bitmap_physical_page, new_bitmap_buffer);

  // 更新元数据信息
  meta_page->num_extents_++; // 增加分区数量
  meta_page->num_allocated_pages_++; // 已分配页总数加1
  meta_page->extent_used_page_[new_extent_id] = 1;  // 新分区已分配1页
  WritePhysicalPage(META_PAGE_ID, meta_data_);

  return new_extent_id * BITMAP_SIZE + page_offset;
}

/**
 * 释放指定逻辑页号对应的物理页
 */
void DiskManager::DeAllocatePage(page_id_t logical_page_id) {
  // if (logical_page_id == INVALID_PAGE_ID || logical_page_id == META_PAGE_ID) return;

  DiskFileMetaPage *meta_page = reinterpret_cast<DiskFileMetaPage *>(meta_data_);
  uint32_t extent_id = logical_page_id / BITMAP_SIZE;
  uint32_t page_offset = logical_page_id % BITMAP_SIZE;

  // 检查分区ID是否有效，即是否在已存在的分区范围内
  if (extent_id >= meta_page->GetExtentNums()) { // 使用 GetExtentNums()
    return;
  }

  page_id_t bitmap_physical_page = META_PAGE_ID + 1 + extent_id * (BITMAP_SIZE + 1);
  char bitmap_buffer[PAGE_SIZE];
  ReadPhysicalPage(bitmap_physical_page, bitmap_buffer);
  auto page_bitmap = reinterpret_cast<BitmapPage<PAGE_SIZE>*>(bitmap_buffer);

  if (page_bitmap->DeAllocatePage(page_offset)) {
    meta_page->num_allocated_pages_--;
    meta_page->extent_used_page_[extent_id]--;
    WritePhysicalPage(bitmap_physical_page, bitmap_buffer);
    WritePhysicalPage(META_PAGE_ID, meta_data_);
  }
}

/**
 * 检查指定逻辑页号对应的页是否空闲
 */
bool DiskManager::IsPageFree(page_id_t logical_page_id) {
  if (logical_page_id == INVALID_PAGE_ID || logical_page_id == META_PAGE_ID) return true;

  DiskFileMetaPage *meta_page = reinterpret_cast<DiskFileMetaPage *>(meta_data_);
  uint32_t extent_id = logical_page_id / BITMAP_SIZE;
  uint32_t page_offset = logical_page_id % BITMAP_SIZE;

  // 检查分区ID是否有效
  if (extent_id >= meta_page->GetExtentNums()) { // 使用 GetExtentNums()
    return true;
  }

  page_id_t bitmap_physical_page = 1 + extent_id * (BITMAP_SIZE + 1);
  BitmapPage<PAGE_SIZE> bitmap_page;
  ReadPhysicalPage(bitmap_physical_page, reinterpret_cast<char *>(&bitmap_page));

  bool is_free = bitmap_page.IsPageFree(page_offset);
  return is_free;
}

/**
 * 将逻辑页号映射为物理页号
 */
page_id_t DiskManager::MapPageId(page_id_t logical_page_id) {
  if (logical_page_id == META_PAGE_ID) return META_PAGE_ID;
  if (logical_page_id == INVALID_PAGE_ID) return INVALID_PAGE_ID;

  DiskFileMetaPage *meta_page = reinterpret_cast<DiskFileMetaPage *>(meta_data_);
  uint32_t extent_id = logical_page_id / BITMAP_SIZE;
  uint32_t page_offset = logical_page_id % BITMAP_SIZE;

  // 检查分区ID是否有效
  if (extent_id >= meta_page->GetExtentNums()) { // 使用 GetExtentNums()
    return INVALID_PAGE_ID;
  }

  return 1 + extent_id * (BITMAP_SIZE + 1) + 1 + page_offset;
}

int DiskManager::GetFileSize(const std::string &file_name) {
  struct stat stat_buf;
  int rc = stat(file_name.c_str(), &stat_buf);
  return rc == 0 ? stat_buf.st_size : -1;
}

void DiskManager::ReadPhysicalPage(page_id_t physical_page_id, char *page_data) {
  int offset = physical_page_id * PAGE_SIZE;
  // check if read beyond file length
  if (offset >= GetFileSize(file_name_)) {
#ifdef ENABLE_BPM_DEBUG
    LOG(INFO) << "Read less than a page" << std::endl;
#endif
    memset(page_data, 0, PAGE_SIZE);
  } else {
    // set read cursor to offset
    db_io_.seekp(offset);
    db_io_.read(page_data, PAGE_SIZE);
    // if file ends before reading PAGE_SIZE
    int read_count = db_io_.gcount();
    if (read_count < PAGE_SIZE) {
#ifdef ENABLE_BPM_DEBUG
      LOG(INFO) << "Read less than a page" << std::endl;
#endif
      memset(page_data + read_count, 0, PAGE_SIZE - read_count);
    }
  }
}

void DiskManager::WritePhysicalPage(page_id_t physical_page_id, const char *page_data) {
  size_t offset = static_cast<size_t>(physical_page_id) * PAGE_SIZE;
  // set write cursor to offset
  db_io_.seekp(offset);
  db_io_.write(page_data, PAGE_SIZE);
  // check for I/O error
  if (db_io_.bad()) {
    LOG(ERROR) << "I/O error while writing";
    return;
  }
  // needs to flush to keep disk file in sync
  db_io_.flush();
}