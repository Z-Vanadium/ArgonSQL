#include "storage/disk_manager.h"

#include <sys/stat.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <cstring>

#include "glog/logging.h"
#include "page/bitmap_page.h"
#include "page/disk_file_meta_page.h"

/**
 * DiskManager 构造函数
 * 负责打开或创建数据库文件，并初始化文件流和元数据。
 * @param db_file 数据库文件名
 */
DiskManager::DiskManager(const std::string &db_file) : file_name_(db_file) {
  // 使用互斥锁保护文件I/O操作，确保线程安全
  std::scoped_lock<std::recursive_mutex> lock(db_io_latch_);

  // 1. 检查物理文件是否存在
  if (!std::filesystem::exists(db_file)) {
    // 如果文件不存在，则安全地创建它
    // a. 确保父目录存在
    std::filesystem::path p = db_file;
    if (p.has_parent_path()) {
      std::filesystem::create_directories(p.parent_path());
    }
    // b. 使用 ofstream 创建文件，这是最可靠的跨平台文件创建方式
    std::ofstream create_file(db_file, std::ios::binary);
    if (!create_file) {
      throw std::runtime_error("Failed to create database file: " + db_file);
    }
    create_file.close(); // 创建后立即关闭
  }

  // 2. 文件现在保证存在，以读/写/二进制模式打开它
  db_io_.open(db_file, std::ios::in | std::ios::out | std::ios::binary);
  if (!db_io_.is_open()) {
    throw std::runtime_error("Failed to open database file for R/W: " + db_file);
  }

  // 3. 检查文件大小，判断是否为新文件，以决定是否需要初始化元数据
  if (GetFileSize(db_file) == 0) {
    // 文件为空，说明是新创建的，需要初始化元数据页
    DiskFileMetaPage *meta_page = reinterpret_cast<DiskFileMetaPage *>(meta_data_);
    meta_page->Init(); // 初始化内存中的元数据对象
    WritePhysicalPage(META_PAGE_ID, meta_data_); // 将初始化的元数据写入磁盘的第0页
  } else {
    // 文件已存在且有内容，直接读取元数据页到内存
    ReadPhysicalPage(META_PAGE_ID, meta_data_);
  }
}

/**
 * 关闭DiskManager，将元数据写回并关闭文件流。
 */
void DiskManager::Close() {
  std::scoped_lock<std::recursive_mutex> lock(db_io_latch_);
  if (!closed) {
    WritePhysicalPage(META_PAGE_ID, meta_data_);
    db_io_.close();
    closed = true;
  }
}

/**
 * 读取一个逻辑页。
 * 它通过MapPageId将逻辑页号转换为物理页号，然后调用ReadPhysicalPage。
 */
void DiskManager::ReadPage(page_id_t logical_page_id, char *page_data) {
  ASSERT(logical_page_id >= 0, "Invalid page id.");
  ReadPhysicalPage(MapPageId(logical_page_id), page_data);
}

/**
 * 写入一个逻辑页。
 * 它通过MapPageId将逻辑页号转换为物理页号，然后调用WritePhysicalPage。
 */
void DiskManager::WritePage(page_id_t logical_page_id, const char *page_data) {
  ASSERT(logical_page_id >= 0, "Invalid page id.");
  WritePhysicalPage(MapPageId(logical_page_id), page_data);
}

/**
 * 从磁盘分配一个空闲页，返回其逻辑页号。
 */
