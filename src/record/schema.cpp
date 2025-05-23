#include "record/schema.h"

/**
 * TODO: Student Implement
 */
uint32_t Schema::SerializeTo(char *buf) const {
  uint32_t offset = 0;

  uint32_t num_columns = columns_.size();
  std::memcpy(buf + offset, &num_columns, sizeof(uint32_t));
  offset += sizeof(uint32_t);

  for (const auto &col : columns_) {
    offset += col->SerializeTo(buf + offset);
  }

  return offset;
}

uint32_t Schema::GetSerializedSize() const {
  uint32_t total_size = 0;

  total_size += sizeof(uint32_t);

  for (const auto &col : columns_) {
    total_size += col->GetSerializedSize();
  }

  return total_size;
}

uint32_t Schema::DeserializeFrom(char *buf, Schema *&schema) {
  uint32_t offset = 0;

  uint32_t num_columns;
  std::memcpy(&num_columns, buf + offset, sizeof(uint32_t));
  offset += sizeof(uint32_t);

  std::vector<Column *> columns;
  for (uint32_t i = 0; i < num_columns; ++i) {
    Column *col = nullptr;
    offset += Column::DeserializeFrom(buf + offset, col);
    columns.push_back(col);
  }

  schema = new Schema(columns);

  return offset;
}