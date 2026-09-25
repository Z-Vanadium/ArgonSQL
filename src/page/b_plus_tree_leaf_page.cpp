#include "page/b_plus_tree_leaf_page.h"

#include <algorithm>

#include "index/generic_key.h"

#define pairs_off (data_)
#define pair_size (GetKeySize() + sizeof(RowId))
#define key_off 0
#define val_off GetKeySize()
/*****************************************************************************
 * HELPER METHODS AND UTILITIES
 *****************************************************************************/

/**
 * TODO: Student Implement
 */
/**
 * Init method after creating a new leaf page
 * Including set page type, set current size to zero, set page id/parent id, set
 * next page id and set max size
 * 未初始化next_page_id
 */
void LeafPage::Init(page_id_t page_id, page_id_t parent_id, int key_size, int max_size) {
  SetPageType(IndexPageType::LEAF_PAGE);
  SetSize(0);
  SetPageId(page_id);
  SetParentPageId(parent_id);
  SetMaxSize(max_size);
  SetKeySize(key_size);
  SetNextPageId(INVALID_PAGE_ID);
  // memset(data_, 0, PAGE_SIZE); 
}

/**
 * Helper methods to set/get next page id
 */
page_id_t LeafPage::GetNextPageId() const {
  return next_page_id_;
}

void LeafPage::SetNextPageId(page_id_t next_page_id) {
  next_page_id_ = next_page_id;
  if (next_page_id == 0) {
    LOG(INFO) << "Fatal error";
  }
}

/**
 * TODO: Student Implement
 */
/**
 * Helper method to find the first index i so that pairs_[i].first >= key
 * NOTE: This method is only used when generating index iterator
 * 二分查找
 */
int LeafPage::KeyIndex(const GenericKey *key, const KeyManager &KM) {
  // 返回第一个大于等于 key 的位置，才能正确支持范围扫描的 Begin(key)。
  int left = 0;
  int right = GetSize();
  while (left < right) {
    int middle = left + (right - left) / 2;
    if (KM.CompareKeys(KeyAt(middle), key) < 0) {
      left = middle + 1;
    } else {
      right = middle;
    }
  }
  return left;
}

/*
 * Helper method to find and return the key associated with input "index"(a.k.a
 * array offset)
 */
GenericKey *LeafPage::KeyAt(int index) {
  return reinterpret_cast<GenericKey *>(pairs_off + index * pair_size + key_off);
}

void LeafPage::SetKeyAt(int index, GenericKey *key) {
  memcpy(pairs_off + index * pair_size + key_off, key, GetKeySize());
}

RowId LeafPage::ValueAt(int index) const {
  return *reinterpret_cast<const RowId *>(pairs_off + index * pair_size + val_off);
}

void LeafPage::SetValueAt(int index, RowId value) {
  *reinterpret_cast<RowId *>(pairs_off + index * pair_size + val_off) = value;
}

void *LeafPage::PairPtrAt(int index) {
  return KeyAt(index);
}

void LeafPage::PairCopy(void *dest, void *src, int pair_num) {
  memcpy(dest, src, pair_num * (GetKeySize() + sizeof(RowId)));
}
/*
 * Helper method to find and return the key & value pair associated with input
 * "index"(a.k.a. array offset)
 */
std::pair<GenericKey *, RowId> LeafPage::GetItem(int index) { return {KeyAt(index), ValueAt(index)}; }

/*****************************************************************************
 * INSERTION
 *****************************************************************************/
/*
 * Insert key & value pair into leaf page ordered by key
 * @return page size after insertion
 */
int LeafPage::Insert(GenericKey *key, const RowId &value, const KeyManager &KM) {
  // printf("LeafPage::Insert: 接收到键，对应值 RowId(%u, %u)\n", value.GetPageId(), value.GetSlotNum());
  //   printf("LeafPage::Insert: 页面 %u 插入qian内容 (大小 %d):\n", GetPageId(), GetSize());
    // for (int dbg_idx = 0; dbg_idx < GetSize(); ++dbg_idx) {
    //     printf("  [%d]: 键, 值 (%u, %u)\n", dbg_idx, ValueAt(dbg_idx).GetPageId(), ValueAt(dbg_idx).GetSlotNum());
    // }
    int current_size = GetSize();
    int actual_insert_idx = 0;
    while (actual_insert_idx < current_size && KM.CompareKeys(KeyAt(actual_insert_idx), key) < 0) {
        actual_insert_idx++;
    }
    for (int i = current_size; i > actual_insert_idx; --i) {
        SetKeyAt(i, KeyAt(i - 1));
        SetValueAt(i, ValueAt(i - 1));
    }
    SetKeyAt(actual_insert_idx, key);
    SetValueAt(actual_insert_idx, value);
    
    IncreaseSize(1);
    // printf("LeafPage::Insert: 页面 %u 插入后内容 (大小 %d):\n", GetPageId(), GetSize());
    // for (int dbg_idx = 0; dbg_idx < GetSize(); ++dbg_idx) {
    //     printf("  [%d]: 键, 值 (%u, %u)\n", dbg_idx, ValueAt(dbg_idx).GetPageId(), ValueAt(dbg_idx).GetSlotNum());
    // }
    // printf("---------------------------\n");
    return GetSize();
}

