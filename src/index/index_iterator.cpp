#include "index/index_iterator.h"

#include "index/basic_comparator.h"
#include "index/generic_key.h"

IndexIterator::IndexIterator() = default;

IndexIterator::IndexIterator(page_id_t page_id, BufferPoolManager *bpm, int index)
    : current_page_id(page_id), item_index(index), buffer_pool_manager(bpm) {
  page = reinterpret_cast<LeafPage *>(buffer_pool_manager->FetchPage(current_page_id)->GetData());
}

IndexIterator::~IndexIterator() {
  if (current_page_id != INVALID_PAGE_ID)
    buffer_pool_manager->UnpinPage(current_page_id, false);
}

/**
 * TODO: Student Implement
 */
std::pair<GenericKey *, RowId> IndexIterator::operator*() {
  // printf("$ %d %d\n", page->GetItem(item_index).second.GetPageId(), page->GetItem(item_index).second.GetSlotNum());
  return page->GetItem(item_index);
}

/**
 * TODO: Student Implement
 */
IndexIterator &IndexIterator::operator++() {
  item_index++;
  if (item_index == page->GetSize()) {
    page_id_t next_page_id = page->GetNextPageId();
    buffer_pool_manager->UnpinPage(current_page_id, false);
    if (next_page_id == INVALID_PAGE_ID) {
      item_index = 0;
      current_page_id = INVALID_PAGE_ID;
      return *this;
    }
    current_page_id = INVALID_PAGE_ID;
    item_index = 0;
    page = reinterpret_cast<LeafPage *>(buffer_pool_manager->FetchPage(current_page_id)->GetData());
    return *this;
  } 
  else {
    return *this;
  }
}

bool IndexIterator::operator==(const IndexIterator &itr) const {
  return (current_page_id == itr.current_page_id) && (item_index == itr.item_index);
}

bool IndexIterator::operator!=(const IndexIterator &itr) const {
  // printf("%d %d %d %d\n", this->current_page_id, this->item_index, itr.current_page_id, itr.item_index);
  return !((*this) == itr);
}