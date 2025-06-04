#include "record/row.h"

/**
 * TODO: Student Implement
 */
uint32_t Row::SerializeTo(char *buf, Schema *schema) const {
  ASSERT(schema != nullptr, "Invalid schema before serialize.");
  ASSERT(schema->GetColumnCount() == fields_.size(), "Fields size do not match schema's column size.");

    uint32_t offset = 0;
    uint32_t field_count = fields_.size();
    std::memcpy(buf + offset, &field_count, sizeof(uint32_t));
    offset += sizeof(uint32_t);

    uint32_t null_bitmap_size = (field_count + 7) / 8; // Calculate size in bytes, round up
    std::vector<uint8_t> null_bitmap(null_bitmap_size, 0); // Initialize to all 0s (not null)

    for (uint32_t i = 0; i < field_count; ++i) {
      if (fields_[i]->IsNull()) {
        // Set the corresponding bit in the bitmap
        null_bitmap[i / 8] |= (1 << (i % 8));
      }
    }
    std::memcpy(buf + offset, null_bitmap.data(), null_bitmap_size);
    offset += null_bitmap_size;

    for (uint32_t i = 0; i < field_count; ++i) {
      if (!fields_[i]->IsNull()) {
        uint32_t field_size = fields_[i]->SerializeTo(buf + offset);;
        // std::memcpy(buf + offset, fields_[i]->GetData(), field_size);
        offset += field_size;
      }
    }
    return offset;
}

uint32_t Row::DeserializeFrom(char *buf, Schema *schema) {
  ASSERT(schema != nullptr, "Invalid schema before deserialize.");
  ASSERT(fields_.empty(), "Non empty field in row.");

  uint32_t offset = 0;
  uint32_t field_count;

  std::memcpy(&field_count, buf + offset, sizeof(uint32_t));
  offset += sizeof(uint32_t);

  uint32_t null_bitmap_size = (field_count + 7) / 8;
  std::vector<uint8_t> null_bitmap(null_bitmap_size);
  std::memcpy(null_bitmap.data(), buf + offset, null_bitmap_size);
  offset += null_bitmap_size;

  fields_.reserve(field_count);
  for (uint32_t i = 0; i < field_count; ++i) {
    bool is_null = (null_bitmap[i / 8] >> (i % 8)) & 1;
    TypeId field_type = schema->GetColumn(i)->GetType();

    Field *field = nullptr;
    uint32_t sz = 0;
    sz = Field::DeserializeFrom(buf + offset, field_type, &field, is_null);
    offset += sz;    
    fields_.push_back(field);
  }
  return offset;
}

uint32_t Row::GetSerializedSize(Schema *schema) const {
  ASSERT(schema != nullptr, "Invalid schema before serialize.");
  ASSERT(schema->GetColumnCount() == fields_.size(), "Fields size do not match schema's column size.");

  uint32_t total_size = 0;
  uint32_t field_count = fields_.size();

  total_size += sizeof(uint32_t);

  total_size += (field_count + 7) / 8;

  for (uint32_t i = 0; i < field_count; ++i) {
    if (!fields_[i]->IsNull()) {
      if (fields_[i]->GetTypeId() == TypeId::kTypeChar) {
        total_size += sizeof(uint32_t);
      }
      total_size += fields_[i]->GetSerializedSize();
    }
  }
  return total_size;
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