/*****************************************************************************
 * SPLIT
 *****************************************************************************/
/*
 * Remove half of key & value pairs from this page to "recipient" page
 */
void LeafPage::MoveHalfTo(LeafPage *recipient) {
  int size = GetSize();
  int half = size / 2;
  recipient->CopyNFrom(PairPtrAt(half), size - half);
  SetSize(half);
}

/*
 * Copy starting from items, and copy {size} number of elements into me.
 */
void LeafPage::CopyNFrom(void *src, int size) {
  PairCopy(PairPtrAt(0), src, size);
  SetSize(size);
}

/*****************************************************************************
 * LOOKUP
 *****************************************************************************/
/*
 * For the given key, check to see whether it exists in the leaf page. If it
 * does, then store its corresponding value in input "value" and return true.
 * If the key does not exist, then return false
 */
bool LeafPage::Lookup(const GenericKey *key, RowId &value, const KeyManager &KM) {
  int l, r, m;
  l = 0; 
  r = GetSize() - 1; 
  while (l <= r) {
    m = (l + r) /2;
    if (KM.CompareKeys(KeyAt(m), key) < 0) {
        l = m + 1;
    } else if (KM.CompareKeys(KeyAt(m), key) > 0){  
        r = m - 1;
    } else {
      value = ValueAt(m);
      return true;
    }
  }
  return false;
}

/*****************************************************************************
 * REMOVE
 *****************************************************************************/
/*
 * First look through leaf page to see whether delete key exist or not. If
 * existed, perform deletion, otherwise return immediately.
 * NOTE: store key&value pair continuously after deletion
 * @return  page size after deletion
 */
int LeafPage::RemoveAndDeleteRecord(const GenericKey *key, const KeyManager &KM) {
  int size = GetSize();
  RowId value;
  if (Lookup(key, value, KM)){
    int index = KeyIndex(key, KM);
    for (int i = index; i < size - 1; ++i) {
      SetKeyAt(i, KeyAt(i + 1));
      SetValueAt(i, ValueAt(i + 1));
    }
    IncreaseSize(-1);
  }
  return size;
}

/*****************************************************************************
 * MERGE
 *****************************************************************************/
/*
 * Remove all key & value pairs from this page to "recipient" page. Don't forget
 * to update the next_page id in the sibling page
 */
void LeafPage::MoveAllTo(LeafPage *recipient) {
  int size = GetSize();
  int recipient_size = recipient->GetSize();
  recipient->PairCopy(recipient->PairPtrAt(recipient_size), PairPtrAt(0), size);
  recipient->SetSize(recipient_size + size);
  recipient->SetNextPageId(GetNextPageId());
  SetSize(0);
}

/*****************************************************************************
 * REDISTRIBUTE
 *****************************************************************************/
/*
 * Remove the first key & value pair from this page to "recipient" page.
 *
 */
void LeafPage::MoveFirstToEndOf(LeafPage *recipient) {
  int size = GetSize();
  int recipient_size = recipient->GetSize();
  recipient->CopyLastFrom(KeyAt(0), ValueAt(0));
  PairCopy(PairPtrAt(0), PairPtrAt(1), size - 1);
  SetSize(size - 1);
}

/*
 * Copy the item into the end of my item list. (Append item to my array)
 */
void LeafPage::CopyLastFrom(GenericKey *key, const RowId value) {
  int size = GetSize();
  SetKeyAt(size, key);
  SetValueAt(size, value);
  SetSize(size + 1);
}

/*
 * Remove the last key & value pair from this page to "recipient" page.
 */
void LeafPage::MoveLastToFrontOf(LeafPage *recipient) {
  int size = GetSize();
  int recipient_size = recipient->GetSize();
  recipient->CopyFirstFrom(KeyAt(size - 1), ValueAt(size - 1));
  SetSize(size - 1);
}

/*
 * Insert item at the front of my items. Move items accordingly.
 *
 */
void LeafPage::CopyFirstFrom(GenericKey *key, const RowId value) {
  int size = GetSize();
  for (int i = size - 1; i >= 0; --i) {
    SetKeyAt(i + 1, KeyAt(i));
    SetValueAt(i + 1, ValueAt(i));
  }
  SetSize(size + 1);
  SetKeyAt(0, key);
  SetValueAt(0, value);
}
