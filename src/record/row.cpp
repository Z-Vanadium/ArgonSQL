#include "record/row.h"

/**
 * TODO: Student Implement
 */
uint32_t Row::SerializeTo(char *buf, Schema *schema) const {
  ASSERT(schema != nullptr, "Invalid schema before serialize.");
  ASSERT(schema->GetColumnCount() == fields_.size(), "Fields size do not match schema's column size.");

    uint32_t offset = 0;
    uint32_t field_count = fields_.size();

    // 1. Write the number of fields (uint32_t)
    // 1. 写入字段数量 (uint32_t)
    std::memcpy(buf + offset, &field_count, sizeof(uint32_t));
    offset += sizeof(uint32_t);

    // 2. Write the null bitmap.  Each field is represented by one bit.
    // 2. 写入空值位图。每个字段用一个位表示。
    uint32_t null_bitmap_size = (field_count + 7) / 8; // Calculate size in bytes, round up
    // 计算字节大小，向上取整
    std::vector<uint8_t> null_bitmap(null_bitmap_size, 0); // Initialize to all 0s (not null)
    // 初始化为全0 (非空)

    for (uint32_t i = 0; i < field_count; ++i) {
      if (fields_[i]->IsNull()) {
        // Set the corresponding bit in the bitmap
        // 设置位图中对应的位
        null_bitmap[i / 8] |= (1 << (i % 8));
      }
    }
    std::memcpy(buf + offset, null_bitmap.data(), null_bitmap_size);
    offset += null_bitmap_size;

    // 3. Write the fields
    // 3. 写入字段数据
    for (uint32_t i = 0; i < field_count; ++i) {
      if (!fields_[i]->IsNull()) {
        uint32_t field_size = fields_[i]->GetLength();
        std::memcpy(buf + offset, fields_[i]->GetData(), field_size);
        offset += field_size;
      }
      // If the field is null, we don't write any data for it, but the bitmap indicates it is null.
      // 如果字段为空，我们不写入任何数据，但位图会指示它为空。
    }
    return offset;
}

uint32_t Row::DeserializeFrom(char *buf, Schema *schema) {
  ASSERT(schema != nullptr, "Invalid schema before serialize.");
  ASSERT(fields_.empty(), "Non empty field in row.");
  // replace with your code here
  return 0;
}

uint32_t Row::GetSerializedSize(Schema *schema) const {
  ASSERT(schema != nullptr, "Invalid schema before serialize.");
  ASSERT(schema->GetColumnCount() == fields_.size(), "Fields size do not match schema's column size.");
  // replace with your code here
  return 0;
}

void Row::GetKeyFromRow(const Schema *schema, const Schema *key_schema, Row &key_row) {
  auto columns = key_schema->GetColumns();
  std::vector<Field> fields;
  uint32_t idx;
  for (auto column : columns) {
    schema->GetColumnIndex(column->GetName(), idx);
    fields.emplace_back(*this->GetField(idx));
  }
  key_row = Row(fields);
}