page_id_t DiskManager::AllocatePage() {
  // 读入最新元数据，确保操作基于最新状态
  ReadPhysicalPage(META_PAGE_ID, meta_data_);
  auto *meta_page = reinterpret_cast<DiskFileMetaPage *>(meta_data_);

  // 1. 遍历现有分区(extent)，寻找空闲页
  for (uint32_t extent_id = 0; extent_id < meta_page->GetExtentNums(); ++extent_id) {
    // 优化：通过元数据快速判断分区是否已满
    if (meta_page->extent_used_page_[extent_id] < BITMAP_SIZE) {
      page_id_t bitmap_physical_page = 1 + extent_id * (BITMAP_SIZE + 1);
      char bitmap_buffer[PAGE_SIZE];
      ReadPhysicalPage(bitmap_physical_page, bitmap_buffer);
      auto *bitmap = reinterpret_cast<BitmapPage<PAGE_SIZE> *>(bitmap_buffer);

      uint32_t page_offset = 0; // 初始化以防万一
      if (bitmap->AllocatePage(page_offset)) {
        // 分配成功，更新元数据
        meta_page->num_allocated_pages_++;
        meta_page->extent_used_page_[extent_id]++;

        // 将更新后的位图和元数据都写回磁盘，确保持久化
        WritePhysicalPage(bitmap_physical_page, bitmap_buffer);
        WritePhysicalPage(META_PAGE_ID, meta_data_);

        return extent_id * BITMAP_SIZE + page_offset;
      }
    }
  }

  // 2. 现有分区均已满，尝试创建新分区
  if (meta_page->GetExtentNums() >= DiskFileMetaPage::MAX_EXTENT_NUM) {
    return INVALID_PAGE_ID; // 达到最大分区数限制
  }

  // 3. 创建新分区
  uint32_t new_extent_id = meta_page->GetExtentNums();

  // 关键修正：预先扩展物理文件的大小，防止后续写操作失败
  // 我们通过向新分区的每一页写入一个空页，来确保文件被撑大到足够容纳这个新分区
  char empty_page[PAGE_SIZE] = {0};
  page_id_t new_extent_start_physical_page = 1 + new_extent_id * (BITMAP_SIZE + 1);
  for (uint32_t i = 0; i < BITMAP_SIZE + 1; ++i) {
      WritePhysicalPage(new_extent_start_physical_page + i, empty_page);
  }

  // 4. 文件大小已保证，现在正式初始化新分区的位图
  page_id_t bitmap_physical_page = new_extent_start_physical_page;
  char bitmap_buffer[PAGE_SIZE];
  memcpy(bitmap_buffer, empty_page, PAGE_SIZE); // 使用全0的缓冲区初始化位图

  auto *bitmap = reinterpret_cast<BitmapPage<PAGE_SIZE> *>(bitmap_buffer);
  uint32_t page_offset = 0;
  if (bitmap->AllocatePage(page_offset)) {
    // 在新位图中分配成功，更新元数据
    meta_page->num_extents_++;
    meta_page->num_allocated_pages_++;
    meta_page->extent_used_page_[new_extent_id] = 1;
    // 将更新后的位图和元数据写回磁盘
    WritePhysicalPage(bitmap_physical_page, bitmap_buffer);
    WritePhysicalPage(META_PAGE_ID, meta_data_);
    // 返回新分配的页的逻辑ID
    return new_extent_id * BITMAP_SIZE + page_offset;
  }

  return INVALID_PAGE_ID;
}

/**
 * 释放一个逻辑页。
 */
void DiskManager::DeAllocatePage(page_id_t logical_page_id) {
  // 读入最新元数据
  ReadPhysicalPage(META_PAGE_ID, meta_data_);
  auto *meta_page = reinterpret_cast<DiskFileMetaPage *>(meta_data_);

  uint32_t extent_id = logical_page_id / BITMAP_SIZE;
  if (extent_id >= meta_page->GetExtentNums()) {
    return; // 分区不存在，无需操作
  }
  uint32_t page_offset = logical_page_id % BITMAP_SIZE;

  page_id_t bitmap_physical_page = 1 + extent_id * (BITMAP_SIZE + 1);
  char bitmap_buffer[PAGE_SIZE];
  ReadPhysicalPage(bitmap_physical_page, bitmap_buffer);
  auto *bitmap = reinterpret_cast<BitmapPage<PAGE_SIZE> *>(bitmap_buffer);

  if (bitmap->DeAllocatePage(page_offset)) {
    // 释放成功，更新元数据并写回
    meta_page->num_allocated_pages_--;
    meta_page->extent_used_page_[extent_id]--;
    WritePhysicalPage(bitmap_physical_page, bitmap_buffer);
    WritePhysicalPage(META_PAGE_ID, meta_data_);
  }
}

