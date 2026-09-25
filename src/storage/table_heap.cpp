#include "storage/table_heap.h"

/**
 * TODO: Student Implement
 */
bool TableHeap::InsertTuple(Row &row, Txn *txn) {
  if (schema_ == nullptr || row.GetSerializedSize(schema_) > TablePage::SIZE_MAX_ROW) {
    return false;
  }
  page_id_t cur_id = first_page_id_;
  page_id_t prev_id = INVALID_PAGE_ID;
  while(cur_id != INVALID_PAGE_ID) {
    auto page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(cur_id));
    if(page == nullptr) {
      return false;
    }
    if(page->InsertTuple(row, schema_, txn, lock_manager_, log_manager_)) {
      buffer_pool_manager_->UnpinPage(cur_id, true);
      return true;
    }
    prev_id = cur_id;
    cur_id = page->GetNextPageId();

    buffer_pool_manager_->UnpinPage(prev_id, false);
  }

  page_id_t new_id;
  auto page = reinterpret_cast<TablePage*>(buffer_pool_manager_->NewPage(new_id));
  if (page == nullptr) {
    return false;
  }
  page->Init(new_id, prev_id, log_manager_, txn);
  bool inserted = page->InsertTuple(row, schema_, txn, lock_manager_, log_manager_);

  buffer_pool_manager_->UnpinPage(new_id, inserted);
  if (!inserted) {
    buffer_pool_manager_->DeletePage(new_id);
    return false;
  }

  auto pre_page = reinterpret_cast<TablePage*>(buffer_pool_manager_->FetchPage(prev_id));
  if (pre_page == nullptr) {
    return false;
  }
  pre_page->SetNextPageId(new_id);

  buffer_pool_manager_->UnpinPage(prev_id, true);
  return true;
}

bool TableHeap::MarkDelete(const RowId &rid, Txn *txn) {
  // Find the page which contains the tuple.
  auto page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(rid.GetPageId()));
  // If the page could not be found, then abort the recovery.
  if (page == nullptr) {
    return false;
  }
  // Otherwise, mark the tuple as deleted.
  page->WLatch();
  bool marked = page->MarkDelete(rid, txn, lock_manager_, log_manager_);
  page->WUnlatch();
  buffer_pool_manager_->UnpinPage(page->GetTablePageId(), marked);
  return marked;
}

/**
 * TODO: Student Implement
 */
bool TableHeap::UpdateTuple(Row &row, const RowId &rid, Txn *txn) {
  auto page = reinterpret_cast<TablePage*>(buffer_pool_manager_->FetchPage(rid.GetPageId()));
  if (page == nullptr) {
    return false;
  }
  Row old_row(rid);
  page->WLatch();
  if(page->UpdateTuple(row, &old_row, schema_, txn, lock_manager_, log_manager_)) {
    page->WUnlatch();
    buffer_pool_manager_->UnpinPage(rid.GetPageId(), true);
    return true;
  }
  page->WUnlatch();
  buffer_pool_manager_->UnpinPage(rid.GetPageId(), false);
  return false;
}

/**
 * TODO: Student Implement
 */
void TableHeap::ApplyDelete(const RowId &rid, Txn *txn) {
  // Step1: Find the page which contains the tuple.
  auto page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(rid.GetPageId()));
  // Step2: Delete the tuple from the page.
  if(page == nullptr) {
    return;
  }
  page->WLatch();
  page->ApplyDelete(rid, txn, log_manager_);
  page->WUnlatch();

  buffer_pool_manager_->UnpinPage(rid.GetPageId(), true);
}

void TableHeap::RollbackDelete(const RowId &rid, Txn *txn) {
  // Find the page which contains the tuple.
  auto page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(rid.GetPageId()));
  assert(page != nullptr);
  // Rollback to delete.
  page->WLatch();
  page->RollbackDelete(rid, txn, log_manager_);
  page->WUnlatch();
  buffer_pool_manager_->UnpinPage(page->GetTablePageId(), true);
}

/**
 * TODO: Student Implement
 */
bool TableHeap::GetTuple(Row *row, Txn *txn) {
  if (row == nullptr) {
    return false;
  }
  auto page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(row->GetRowId().GetPageId()));
  if (page == nullptr) {
    return false;
  }
  // 迭代器会复用同一个 Row 对象。反序列化前必须清空旧字段，
  // 否则 Row::DeserializeFrom 会把旧字段误认为是非法的非空目标。
  row->destroy();
  if(page->GetTuple(row, schema_, txn, lock_manager_)) {
    buffer_pool_manager_->UnpinPage(row->GetRowId().GetPageId(), false);
    return true;
  }
  else{
    buffer_pool_manager_->UnpinPage(row->GetRowId().GetPageId(), false);
    return false;
  }
}

void TableHeap::DeleteTable(page_id_t page_id) {
  if (page_id != INVALID_PAGE_ID) {
    auto temp_table_page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(page_id));  // 删除table_heap
    if (temp_table_page->GetNextPageId() != INVALID_PAGE_ID)
      DeleteTable(temp_table_page->GetNextPageId());
    buffer_pool_manager_->UnpinPage(page_id, false);
    buffer_pool_manager_->DeletePage(page_id);
  } else {
    DeleteTable(first_page_id_);
  }
}

/**
 * TODO: Student Implement
 */
TableIterator TableHeap::Begin(Txn *txn) {
  page_id_t cur_page = first_page_id_;

  while(cur_page != INVALID_PAGE_ID){
    auto page = reinterpret_cast<TablePage *>(buffer_pool_manager_->FetchPage(cur_page));
    RowId first_rid;

    page_id_t next_page = page->GetNextPageId();
    if(page->GetFirstTupleRid(&first_rid)){
      Row first_row(first_rid);
      bool found = page->GetTuple(&first_row, schema_, txn, lock_manager_);
      buffer_pool_manager_->UnpinPage(cur_page, false);
      if (found) {
        return TableIterator(this, first_row, txn);
      }
    } else {
      buffer_pool_manager_->UnpinPage(cur_page, false);
    }

    cur_page = next_page;
  }
  return End();
}

/**
 * TODO: Student Implement
 */
TableIterator TableHeap::End() {
  return TableIterator(this, RowId(INVALID_PAGE_ID,0), nullptr);
}
