#include "record/column.h"

#include "glog/logging.h"

Column::Column(std::string column_name, TypeId type, uint32_t index, bool nullable, bool unique)
    : name_(std::move(column_name)), type_(type), table_ind_(index), nullable_(nullable), unique_(unique) {
  ASSERT(type != TypeId::kTypeChar, "Wrong constructor for CHAR type.");
  switch (type) {
    case TypeId::kTypeInt:
      len_ = sizeof(int32_t);
      break;
    case TypeId::kTypeFloat:
      len_ = sizeof(float_t);
      break;
    default:
      ASSERT(false, "Unsupported column type.");
  }
}

Column::Column(std::string column_name, TypeId type, uint32_t length, uint32_t index, bool nullable, bool unique)
    : name_(std::move(column_name)),
      type_(type),
      len_(length),
      table_ind_(index),
      nullable_(nullable),
      unique_(unique) {
  ASSERT(type == TypeId::kTypeChar, "Wrong constructor for non-VARCHAR type.");
}

Column::Column(const Column *other)
    : name_(other->name_),
      type_(other->type_),
      len_(other->len_),
      table_ind_(other->table_ind_),
      nullable_(other->nullable_),
      unique_(other->unique_) {}

/**
* TODO: Student Implement
*/
uint32_t Column::SerializeTo(char *buf) const {
  uint32_t name_length = name_.length;

  uint32_t offset = 0;

  // name
  std::memcpy(buf + offset, &name_length, sizeof(uint32_t));
  offset += sizeof(uint32_t);

  // name_ content
  std::memcpy(buf + offset, name_.c_str(), name_length);
  offset += name_length;

  // type_
  uint32_t type_value = static_cast<uint32_t>(type_);
  std::memcpy(buf + offset, &type_value, sizeof(uint32_t));
  offset += sizeof(uint32_t);

  // len_
  std::memcpy(buf + offset, &len_, sizeof(uint32_t));
  offset += sizeof(uint32_t);

  // table_ind_ 
  std::memcpy(buf + offset, &table_ind_, sizeof(uint32_t));
  offset += sizeof(uint32_t);

  // nullable_ 
  uint8_t nullable_value = nullable_ ? 1 : 0;
  std::memcpy(buf + offset, &nullable_value, sizeof(uint8_t));
  offset += sizeof(uint8_t);

  // unique_
  uint8_t unique_value = unique_ ? 1 : 0;
  std::memcpy(buf + offset, &unique_value, sizeof(uint8_t));
  offset += sizeof(uint8_t);

  return offset;
}

/**
 * TODO: Student Implement
 */
uint32_t Column::GetSerializedSize() const {
  uint32_t size = 0;

  size += sizeof(uint32_t);         // name_
  size += name_.length();           // name_ content
  size += sizeof(uint32_t);         // type_
  size += sizeof(uint32_t);         // len_
  size += sizeof(uint32_t);         // table_ind_
  size += sizeof(uint8_t);          // nullable_
  size += sizeof(uint8_t);          // unique_
  return size;

}

/**
 * TODO: Student Implement
 */
uint32_t Column::DeserializeFrom(char *buf, Column *&column) {
  uint32_t offset = 0;

  // name_
  uint32_t name_length;
  std::memcpy(&name_length, buf + offset, sizeof(uint32_t));
  offset += sizeof(uint32_t);

  // name_ content
  char* name_buf = new char[name_length + 1]; // +1 for '\0'
  std::memcpy(name_buf, buf + offset, name_length);
  name_buf[name_length] = '\0';
  std::string name(name_buf);
  offset += name_length;
  delete[] name_buf;

  // type_
  uint32_t type_value;
  std::memcpy(&type_value, buf + offset, sizeof(uint32_t));
  TypeId type = static_cast<TypeId>(type_value);
  offset += sizeof(uint32_t);

  // len_
  uint32_t len;
  std::memcpy(&len, buf + offset, sizeof(uint32_t));
  offset += sizeof(uint32_t);

  // table_ind_
  uint32_t table_ind;
  std::memcpy(&table_ind, buf + offset, sizeof(uint32_t));
  offset += sizeof(uint32_t);

  // nullable_
  uint8_t nullable_value;
  std::memcpy(&nullable_value, buf + offset, sizeof(uint8_t));
  bool nullable = (nullable_value != 0);
  offset += sizeof(uint8_t);

  // unique_
  uint8_t unique_value;
  std::memcpy(&unique_value, buf + offset, sizeof(uint8_t));
  bool unique = (unique_value != 0);
  offset += sizeof(uint8_t);

  if (type == TypeId::kTypeChar) {
    column = new Column(name, type, len, table_ind, nullable, unique);
  } else {
    column = new Column(name, type, table_ind, nullable, unique);
  }

  return offset;
}
