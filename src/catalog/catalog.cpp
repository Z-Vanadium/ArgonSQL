#include "catalog/catalog.h"

void CatalogMeta::SerializeTo(char *buf) const {
  ASSERT(GetSerializedSize() <= PAGE_SIZE, "Failed to serialize catalog metadata to disk.");
  MACH_WRITE_UINT32(buf, CATALOG_METADATA_MAGIC_NUM);
  buf += 4;
  MACH_WRITE_UINT32(buf, table_meta_pages_.size());
  buf += 4;
  MACH_WRITE_UINT32(buf, index_meta_pages_.size());
  buf += 4;
  for (auto iter : table_meta_pages_) {
    MACH_WRITE_TO(table_id_t, buf, iter.first);
    buf += 4;
    MACH_WRITE_TO(page_id_t, buf, iter.second);
    buf += 4;
  }
  for (auto iter : index_meta_pages_) {
    MACH_WRITE_TO(index_id_t, buf, iter.first);
    buf += 4;
    MACH_WRITE_TO(page_id_t, buf, iter.second);
    buf += 4;
  }
}

CatalogMeta *CatalogMeta::DeserializeFrom(char *buf) {
  // check valid
  uint32_t magic_num = MACH_READ_UINT32(buf);
  buf += 4;
  ASSERT(magic_num == CATALOG_METADATA_MAGIC_NUM, "Failed to deserialize catalog metadata from disk.");
  // get table and index nums
  uint32_t table_nums = MACH_READ_UINT32(buf);
  buf += 4;
  uint32_t index_nums = MACH_READ_UINT32(buf);
  buf += 4;
  // create metadata and read value
  CatalogMeta *meta = new CatalogMeta();
  for (uint32_t i = 0; i < table_nums; i++) {
    auto table_id = MACH_READ_FROM(table_id_t, buf);
    buf += 4;
    auto table_heap_page_id = MACH_READ_FROM(page_id_t, buf);
    buf += 4;
    meta->table_meta_pages_.emplace(table_id, table_heap_page_id);
  }
  for (uint32_t i = 0; i < index_nums; i++) {
    auto index_id = MACH_READ_FROM(index_id_t, buf);
    buf += 4;
    auto index_page_id = MACH_READ_FROM(page_id_t, buf);
    buf += 4;
    meta->index_meta_pages_.emplace(index_id, index_page_id);
  }
  return meta;
}

/**
 * TODO: Student Implement
 */
uint32_t CatalogMeta::GetSerializedSize() const {
  // 该大小包括魔数、表和索引的数量，
  // 以及每个表/索引ID及其对应页ID所需的空间。
  return sizeof(uint32_t) * 3 + table_meta_pages_.size() * (sizeof(table_id_t) + sizeof(page_id_t)) +
         index_meta_pages_.size() * (sizeof(index_id_t) + sizeof(page_id_t));
}

CatalogMeta::CatalogMeta() {}

/**
 * TODO: Student Implement
 */
CatalogManager::CatalogManager(BufferPoolManager *buffer_pool_manager, LockManager *lock_manager,
                               LogManager *log_manager, bool init)
    : buffer_pool_manager_(buffer_pool_manager),
      lock_manager_(lock_manager),
      log_manager_(log_manager),
      catalog_meta_(nullptr),
      next_table_id_(0),
      next_index_id_(0) {
    if (init) {
        // 如果是初始化数据库，则创建新的目录元数据
        catalog_meta_ = CatalogMeta::NewInstance();
    } else {
        // 如果是重新打开数据库，则从磁盘加载目录元数据
        Page *meta_page = buffer_pool_manager_->FetchPage(CATALOG_META_PAGE_ID);
        ASSERT(meta_page != nullptr, "Failed to fetch catalog meta page.");
        catalog_meta_ = CatalogMeta::DeserializeFrom(meta_page->GetData());

        // 加载所有表和索引
        for (auto const& [table_id, page_id] : catalog_meta_->table_meta_pages_) {
            if (page_id != INVALID_PAGE_ID) {
                LoadTable(table_id, page_id);
            }
        }
        for (auto const& [index_id, page_id] : catalog_meta_->index_meta_pages_) {
            if (page_id != INVALID_PAGE_ID) {
                LoadIndex(index_id, page_id);
            }
        }

        // 设置下一个可用的ID
        next_table_id_ = catalog_meta_->GetNextTableId();
        next_index_id_ = catalog_meta_->GetNextIndexId();
        
        buffer_pool_manager_->UnpinPage(CATALOG_META_PAGE_ID, false);
    }
}

