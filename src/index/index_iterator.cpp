#include "index/index_iterator.h"

#include "index/basic_comparator.h"
#include "index/generic_key.h"

IndexIterator::IndexIterator() = default;

IndexIterator::IndexIterator(page_id_t page_id, BufferPoolManager *bpm, int index)
    : current_page_id(page_id), item_index(index), buffer_pool_manager(bpm) {
  if (current_page_id != INVALID_PAGE_ID && buffer_pool_manager != nullptr) {
    Page *raw_page = buffer_pool_manager->FetchPage(current_page_id);
    page = raw_page == nullptr ? nullptr : reinterpret_cast<LeafPage *>(raw_page->GetData());
  }
}

IndexIterator::IndexIterator(const IndexIterator &other)
    : current_page_id(other.current_page_id), page(nullptr), item_index(other.item_index),
      buffer_pool_manager(other.buffer_pool_manager) {
  if (current_page_id != INVALID_PAGE_ID && buffer_pool_manager != nullptr) {
    Page *raw_page = buffer_pool_manager->FetchPage(current_page_id);
    page = raw_page == nullptr ? nullptr : reinterpret_cast<LeafPage *>(raw_page->GetData());
    if (page == nullptr) {
      current_page_id = INVALID_PAGE_ID;
      item_index = 0;
    }
  }
}

IndexIterator::IndexIterator(IndexIterator &&other) noexcept
    : current_page_id(other.current_page_id), page(other.page), item_index(other.item_index),
      buffer_pool_manager(other.buffer_pool_manager) {
  other.current_page_id = INVALID_PAGE_ID;
  other.page = nullptr;
  other.item_index = 0;
  other.buffer_pool_manager = nullptr;
}

IndexIterator &IndexIterator::operator=(const IndexIterator &other) {
  if (this == &other) {
    return *this;
  }
  if (current_page_id != INVALID_PAGE_ID && buffer_pool_manager != nullptr) {
    buffer_pool_manager->UnpinPage(current_page_id, false);
  }
  current_page_id = other.current_page_id;
  page = nullptr;
  item_index = other.item_index;
  buffer_pool_manager = other.buffer_pool_manager;
  if (current_page_id != INVALID_PAGE_ID && buffer_pool_manager != nullptr) {
    Page *raw_page = buffer_pool_manager->FetchPage(current_page_id);
    page = raw_page == nullptr ? nullptr : reinterpret_cast<LeafPage *>(raw_page->GetData());
    if (page == nullptr) {
      current_page_id = INVALID_PAGE_ID;
      item_index = 0;
    }
  }
  return *this;
}

IndexIterator &IndexIterator::operator=(IndexIterator &&other) noexcept {
  if (this == &other) {
    return *this;
  }
  if (current_page_id != INVALID_PAGE_ID && buffer_pool_manager != nullptr) {
    buffer_pool_manager->UnpinPage(current_page_id, false);
  }
  current_page_id = other.current_page_id;
  page = other.page;
  item_index = other.item_index;
  buffer_pool_manager = other.buffer_pool_manager;
  other.current_page_id = INVALID_PAGE_ID;
  other.page = nullptr;
  other.item_index = 0;
  other.buffer_pool_manager = nullptr;
  return *this;
}

IndexIterator::~IndexIterator() {
  if (current_page_id != INVALID_PAGE_ID)
    buffer_pool_manager->UnpinPage(current_page_id, false);
}

std::pair<GenericKey *, RowId> IndexIterator::operator*() {
  ASSERT(page != nullptr && item_index >= 0 && item_index < page->GetSize(), "Dereference invalid index iterator.");
  return page->GetItem(item_index);
}

IndexIterator &IndexIterator::operator++() {
  if (page == nullptr || current_page_id == INVALID_PAGE_ID) {
    return *this;
  }

  ++item_index;
  if (item_index < page->GetSize()) {
    return *this;
  }

  page_id_t next_page_id = page->GetNextPageId();
  buffer_pool_manager->UnpinPage(current_page_id, false);
  if (next_page_id == INVALID_PAGE_ID) {
    current_page_id = INVALID_PAGE_ID;
    item_index = 0;
    page = nullptr;
    return *this;
  }

  Page *new_raw_page = buffer_pool_manager->FetchPage(next_page_id);
  if (new_raw_page == nullptr) {
    current_page_id = INVALID_PAGE_ID;
    item_index = 0;
    page = nullptr;
    return *this;
  }
  current_page_id = next_page_id;
  item_index = 0;
  page = reinterpret_cast<LeafPage *>(new_raw_page->GetData());
  return *this;
}

bool IndexIterator::operator==(const IndexIterator &itr) const {
  return (current_page_id == itr.current_page_id) && (item_index == itr.item_index);
}

bool IndexIterator::operator!=(const IndexIterator &itr) const {
  // printf("%d %d %d %d\n", this->current_page_id, this->item_index, itr.current_page_id, itr.item_index);
  return !((*this) == itr);
}