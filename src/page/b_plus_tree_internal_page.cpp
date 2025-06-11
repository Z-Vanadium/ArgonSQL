#include "page/b_plus_tree_internal_page.h"

#include "index/generic_key.h"

#define pairs_off (data_)
#define pair_size (GetKeySize() + sizeof(page_id_t))
#define key_off 0
#define val_off GetKeySize()

/**
 * TODO: Student Implement
 */
/*****************************************************************************
 * HELPER METHODS AND UTILITIES
 *****************************************************************************/
/*
 * Init method after creating a new internal page
 * Including set page type, set current size, set page id, set parent id and set
 * max page size
 */
void InternalPage::Init(page_id_t page_id, page_id_t parent_id, int key_size, int max_size) {
  SetPageType(IndexPageType::INTERNAL_PAGE);
  SetPageId(page_id);
  SetParentPageId(parent_id);
  SetKeySize(key_size);
  SetMaxSize(max_size);
  SetSize(0);
}
/*
 * Helper method to get/set the key associated with input "index"(a.k.a
 * array offset)
 */
GenericKey *InternalPage::KeyAt(int index) {
  return reinterpret_cast<GenericKey *>(pairs_off + index * pair_size + key_off);
}

void InternalPage::SetKeyAt(int index, GenericKey *key) {
  memcpy(pairs_off + index * pair_size + key_off, key, GetKeySize());
}

page_id_t InternalPage::ValueAt(int index) const {
  return *reinterpret_cast<const page_id_t *>(pairs_off + index * pair_size + val_off);
}

void InternalPage::SetValueAt(int index, page_id_t value) {
  *reinterpret_cast<page_id_t *>(pairs_off + index * pair_size + val_off) = value;
}

int InternalPage::ValueIndex(const page_id_t &value) const {
  for (int i = 0; i < GetSize(); ++i) {
    if (ValueAt(i) == value)
      return i;
  }
  return -1;
}

void *InternalPage::PairPtrAt(int index) {
  return KeyAt(index);
}

void InternalPage::PairCopy(void *dest, void *src, int pair_num) {
  memcpy(dest, src, pair_num * (GetKeySize() + sizeof(page_id_t)));
}
/*****************************************************************************
 * LOOKUP
 *****************************************************************************/
/*
 * Find and return the child pointer(page_id) which points to the child page
 * that contains input "key"
 * Start the search from the second key(the first key should always be invalid)
 * 用了二分查找
 */
page_id_t InternalPage::Lookup(const GenericKey *key, const KeyManager &KM) {
  int l, r, m;
  l = 1;
  r = GetSize() - 1;
  // printf("%d %d\n", l, r);
  while(l <= r){
    m = (l + r) / 2;
    // printf("m = %d\n", m);
    if(KM.CompareKeys(key, KeyAt(m)) > 0){
      l = m + 1;
    }
    else if(KM.CompareKeys(key, KeyAt(m)) < 0){
      r = m - 1;
    }
    else{
      return ValueAt(m);
    }
  }
  return ValueAt(r);
}

/*****************************************************************************
 * INSERTION
 *****************************************************************************/
/*
 * Populate new root page with old_value + new_key & new_value
 * When the insertion cause overflow from leaf page all the way upto the root
 * page, you should create a new root page and populate its elements.
 * NOTE: This method is only called within InsertIntoParent()(b_plus_tree.cpp)
 */
void InternalPage::PopulateNewRoot(const page_id_t &old_value, GenericKey *new_key, const page_id_t &new_value) {
  SetSize(2);
  SetValueAt(0, old_value);
  SetValueAt(1, new_value);
  SetKeyAt(1, new_key);
}

/*
 * Insert new_key & new_value pair right after the pair with its value ==
 * old_value
 * @return:  new size after insertion
 */
int InternalPage::InsertNodeAfter(const page_id_t &old_value, GenericKey *new_key, const page_id_t &new_value) {
  int size = GetSize();
  int old_index = ValueIndex(old_value);
  if (size == GetMaxSize()){
    return size;
  }

  SetSize(size + 1);
  for (int i = GetSize() - 1; i > old_index+1; --i){
      SetValueAt(i, ValueAt(i-1));
      SetKeyAt(i, KeyAt(i - 1));
  }

  SetValueAt(old_index + 1, new_value);
  SetKeyAt(old_index + 1, new_key);

  return size + 1;
}

/*****************************************************************************
 * SPLIT
 *****************************************************************************/
/*
 * Remove half of key & value pairs from this page to "recipient" page
 * buffer_pool_manager 是干嘛的？传给CopyNFrom()用于Fetch数据页
 */
void InternalPage::MoveHalfTo(InternalPage *recipient, BufferPoolManager *buffer_pool_manager) {
  int size = GetSize();
  int move_size = size / 2;
  void *src = PairPtrAt(size - move_size);
  
  recipient->CopyNFrom(src, move_size, buffer_pool_manager);
  
  SetSize(size - move_size);
}

/* Copy entries into me, starting from {items} and copy {size} entries.
 * Since it is an internal page, for all entries (pages) moved, their parents page now changes to me.
 * So I need to 'adopt' them by changing their parent page id, which needs to be persisted with BufferPoolManger
 *
 */
