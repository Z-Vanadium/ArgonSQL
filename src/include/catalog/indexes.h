#ifndef MINISQL_INDEXES_H
#define MINISQL_INDEXES_H

#include <memory>

#include "catalog/table.h"
#include "common/macros.h"
#include "common/rowid.h"
#include "index/b_plus_tree_index.h"
#include "index/generic_key.h"
#include "record/schema.h"

class IndexMetadata {
  friend class IndexInfo;

 public:
  static IndexMetadata *Create(const index_id_t index_id, const std::string &index_name, const table_id_t table_id,
                               const std::vector<uint32_t> &key_map);

  uint32_t SerializeTo(char *buf) const;

  uint32_t GetSerializedSize() const;

  static uint32_t DeserializeFrom(char *buf, IndexMetadata *&index_meta);

  inline std::string GetIndexName() const { return index_name_; }

  inline table_id_t GetTableId() const { return table_id_; }

  uint32_t GetIndexColumnCount() const { return key_map_.size(); }

  inline const std::vector<uint32_t> &GetKeyMapping() const { return key_map_; }

  inline index_id_t GetIndexId() const { return index_id_; }

 private:
  IndexMetadata() = delete;

  explicit IndexMetadata(const index_id_t index_id, const std::string &index_name, const table_id_t table_id,
                         const std::vector<uint32_t> &key_map);

 private:
  static constexpr uint32_t INDEX_METADATA_MAGIC_NUM = 344528;
  index_id_t index_id_;
  std::string index_name_;
  table_id_t table_id_;
  std::vector<uint32_t> key_map_; /** The mapping of index key to tuple key */
};

/**
 * The IndexInfo class maintains metadata about a index.
 */
class IndexInfo {
 public:
  static IndexInfo *Create() { return new IndexInfo(); }
  inline IndexMetadata *GetMetaData() { return meta_data_; }
  ~IndexInfo() {
    delete meta_data_;
    delete index_;
    delete key_schema_;
  }

/**
 * TODO: Student Implement
 */
  void Init(IndexMetadata *meta_data, TableInfo *table_info, BufferPoolManager *buffer_pool_manager) {
    // 步骤1：初始化索引元数据
    meta_data_ = meta_data;

    // 步骤2：将索引键映射到键模式。
    // 这包括根据索引元数据中定义的键映射，从表的模式中选择列来创建键模式。
    // 注意：这里不再需要手动构建 key_columns 向量，
    // 因为 Schema::ShallowCopySchema 函数会根据 key_map 自己去 table_schema 中取列。
    // 所以，删除之前创建 key_columns 向量和循环的代码。

    // 直接调用 Schema::ShallowCopySchema，传入表模式和索引键映射
    key_schema_ = Schema::ShallowCopySchema(table_info->GetSchema(), meta_data_->GetKeyMapping()); //

    // 步骤3：调用 CreateIndex 创建索引。
    // 根据 CreateIndex 的实现，假定索引类型为 "bptree"。
    index_ = CreateIndex(buffer_pool_manager, "bptree");
  }

  inline Index *GetIndex() { return index_; }

  std::string GetIndexName() { return meta_data_->GetIndexName(); }

  IndexSchema *GetIndexKeySchema() { return key_schema_; }

 private:
  explicit IndexInfo() : meta_data_{nullptr}, index_{nullptr}, key_schema_{nullptr} {}

  Index *CreateIndex(BufferPoolManager *buffer_pool_manager, const string &index_type);

 private:
  IndexMetadata *meta_data_;
  Index *index_;
  IndexSchema *key_schema_;
};

#endif  // MINISQL_INDEXES_H