CatalogManager::~CatalogManager() {
  FlushCatalogMetaPage();
  delete catalog_meta_;
  for (auto iter : tables_) {
    delete iter.second;
  }
  for (auto iter : indexes_) {
    delete iter.second;
  }
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::CreateTable(const string &table_name, TableSchema *schema, Txn *txn, TableInfo *&table_info) {
    if (table_names_.count(table_name)) {
        return DB_TABLE_ALREADY_EXIST;
    }

    // 1. 目录必须拥有一份独立的 Schema。
    // TableHeap 会长期保存 Schema 指针，不能直接保存调用者的临时对象。
    TableSchema *new_schema = TableSchema::DeepCopySchema(schema);
    if (new_schema == nullptr) {
        return DB_FAILED;
    }

    // 2. TableHeap 和 TableMetadata 共同使用这份由目录拥有的 Schema。
    page_id_t root_page_id;
    TableHeap *table_heap = TableHeap::Create(buffer_pool_manager_, new_schema, txn, log_manager_, lock_manager_);
    if (table_heap == nullptr) {
        delete new_schema;
        return DB_FAILED;
    }
    root_page_id = table_heap->GetFirstPageId();

    // 3. 创建TableMetadata
    table_id_t table_id = next_table_id_++;
    TableMetadata* table_meta = TableMetadata::Create(table_id, table_name, root_page_id, new_schema);
    
    // 3. 创建TableInfo
    table_info = TableInfo::Create();
    table_info->Init(table_meta, table_heap);

    // 4. 将新的表信息加入到目录管理器的内部映射中
    tables_.emplace(table_id, table_info);
    table_names_.emplace(table_name, table_id);

    // 5. 将表的元数据序列化并存储到一个新页面
    page_id_t meta_page_id;
    Page *page = buffer_pool_manager_->NewPage(meta_page_id);
    if(page == nullptr) {
        // 清理已创建的资源
        table_heap->FreeTableHeap();
        delete table_info;
        tables_.erase(table_id);
        table_names_.erase(table_name);
        return DB_FAILED;
    }
    table_meta->SerializeTo(page->GetData());
    buffer_pool_manager_->UnpinPage(meta_page_id, true);

    // 6. 更新并持久化总的目录元数据
    catalog_meta_->table_meta_pages_.emplace(table_id, meta_page_id);
    FlushCatalogMetaPage();
    
    return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::GetTable(const string &table_name, TableInfo *&table_info) {
    auto it = table_names_.find(table_name);
    if (it == table_names_.end()) {
        return DB_TABLE_NOT_EXIST;
    }
    table_id_t table_id = it->second;
    return GetTable(table_id, table_info);
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::GetTables(vector<TableInfo *> &tables) const {
    for (const auto &pair : tables_) {
        tables.push_back(pair.second);
    }
    return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::CreateIndex(const std::string &table_name, const string &index_name,
                                    const std::vector<std::string> &index_keys, Txn *txn, IndexInfo *&index_info,
                                    const string &index_type) {
    // 1. 检查表是否存在
    TableInfo *table_info;
    if (GetTable(table_name, table_info) != DB_SUCCESS) {
        return DB_TABLE_NOT_EXIST;
    }
    
    // 2. 检查索引是否已存在
    if (index_names_.count(table_name) && index_names_[table_name].count(index_name)) {
        return DB_INDEX_ALREADY_EXIST;
    }

    // 3. 创建key_map (列名到列表达式中列索引的映射)
    std::vector<uint32_t> key_map;
    for (const auto &key_name : index_keys) {
        uint32_t column_index;
        if (table_info->GetSchema()->GetColumnIndex(key_name, column_index) != DB_SUCCESS) {
            return DB_COLUMN_NAME_NOT_EXIST;
        }
        key_map.push_back(column_index);
    }

    // 4. 创建IndexMetadata和IndexInfo
    index_id_t index_id = next_index_id_++;
    table_id_t table_id = table_info->GetTableId();
    IndexMetadata* index_meta = IndexMetadata::Create(index_id, index_name, table_id, key_map);
    index_info = IndexInfo::Create();
    index_info->Init(index_meta, table_info, buffer_pool_manager_);
    
    // 5. 更新内部映射
    indexes_.emplace(index_id, index_info);
    index_names_[table_name].emplace(index_name, index_id);

    // 6. 将索引元数据序列化到新页面
    page_id_t meta_page_id;
    Page *page = buffer_pool_manager_->NewPage(meta_page_id);
    if(page == nullptr) {
        // 清理
        delete index_info;
        indexes_.erase(index_id);
        index_names_[table_name].erase(index_name);
        return DB_FAILED;
    }
    index_meta->SerializeTo(page->GetData());
    buffer_pool_manager_->UnpinPage(meta_page_id, true);

    // 7. 更新并持久化总的目录元数据
    catalog_meta_->index_meta_pages_.emplace(index_id, meta_page_id);
    FlushCatalogMetaPage();

    return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::GetIndex(const std::string &table_name, const std::string &index_name,
                                 IndexInfo *&index_info) const {
    auto table_it = index_names_.find(table_name);
    if (table_it == index_names_.end()) {
        return DB_INDEX_NOT_FOUND;
    }
    auto index_it = table_it->second.find(index_name);
    if (index_it == table_it->second.end()) {
        return DB_INDEX_NOT_FOUND;
    }
    index_id_t index_id = index_it->second;
    auto final_it = indexes_.find(index_id);
    if(final_it == indexes_.end()) {
        return DB_INDEX_NOT_FOUND;
    }
    index_info = final_it->second;
    return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::GetTableIndexes(const std::string &table_name, std::vector<IndexInfo *> &indexes) const {
    auto it = index_names_.find(table_name);
    if (it != index_names_.end()) {
        for (const auto &pair : it->second) {
            IndexInfo *index_info;
            GetIndex(table_name, pair.first, index_info);
            indexes.push_back(index_info);
        }
    }
    return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::DropTable(const string &table_name) {
    TableInfo *table_info;
    if (GetTable(table_name, table_info) != DB_SUCCESS) {
        return DB_TABLE_NOT_EXIST;
    }
    table_id_t table_id = table_info->GetTableId();

    // 1. 删除与该表关联的所有索引
    std::vector<IndexInfo *> indexes_to_drop;
    GetTableIndexes(table_name, indexes_to_drop);
    for (auto index : indexes_to_drop) {
        DropIndex(table_name, index->GetIndexName());
    }

    // 2. 删除表的元数据页和堆数据
    buffer_pool_manager_->DeletePage(catalog_meta_->table_meta_pages_[table_id]);
    table_info->GetTableHeap()->FreeTableHeap();

    // 3. 从目录中移除表信息
    catalog_meta_->table_meta_pages_.erase(table_id);
    table_names_.erase(table_name);
    delete tables_[table_id]; // 释放TableInfo内存
    tables_.erase(table_id);
    
    // 4. 持久化目录元数据的更改
    FlushCatalogMetaPage();
    return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t CatalogManager::DropIndex(const string &table_name, const string &index_name) {
    IndexInfo *index_info;
    if (GetIndex(table_name, index_name, index_info) != DB_SUCCESS) {
        return DB_INDEX_NOT_FOUND;
    }
  index_id_t index_id = index_info->GetMetaData()->GetIndexId();

    // 1. 删除索引的元数据页和B+树数据
    buffer_pool_manager_->DeletePage(catalog_meta_->index_meta_pages_[index_id]);
    index_info->GetIndex()->Destroy();
    
    // 2. 从目录中移除索引信息
    catalog_meta_->index_meta_pages_.erase(index_id);
    index_names_[table_name].erase(index_name);
    if (index_names_[table_name].empty()) {
        index_names_.erase(table_name);
    }
    delete indexes_[index_id]; // 释放IndexInfo内存
    indexes_.erase(index_id);
    
    // 3. 持久化目录元数据的更改
    FlushCatalogMetaPage();
    return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
/**
 * @brief 将目录元数据页（CatalogMeta）刷回磁盘。
 * 这是确保所有对目录的更改（如创建/删除表和索引）被持久化的关键操作。
 * @return DB_SUCCESS 当操作成功时返回
 */
dberr_t CatalogManager::FlushCatalogMetaPage() const {
    Page *meta_page = buffer_pool_manager_->FetchPage(CATALOG_META_PAGE_ID);
    if (meta_page == nullptr) {
        return DB_FAILED;
    }
    catalog_meta_->SerializeTo(meta_page->GetData());
    buffer_pool_manager_->FlushPage(CATALOG_META_PAGE_ID);
    buffer_pool_manager_->UnpinPage(CATALOG_META_PAGE_ID, true);
    return DB_SUCCESS;
}


/**
 * TODO: Student Implement
 */
/**
 * @brief (内部辅助函数) 从指定的元数据页加载单个表的信息。
 * 在数据库启动时被构造函数调用。
 * @param table_id 要加载的表的ID
 * @param page_id 存储该表元数据的页面ID
 * @return DB_SUCCESS 当操作成功时返回
 */
dberr_t CatalogManager::LoadTable(const table_id_t table_id, const page_id_t page_id) {
    Page *page = buffer_pool_manager_->FetchPage(page_id);
    if (page == nullptr) {
        return DB_FAILED;
    }
    TableMetadata *table_meta = nullptr;
    TableMetadata::DeserializeFrom(page->GetData(), table_meta);
    buffer_pool_manager_->UnpinPage(page_id, false);
    
    TableHeap *table_heap = TableHeap::Create(buffer_pool_manager_,
                                            table_meta->GetFirstPageId(),
                                            table_meta->GetSchema(),
                                            log_manager_,
                                            lock_manager_);
    
    TableInfo *table_info = TableInfo::Create();
    table_info->Init(table_meta, table_heap);
    
    table_names_.emplace(table_meta->GetTableName(), table_id);
    tables_.emplace(table_id, table_info);
    
    return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
/**
 * @brief (内部辅助函数) 从指定的元数据页加载单个索引的信息。
 * 在数据库启动时被构造函数调用。
 * @param index_id 要加载的索引的ID
 * @param page_id 存储该索引元数据的页面ID
 * @return DB_SUCCESS 当操作成功时返回
 */
dberr_t CatalogManager::LoadIndex(const index_id_t index_id, const page_id_t page_id) {
    Page *page = buffer_pool_manager_->FetchPage(page_id);
    if (page == nullptr) {
        return DB_FAILED;
    }
    IndexMetadata *index_meta = nullptr;
    IndexMetadata::DeserializeFrom(page->GetData(), index_meta);
    buffer_pool_manager_->UnpinPage(page_id, false);
    
    TableInfo *table_info;
    if (GetTable(index_meta->GetTableId(), table_info) != DB_SUCCESS) {
        return DB_FAILED;
    }
    
    IndexInfo *index_info = IndexInfo::Create();
    index_info->Init(index_meta, table_info, buffer_pool_manager_);
    
    index_names_[table_info->GetTableName()].emplace(index_meta->GetIndexName(), index_id);
    indexes_.emplace(index_id, index_info);

    return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
/**
 * @brief (内部辅助函数) 通过表ID获取表信息。
 * @param table_id 表的唯一标识符
 * @param table_info 输出参数，用于接收表信息的指针
 * @return DB_SUCCESS 如果找到表，否则返回DB_TABLE_NOT_EXIST
 */
dberr_t CatalogManager::GetTable(const table_id_t table_id, TableInfo *&table_info) {
    auto it = tables_.find(table_id);
    if (it == tables_.end()) {
        return DB_TABLE_NOT_EXIST;
    }
    table_info = it->second;
    return DB_SUCCESS;
}