void InternalPage::CopyNFrom(void *src, int size, BufferPoolManager *buffer_pool_manager) {
  PairCopy(PairPtrAt(0), src, size);
  
  SetSize(size);
  
  for (int i = 0; i < size; i++) {
    page_id_t child_page_id = ValueAt(i);
    Page *page = buffer_pool_manager->FetchPage(child_page_id);
    if (page == nullptr) {
      return;
    }
    BPlusTreePage *child_page = reinterpret_cast<BPlusTreePage *>(page->GetData());
    child_page->SetParentPageId(GetPageId());
    buffer_pool_manager->UnpinPage(child_page_id, true);
  }
  return;
}

/*****************************************************************************
 * REMOVE
 *****************************************************************************/
/*
 * Remove the key & value pair in internal page according to input index(a.k.a
 * array offset)
 * NOTE: store key&value pair continuously after deletion
 */
void InternalPage::Remove(int index) {
  int size = GetSize();
  for (int i = index; i < size - 1; ++i){
    SetValueAt(i, ValueAt(i + 1));
    SetKeyAt(i, KeyAt(i + 1));
  }

  SetSize(size - 1);
  return;
}

/*
 * Remove the only key & value pair in internal page and return the value
 * NOTE: only call this method within AdjustRoot()(in b_plus_tree.cpp)
 */
page_id_t InternalPage::RemoveAndReturnOnlyChild() {
  SetSize(0);
  return ValueAt(0);
}

/*****************************************************************************
 * MERGE
 *****************************************************************************/
/*
 * Remove all of key & value pairs from this page to "recipient" page.
 * The middle_key is the separation key you should get from the parent. You need
 * to make sure the middle key is added to the recipient to maintain the invariant.
 * You also need to use BufferPoolManager to persist changes to the parent page id for those
 * pages that are moved to the recipient
 */
void InternalPage::MoveAllTo(InternalPage *recipient, GenericKey *middle_key, BufferPoolManager *buffer_pool_manager) {
  int recp_size = recipient->GetSize();
  recipient->SetKeyAt(0, middle_key);
  
  PairCopy(recipient->PairPtrAt(recp_size), PairPtrAt(0), GetSize());
  for (int i = 0; i < GetSize() + recp_size; ++i) {
      page_id_t child_page_id = recipient->ValueAt(i);
      Page *page = buffer_pool_manager->FetchPage(child_page_id);
      BPlusTreePage *child_page = reinterpret_cast<BPlusTreePage *>(page->GetData());
      child_page->SetParentPageId(recipient->GetPageId());
      buffer_pool_manager->UnpinPage(child_page_id, true);
  }
  recipient->SetSize(recp_size + GetSize());
  SetSize(0);
}

/*****************************************************************************
 * REDISTRIBUTE
 *****************************************************************************/
/*
 * Remove the first key & value pair from this page to tail of "recipient" page.
 *
 * The middle_key is the separation key you should get from the parent. You need
 * to make sure the middle key is added to the recipient to maintain the invariant.
 * You also need to use BufferPoolManager to persist changes to the parent page id for those
 * pages that are moved to the recipient
 */
void InternalPage::MoveFirstToEndOf(InternalPage *recipient, GenericKey *middle_key,
                                    BufferPoolManager *buffer_pool_manager) {
  page_id_t first_value = ValueAt(1);
  CopyLastFrom(middle_key, first_value, buffer_pool_manager);
  Remove(1);  
}

/* Append an entry at the end.
 * Since it is an internal page, the moved entry(page)'s parent needs to be updated.
 * So I need to 'adopt' it by changing its parent page id, which needs to be persisted with BufferPoolManger
 */
void InternalPage::CopyLastFrom(GenericKey *key, const page_id_t value, BufferPoolManager *buffer_pool_manager) {
  int size = GetSize();
  SetKeyAt(size, key);
  SetValueAt(size, value);
  
  SetSize(size + 1);
  Page *page = buffer_pool_manager->FetchPage(value);
  BPlusTreePage *child_page = reinterpret_cast<BPlusTreePage *>(page->GetData());
  child_page->SetParentPageId(GetPageId());
  buffer_pool_manager->UnpinPage(value, true);
}

/*
 * Remove the last key & value pair from this page to head of "recipient" page.
 * You need to handle the original dummy key properly, e.g. updating recipient’s array to position the middle_key at the
 * right place.
 * You also need to use BufferPoolManager to persist changes to the parent page id for those pages that are
 * moved to the recipient
 */
void InternalPage::MoveLastToFrontOf(InternalPage *recipient, GenericKey *middle_key,
                                     BufferPoolManager *buffer_pool_manager) {
  int last_index = GetSize() - 1;
  page_id_t last_value = ValueAt(last_index);
  recipient->CopyFirstFrom(middle_key, last_value, buffer_pool_manager);
  Remove(last_index);
}

/* Append an entry at the beginning.
 * Since it is an internal page, the moved entry(page)'s parent needs to be updated.
 * So I need to 'adopt' it by changing its parent page id, which needs to be persisted with BufferPoolManger
 */
void InternalPage::CopyFirstFrom( GenericKey* middle_key, const page_id_t value, BufferPoolManager *buffer_pool_manager) {
  int size = GetSize();
  for (int i = size - 1; i >= 0; --i) {
      SetValueAt(i + 1, ValueAt(i));
      SetKeyAt(i + 1, KeyAt(i));
  }

  SetValueAt(0, value);
  SetKeyAt(0, middle_key);
  SetSize(size + 1);

  Page *page = buffer_pool_manager->FetchPage(value);
  BPlusTreePage* child_page = reinterpret_cast<BPlusTreePage *>(page->GetData());
  child_page->SetParentPageId(GetPageId());
  buffer_pool_manager->UnpinPage(value, true);
}