/**
 * 检查一个逻辑页是否空闲。
 */
bool DiskManager::IsPageFree(page_id_t logical_page_id) {
  ReadPhysicalPage(META_PAGE_ID, meta_data_);
  auto *meta_page = reinterpret_cast<DiskFileMetaPage *>(meta_data_);
  uint32_t extent_id = logical_page_id / BITMAP_SIZE;
  if (extent_id >= meta_page->GetExtentNums()) {
    return true; // 分区不存在，页自然是“空闲”的
  }
  uint32_t page_offset = logical_page_id % BITMAP_SIZE;
  page_id_t bitmap_physical_page = 1 + extent_id * (BITMAP_SIZE + 1);
  char bitmap_buffer[PAGE_SIZE];
  ReadPhysicalPage(bitmap_physical_page, bitmap_buffer);
  auto *bitmap = reinterpret_cast<BitmapPage<PAGE_SIZE> *>(bitmap_buffer);
  return bitmap->IsPageFree(page_offset);
}

/**
 * 将逻辑页号映射为物理页号。
 * 这是一个纯粹的数学计算，不应依赖于当前元数据状态。
 */
page_id_t DiskManager::MapPageId(page_id_t logical_page_id) {
  if (logical_page_id < 0) {
    return INVALID_PAGE_ID;
  }
  uint32_t extent_id = logical_page_id / BITMAP_SIZE;
  uint32_t page_offset = logical_page_id % BITMAP_SIZE;
  // 物理页号 = 元数据页(1) + 所有在它之前的完整分区大小 + 当前分区的位图页(1) + 在分区内的偏移
  return 1 + extent_id * (BITMAP_SIZE + 1) + 1 + page_offset;
}

/**
 * 辅助函数，获取磁盘文件大小。
 */
int DiskManager::GetFileSize(const std::string &file_name) {
  struct stat stat_buf;
  int rc = stat(file_name.c_str(), &stat_buf);
  return rc == 0 ? stat_buf.st_size : -1;
}

/**
 * 从磁盘读取一个物理页。
 */
void DiskManager::ReadPhysicalPage(page_id_t physical_page_id, char *page_data) {
  int offset = physical_page_id * PAGE_SIZE;
  // 检查读取是否会超出文件末尾
  if (offset >= GetFileSize(file_name_)) {
    memset(page_data, 0, PAGE_SIZE);
  } else {
    // 关键修正：使用 seekg() 来定位读指针
    db_io_.seekg(offset);
    if (db_io_.bad()) { LOG(ERROR) << "I/O error while seeking for read"; return; }
    db_io_.read(page_data, PAGE_SIZE);
    // 如果文件在读取PAGE_SIZE字节前就结束了，用0填充剩余部分
    int read_count = db_io_.gcount();
    if(read_count < PAGE_SIZE) {
      memset(page_data + read_count, 0, PAGE_SIZE - read_count);
    }
  }
}

/**
 * 将一个物理页写入磁盘。
 */
void DiskManager::WritePhysicalPage(page_id_t physical_page_id, const char *page_data) {
  size_t offset = static_cast<size_t>(physical_page_id) * PAGE_SIZE;
  // 使用 seekp() 定位写指针
  db_io_.seekp(offset);
  if (db_io_.bad()) { LOG(ERROR) << "I/O error while seeking for write"; return; }
  db_io_.write(page_data, PAGE_SIZE);
  if (db_io_.bad()) { LOG(ERROR) << "I/O error while writing"; return; }
  // flush()确保数据从程序缓冲区推向操作系统
  db_io_.flush();
}