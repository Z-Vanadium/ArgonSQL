#include "storage/table_iterator.h"

#include "common/macros.h"
#include "storage/table_heap.h"

/**
 * TODO: Student Implement
 */
TableIterator::TableIterator(TableHeap *table_heap, Row row, Txn *txn):table_heap_(table_heap), row(row), txn_(txn){}

TableIterator::TableIterator(const TableIterator &other) = default;

TableIterator::~TableIterator() = default;

bool TableIterator::operator==(const TableIterator &itr) const {
  return this->row.GetRowId() == itr.row.GetRowId();
}

bool TableIterator::operator!=(const TableIterator &itr) const {
  return !(*this == itr);
}

const Row &TableIterator::operator*() {
  return row;
}

Row *TableIterator::operator->() {
  return &row;
}

TableIterator &TableIterator::operator=(const TableIterator &itr) noexcept {
  if(itr == *this){
    return *this;
  }
  table_heap_ = itr.table_heap_;
  row = itr.row;
  txn_ = itr.txn_;
  return *this;
}

// ++iter
TableIterator &TableIterator::operator++() {
  RowId rid = row.GetRowId();
  page_id_t page_id = rid.GetPageId();
  RowId next_rid;
  auto page = reinterpret_cast<TablePage*>(table_heap_->buffer_pool_manager_->FetchPage(page_id));

  if(page == nullptr){
    throw runtime_error("Page not exist");
    return *this;
  }
  
  else if(page->GetNextTupleRid(rid, &next_rid)){
    row.SetRowId(next_rid);
    table_heap_->GetTuple(&row, txn_);
    table_heap_->buffer_pool_manager_->UnpinPage(page_id, false);
    return *this;
  }

  auto pre_id = page_id;
  page_id = page->GetNextPageId();
  table_heap_->buffer_pool_manager_->UnpinPage(pre_id, false);

  while(page_id != INVALID_PAGE_ID) {
    page = reinterpret_cast<TablePage*>(table_heap_->buffer_pool_manager_->FetchPage(page_id));

    if(page == nullptr) {
      throw runtime_error("Page not exist");
      return *this;
    }

    if(page->GetFirstTupleRid(&next_rid)) {
      row.SetRowId(next_rid);
      table_heap_->GetTuple(&row, txn_);
      table_heap_->buffer_pool_manager_->UnpinPage(page_id,false);
      return *this;
    }
    pre_id = page_id;
    page_id = page->GetNextPageId();
    table_heap_->buffer_pool_manager_->UnpinPage(pre_id, false);
  }
  
  row.SetRowId(RowId(INVALID_PAGE_ID, 0));
  return *this;
}

// iter++
TableIterator TableIterator::operator++(int) {
  const TableIterator t(*this);
  ++(*this);
  return t;
}
