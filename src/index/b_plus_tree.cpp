#include "index/b_plus_tree.h"

#include <string>

#include "glog/logging.h"
#include "index/basic_comparator.h"
#include "index/generic_key.h"
#include "page/index_roots_page.h"

BPlusTree::BPlusTree(index_id_t index_id, BufferPoolManager *buffer_pool_manager, const KeyManager &KM,
                     int leaf_max_size, int internal_max_size)
    : index_id_(index_id),
      buffer_pool_manager_(buffer_pool_manager),
      processor_(KM),
      leaf_max_size_(leaf_max_size),
      internal_max_size_(internal_max_size) {
  if (leaf_max_size_ == UNDEFINED_SIZE || internal_max_size_ == UNDEFINED_SIZE) {
    leaf_max_size_ = (PAGE_SIZE - LEAF_PAGE_HEADER_SIZE) / (KM.GetKeySize() + sizeof(RowId));
  }
  if (internal_max_size_ == UNDEFINED_SIZE) {
    internal_max_size_ = (PAGE_SIZE - INTERNAL_PAGE_HEADER_SIZE) / (KM.GetKeySize() + sizeof(page_id_t));
  }
  IndexRootsPage* page = reinterpret_cast<IndexRootsPage *>(buffer_pool_manager_->FetchPage(INDEX_ROOTS_PAGE_ID)->GetData());

  if (page->GetRootId(index_id_, &root_page_id_) == false) {
    root_page_id_ = INVALID_PAGE_ID;
  }
  buffer_pool_manager_->UnpinPage(INDEX_ROOTS_PAGE_ID, false);
}

void BPlusTree::Destroy(page_id_t current_page_id) {
  const bool destroying_root = current_page_id == INVALID_PAGE_ID;
  if (destroying_root) {
    current_page_id = root_page_id_;
  }
  if (current_page_id == INVALID_PAGE_ID) {
    return;
  }

  Page *raw_page = buffer_pool_manager_->FetchPage(current_page_id);
  if (raw_page == nullptr) {
    return;
  }
  auto *node = reinterpret_cast<BPlusTreePage *>(raw_page->GetData());
  std::vector<page_id_t> children;
  if (!node->IsLeafPage()) {
    auto *internal = reinterpret_cast<InternalPage *>(raw_page->GetData());
    for (int i = 0; i < internal->GetSize(); ++i) {
      children.push_back(internal->ValueAt(i));
    }
  }
  buffer_pool_manager_->UnpinPage(current_page_id, false);
  for (page_id_t child : children) {
    Destroy(child);
  }
  buffer_pool_manager_->DeletePage(current_page_id);

  if (destroying_root) {
    root_page_id_ = INVALID_PAGE_ID;
    UpdateRootPageId(0);
  }
}

/*
 * Helper function to decide whether current b+tree is empty
 */
bool BPlusTree::IsEmpty() const {
  return root_page_id_ == INVALID_PAGE_ID;
}

/*****************************************************************************
 * SEARCH
 *****************************************************************************/
/*
 * Return the only value that associated with input key
 * This method is used for point query
 * @return : true means key exists
 */
bool BPlusTree::GetValue(const GenericKey *key, std::vector<RowId> &result, Txn *transaction) {
  if (IsEmpty()){
    return false;
  }

  Page *page = FindLeafPage(key, root_page_id_);
  if (page == nullptr) {
      return false;
  }

  BPlusTreeLeafPage *leaf_page = reinterpret_cast<BPlusTreeLeafPage *>(page->GetData());
  RowId value;
  bool found = leaf_page->Lookup(key, value, processor_);
  if (found) {
      result.push_back(value);
  }

  buffer_pool_manager_->UnpinPage(leaf_page->GetPageId(), false);
  return found;
}

/*****************************************************************************
 * INSERTION
 *****************************************************************************/
/*
 * Insert constant key & value pair into b+ tree
 * if current tree is empty, start new tree, update root page id and insert
 * entry, otherwise insert into leaf page.
 * @return: since we only support unique key, if user try to insert duplicate
 * keys return false, otherwise return true.
 */
bool BPlusTree::Insert(GenericKey *key, const RowId &value, Txn *transaction) {
// printf("root_page_id_ = %d\n", root_page_id_);
    if (IsEmpty()) {
        StartNewTree(key, value);
        return true;
    }
    return InsertIntoLeaf(key, value, transaction);
// printf("root_page_id_ = %d\n", root_page_id_);
}
/*
 * Insert constant key & value pair into an empty tree
 * User needs to first ask for new page from buffer pool manager(NOTICE: throw
 * an "out of memory" exception if returned value is nullptr), then update b+
 * tree's root page id and insert entry directly into leaf page.
 */
void BPlusTree::StartNewTree(GenericKey *key, const RowId &value) {
    //printf("root_page_id_ = %d\n", root_page_id_);
    Page *new_page = buffer_pool_manager_->NewPage(root_page_id_);
    if (new_page == nullptr) {
        throw std::runtime_error("Out of memory: Cannot allocate new page for B+ tree root.");
    }

    BPlusTreeLeafPage *root_leaf_page = reinterpret_cast<BPlusTreeLeafPage *>(new_page->GetData());

    UpdateRootPageId(true);
    root_leaf_page->Init(root_page_id_, INVALID_PAGE_ID, processor_.GetKeySize(), leaf_max_size_);
    // root_leaf_page->SetNextPageId(INVALID_PAGE_ID);
    bool inserted = root_leaf_page->Insert(key, value, processor_);
    // ASSERT(inserted, "First insert into new tree failed: key already exists.");

    buffer_pool_manager_->UnpinPage(root_page_id_, true);
}

/*
 * Insert constant key & value pair into leaf page
 * User needs to first find the right leaf page as insertion target, then look
 * through leaf page to see whether insert key exist or not. If exist, return
 * immediately, otherwise insert entry. Remember to deal with split if necessary.
 * @return: since we only support unique key, if user try to insert duplicate
 * keys return false, otherwise return true.
 */
bool BPlusTree::InsertIntoLeaf(GenericKey *key, const RowId &value, Txn *transaction) {
  // printf("InsertIntoLeaf: 准备向叶页插入键，对应 RowId(%u, %u)\n", value.GetPageId(), value.GetSlotNum());

  // printf("a root_page_id_ = %d\n", root_page_id_);
    Page *page = FindLeafPage(key, root_page_id_, false);
    BPlusTreeLeafPage *leaf_page = reinterpret_cast<BPlusTreeLeafPage *>(page);
  // printf("b\n");

    RowId t;
    bool found = leaf_page->Lookup(key, t, processor_);
    if (found) {
        buffer_pool_manager_->UnpinPage(leaf_page->GetPageId(), false);
        return false;
    }
    
    leaf_page->Insert(key, value, processor_);
    if (leaf_page->GetSize() == leaf_page->GetMaxSize()) {
        BPlusTreeLeafPage *new_leaf_page = Split(leaf_page, transaction);

        // GenericKey *promoted_key = new GenericKey[processor_.GetKeySize()];
        // processor_.CopyKey(promoted_key, leaf_page->KeyAt(leaf_page->GetSize() - 1));
        InsertIntoParent(leaf_page, new_leaf_page->KeyAt(0), new_leaf_page, transaction);
    }
    page_id_t leaf_page_id = leaf_page->GetPageId();
    buffer_pool_manager_->UnpinPage(leaf_page_id, true);
    return true;
}

/*
 * Split input page and return newly created page.
 * Using template N to represent either internal page or leaf page.
 * User needs to first ask for new page from buffer pool manager(NOTICE: throw
 * an "out of memory" exception if returned value is nullptr), then move half
 * of key & value pairs from input page to newly created page
 */
BPlusTreeInternalPage *BPlusTree::Split(InternalPage *node, Txn *transaction) {
    page_id_t new_page_id;
    Page *new_page_raw = buffer_pool_manager_->NewPage(new_page_id);
    if (new_page_raw == nullptr) {
        throw std::runtime_error("Out of memory: Cannot allocate new internal page for B+ tree split.");
    }

    BPlusTreeInternalPage *new_internal_page = reinterpret_cast<BPlusTreeInternalPage *>(new_page_raw->GetData());
    new_internal_page->Init(new_page_id, node->GetParentPageId(), processor_.GetKeySize(), internal_max_size_);

    node->MoveHalfTo(new_internal_page, buffer_pool_manager_);
    buffer_pool_manager_->UnpinPage(new_page_id, true);

    return new_internal_page;
}

BPlusTreeLeafPage *BPlusTree::Split(LeafPage *leaf, Txn *transaction) {
    page_id_t new_page_id;
    Page *new_page_raw = buffer_pool_manager_->NewPage(new_page_id);
    if (new_page_raw == nullptr) {
        throw std::runtime_error("Out of memory: Cannot allocate new leaf page for B+ tree split.");
    }

    BPlusTreeLeafPage *new_leaf_page = reinterpret_cast<BPlusTreeLeafPage *>(new_page_raw->GetData());
    new_leaf_page->Init(new_page_id, leaf->GetParentPageId(), processor_.GetKeySize(), leaf_max_size_);

    leaf->MoveHalfTo(new_leaf_page);
    new_leaf_page->SetNextPageId(leaf->GetNextPageId());
    leaf->SetNextPageId(new_leaf_page->GetPageId());

    buffer_pool_manager_->UnpinPage(new_page_id, true);

    return new_leaf_page;
}

/*
 * Insert key & value pair into internal page after split
 * @param   old_node      input page from split() method
 * @param   key
 * @param   new_node      returned page from split() method
 * User needs to first find the parent page of old_node, parent node must be
 * adjusted to take info of new_node into account. Remember to deal with split
 * recursively if necessary.
 */
void BPlusTree::InsertIntoParent(BPlusTreePage *old_node, GenericKey *key, BPlusTreePage *new_node, Txn *transaction) {
  page_id_t parent_page_id = old_node->GetParentPageId();

  if(parent_page_id == INVALID_PAGE_ID) {
    page_id_t new_root_id;
    Page *new_root_raw_page = buffer_pool_manager_->NewPage(new_root_id);
    if (new_root_raw_page == nullptr) {
        throw std::runtime_error("Out of memory: Cannot allocate new root page for B+ tree.");
    }
    BPlusTreeInternalPage *new_root_page = reinterpret_cast<BPlusTreeInternalPage *>(new_root_raw_page->GetData());
    new_root_page->Init(new_root_id, INVALID_PAGE_ID, processor_.GetKeySize(),  internal_max_size_);
    new_root_page->PopulateNewRoot(old_node->GetPageId(), key, new_node->GetPageId());

    old_node->SetParentPageId(new_root_id);
    new_node->SetParentPageId(new_root_id);

    root_page_id_ = new_root_id;
    UpdateRootPageId(false);
    buffer_pool_manager_->UnpinPage(new_root_id, true);
    return;
  }
  else{
    Page *parent_page_raw = buffer_pool_manager_->FetchPage(parent_page_id);
    BPlusTreeInternalPage *parent_page = reinterpret_cast<BPlusTreeInternalPage *>(parent_page_raw->GetData());
    
    parent_page->InsertNodeAfter(old_node->GetPageId(), key, new_node->GetPageId());
    new_node->SetParentPageId(parent_page_id);

    if (parent_page->GetSize() > parent_page->GetMaxSize()) {
      BPlusTreeInternalPage *new_internal_page = Split(parent_page, transaction);
      GenericKey *promoted_key_from_internal = new GenericKey[processor_.GetKeySize()];
      processor_.CopyKey(parent_page->KeyAt(parent_page->GetKeySize() - 1), promoted_key_from_internal);
      InsertIntoParent(parent_page, promoted_key_from_internal, new_internal_page, transaction);
    }

    buffer_pool_manager_->UnpinPage(parent_page_id, true);
  }
}

/*****************************************************************************
 * REMOVE
 *****************************************************************************/
/*
 * Delete key & value pair associated with input key
 * If current tree is empty, return immediately.
 * If not, User needs to first find the right leaf page as deletion target, then
 * delete entry from leaf page. Remember to deal with redistribute or merge if
 * necessary.
 */
void BPlusTree::Remove(const GenericKey *key, Txn *transaction) {
  if(IsEmpty()){
    return;
  }
  Page *page = FindLeafPage(key, root_page_id_, false);
  if (page == nullptr) {
      return;
  }
  BPlusTreeLeafPage *leaf_page = reinterpret_cast<BPlusTreeLeafPage *>(page->GetData());
  page_id_t leaf_page_id = leaf_page->GetPageId();
  if(leaf_page == nullptr) {
    return;
  }

  int old_size = leaf_page->GetSize();
  int new_size = leaf_page->RemoveAndDeleteRecord(key, processor_);

  // 删除不存在的键时，不能继续执行下溢处理，否则会错误修改树结构。
  if (new_size == old_size) {
    buffer_pool_manager_->UnpinPage(leaf_page_id, false);
    return;
  }

  // After removal, check for underflow and handle it
  if (old_size > leaf_page->GetMinSize()) {
    buffer_pool_manager_->UnpinPage(leaf_page_id, true);
    return;
  }
  CoalesceOrRedistribute(leaf_page, transaction);
}

/* todo
 * User needs to first find the sibling of input page. If sibling's size + input
 * page's size > page's max size, then redistribute. Otherwise, merge.
 * Using template N to represent either internal page or leaf page.
 * @return: true means target leaf page should be deleted, false means no
 * deletion happens
 */
template <typename N>
bool BPlusTree::CoalesceOrRedistribute(N *&node, Txn *transaction) {
  if (node->IsRootPage()) { // Root page special case (handled by AdjustRoot)
    buffer_pool_manager_->UnpinPage(node->GetPageId(), true);
    return AdjustRoot(node);
  }

  Page *parent_page_raw = buffer_pool_manager_->FetchPage(node->GetParentPageId());
  BPlusTreeInternalPage *parent_page = reinterpret_cast<BPlusTreeInternalPage *>(parent_page_raw->GetData());

  int index_in_parent = parent_page->ValueIndex(node->GetPageId());
  int sibling_index = (index_in_parent == 0) ? 1 : index_in_parent - 1; // Prioritize left sibling if possible
  
  page_id_t sibling_page_id = parent_page->ValueAt(sibling_index);
  Page *sibling_raw_page = buffer_pool_manager_->FetchPage(sibling_page_id);
  N *sibling_node = reinterpret_cast<N *>(sibling_raw_page->GetData());

  bool result = false;
  if (node->GetSize() + sibling_node->GetSize() > node->GetMaxSize()) {
      Redistribute(sibling_node, node, index_in_parent);
      // buffer_pool_manager_->UnpinPage(sibling_node->GetPageId(), true);
      // buffer_pool_manager_->UnpinPage(node->GetPageId(), true);
      // buffer_pool_manager_->UnpinPage(parent_page->GetPageId(), false);
      result = false;
    
  } else {
    if(index_in_parent == 0){
      std::swap(node, sibling_node);
      std::swap(index_in_parent, sibling_index);
    }
      Coalesce(sibling_node, node, parent_page, index_in_parent, transaction);
      // buffer_pool_manager_->UnpinPage(sibling_node->GetPageId(), true);
      // buffer_pool_manager_->UnpinPage(node->GetPageId(), true);
      // buffer_pool_manager_->UnpinPage(parent_page->GetPageId(), true);
      result = true;
  }
      buffer_pool_manager_->UnpinPage(sibling_node->GetPageId(), true);
      buffer_pool_manager_->UnpinPage(parent_page->GetPageId(), true);

  return result;
}

/*
 * Move all the key & value pairs from one page to its sibling page, and notify
 * buffer pool manager to delete this page. Parent page must be adjusted to
 * take info of deletion into account. Remember to deal with coalesce or
 * redistribute recursively if necessary.
 * Using template N to represent either internal page or leaf page.
 * @param   neighbor_node      sibling page of input "node"
 * @param   node               input from method coalesceOrRedistribute()
 * @param   parent             parent page of input "node"
 * @return  true means parent node should be deleted, false means no deletion happened
 */
bool BPlusTree::Coalesce(LeafPage *&neighbor_node, LeafPage *&node, InternalPage *&parent, int index,
                         Txn *transaction) {
  if (index == 0) {
    neighbor_node->MoveAllTo(node);
    node->SetNextPageId(neighbor_node->GetNextPageId());
    buffer_pool_manager_->UnpinPage(neighbor_node->GetPageId(), true);
    parent->Remove(1);
  }
  else{
    node->MoveAllTo(neighbor_node);
    neighbor_node->SetNextPageId(node->GetNextPageId());
    buffer_pool_manager_->UnpinPage(neighbor_node->GetPageId(), true);
    parent->Remove(index);
  }

  buffer_pool_manager_->UnpinPage(node->GetPageId(), true);
  return CoalesceOrRedistribute(parent, transaction);
}

bool BPlusTree::Coalesce(InternalPage *&neighbor_node, InternalPage *&node, InternalPage *&parent, int index,
                         Txn *transaction) {
  if (index == 0) {
    neighbor_node->MoveAllTo(node, parent->KeyAt(1), buffer_pool_manager_);
    buffer_pool_manager_->UnpinPage(neighbor_node->GetPageId(), true);
    parent->Remove(1);
  }
  else{
    node->MoveAllTo(neighbor_node, parent->KeyAt(index), buffer_pool_manager_);
    buffer_pool_manager_->UnpinPage(neighbor_node->GetPageId(), true);
    parent->Remove(index);
  }

  buffer_pool_manager_->UnpinPage(node->GetPageId(), true);
  return CoalesceOrRedistribute(parent, transaction);
}

/*
 * Redistribute key & value pairs from one page to its sibling page. If index ==
 * 0, move sibling page's first key & value pair into end of input "node",
 * otherwise move sibling page's last key & value pair into head of input
 * "node".
 * Using template N to represent either internal page or leaf page.
 * @param   neighbor_node      sibling page of input "node"
 * @param   node               input from method coalesceOrRedistribute()
 */
void BPlusTree::Redistribute(LeafPage *neighbor_node, LeafPage *node, int index) {
  InternalPage* parent_page = reinterpret_cast<InternalPage*> (buffer_pool_manager_->FetchPage(node->GetParentPageId())->GetData());
    if (index == 0) {
    neighbor_node->MoveFirstToEndOf(node);
    parent_page->SetKeyAt(1, neighbor_node->KeyAt(0));
  } else {
    neighbor_node->MoveLastToFrontOf(node);
    parent_page->SetKeyAt(index, node->KeyAt(0));
  }
  buffer_pool_manager_->UnpinPage(parent_page->GetPageId(), true);
  buffer_pool_manager_->UnpinPage(neighbor_node->GetPageId(), true);
  buffer_pool_manager_->UnpinPage(node->GetPageId(), true);
}
void BPlusTree::Redistribute(InternalPage *neighbor_node, InternalPage *node, int index) {
  InternalPage* parent_page = reinterpret_cast<InternalPage*> (buffer_pool_manager_->FetchPage(node->GetParentPageId())->GetData());
    if (index == 0) {
    neighbor_node->MoveFirstToEndOf(node, parent_page->KeyAt(1), buffer_pool_manager_);
    parent_page->SetKeyAt(1, neighbor_node->KeyAt(0));
  } else {
    neighbor_node->MoveLastToFrontOf(node, parent_page->KeyAt(index), buffer_pool_manager_);
    parent_page->SetKeyAt(index, node->KeyAt(0));
  }
  buffer_pool_manager_->UnpinPage(parent_page->GetPageId(), true);
  buffer_pool_manager_->UnpinPage(neighbor_node->GetPageId(), true);
  buffer_pool_manager_->UnpinPage(node->GetPageId(), true);
}
/*
 * Update root page if necessary
 * NOTE: size of root page can be less than min size and this method is only
 * called within coalesceOrRedistribute() method
 * case 1: when you delete the last element in root page, but root page still
 * has one last child
 * case 2: when you delete the last element in whole b+ tree
 * @return : true means root page should be deleted, false means no deletion
 * happened
 */
bool BPlusTree::AdjustRoot(BPlusTreePage *old_root_node) {
  // case 1
  if(!old_root_node->IsLeafPage() && old_root_node->GetSize() == 1){
    auto root_page = reinterpret_cast<InternalPage*>(old_root_node);
    root_page_id_ = root_page->ValueAt(0);
    UpdateRootPageId(0);

    auto root_new_page = reinterpret_cast<BPlusTreePage*>(buffer_pool_manager_->FetchPage(root_page_id_)->GetData());
    root_new_page->SetParentPageId(INVALID_PAGE_ID);

    return true;
  }
  // case 2
  else if(old_root_node->IsLeafPage() && old_root_node->GetSize() == 0) {
    root_page_id_ = INVALID_PAGE_ID;
    UpdateRootPageId(0);
    buffer_pool_manager_->UnpinPage(old_root_node->GetPageId(), true);

    return true;
  }
  else{
    buffer_pool_manager_->UnpinPage(old_root_node->GetPageId(), true);
    return false;
  }
}

/*****************************************************************************
 * INDEX ITERATOR
 *****************************************************************************/
/*
 * Input parameter is void, find the left most leaf page first, then construct
 * index iterator
 * @return : index iterator
 */
IndexIterator BPlusTree::Begin() {
  if(IsEmpty()){
    return IndexIterator();
  }
  // Find the leftmost leaf page
  Page *page = FindLeafPage(nullptr, root_page_id_, true);
  auto leaf_page = reinterpret_cast<BPlusTreeLeafPage *>(page->GetData());
  buffer_pool_manager_->UnpinPage(page->GetPageId(), false);
  return IndexIterator(page->GetPageId(), buffer_pool_manager_);
}

/*
 * Input parameter is low key, find the leaf page that contains the input key
 * first, then construct index iterator
 * @return : index iterator
 */
IndexIterator BPlusTree::Begin(const GenericKey *key) {
  if (IsEmpty()) {
    return IndexIterator();
  }
  Page *raw_page = FindLeafPage(key, root_page_id_, false);
  if (raw_page == nullptr) {
    return IndexIterator();
  }
  auto *leaf_page = reinterpret_cast<BPlusTreeLeafPage *>(raw_page->GetData());
  page_id_t page_id = leaf_page->GetPageId();
  int index = leaf_page->KeyIndex(key, processor_);
  buffer_pool_manager_->UnpinPage(page_id, false);
  return IndexIterator(page_id, buffer_pool_manager_, index);
}

/*
 * Input parameter is void, construct an index iterator representing the end
 * of the key/value pair in the leaf node
 * @return : index iterator
 */
IndexIterator BPlusTree::End() {
  return IndexIterator(INVALID_PAGE_ID, buffer_pool_manager_, 0);
}

/*****************************************************************************
 * UTILITIES AND DEBUG
 *****************************************************************************/
/*
 * Find leaf page containing particular key, if leftMost flag == true, find
 * the left most leaf page
 * Note: the leaf page is pinned, you need to unpin it after use.
 */
Page *BPlusTree::FindLeafPage(const GenericKey *key, page_id_t page_id, bool leftMost) {
  if(IsEmpty()){
    return nullptr;
  }
  // page_id = (page_id == INVALID_PAGE_ID) ? root_page_id_ : page_id;
  auto page = buffer_pool_manager_->FetchPage(page_id);
  auto t_page = reinterpret_cast<BPlusTreePage *>(page->GetData());
  // printf("FindLeafPage page = %d\n", page_id);
  
  while (!t_page->IsLeafPage()) {
    BPlusTreeInternalPage *internal_page = reinterpret_cast<BPlusTreeInternalPage *>(page->GetData());
    page_id_t next_page_id;
// printf("@1 %d\n", leftMost);
    if (leftMost) {
      next_page_id = internal_page->ValueAt(0); // Leftmost child
    } else {
      // Find the appropriate child page based on the key
      next_page_id = internal_page->Lookup(key, processor_);
    }
// printf("@2\n");
    // printf("d next_page_id = %d\n", next_page_id);
    // auto next_page = reinterpret_cast<BPlusTreePage *>(buffer_pool_manager_->FetchPage(next_page_id)->GetData());
    // Unpin current page before fetching the next one
    buffer_pool_manager_->UnpinPage(internal_page->GetPageId(), false); // No modification, so not dirty
    page = buffer_pool_manager_->FetchPage(next_page_id);
    t_page = reinterpret_cast<BPlusTreePage *>(page->GetData());
    // printf("%llu, ", page->GetPageId());

  }
  // printf("c page = %d\n", page_id);
  return page;
}

/*
 * Update/Insert root page id in header page(where page_id = INDEX_ROOTS_PAGE_ID,
 * header_page isdefined under include/page/header_page.h)
 * Call this method everytime root page id is changed.
 * @parameter: insert_record      default value is false. When set to true,
 * insert a record <index_name, current_page_id> into header page instead of
 * updating it.
 */
void BPlusTree::UpdateRootPageId(int insert_record) {
    auto root_pages = reinterpret_cast<IndexRootsPage *>(buffer_pool_manager_->FetchPage(INDEX_ROOTS_PAGE_ID)->GetData());

    if (insert_record == 1) {
        root_pages->Insert(index_id_, root_page_id_);
    }
    else if (insert_record == 0){
        root_pages->Update(index_id_, root_page_id_);
    }
    buffer_pool_manager_->UnpinPage(INDEX_ROOTS_PAGE_ID, true); // Mark dirty
}

/**
 * This method is used for debug only, You don't need to modify
 */
void BPlusTree::ToGraph(BPlusTreePage *page, BufferPoolManager *bpm, std::ofstream &out, Schema *schema) const {
  std::string leaf_prefix("LEAF_");
  std::string internal_prefix("INT_");
  if (page->IsLeafPage()) {
    auto *leaf = reinterpret_cast<LeafPage *>(page);
    // Print node name
    out << leaf_prefix << leaf->GetPageId();
    // Print node properties
    out << "[shape=plain color=green ";
    // Print data of the node
    out << "label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" CELLPADDING=\"4\">\n";
    // Print data
    out << "<TR><TD COLSPAN=\"" << leaf->GetSize() << "\">P=" << leaf->GetPageId()
        << ",Parent=" << leaf->GetParentPageId() << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << leaf->GetSize() << "\">"
        << "max_size=" << leaf->GetMaxSize() << ",min_size=" << leaf->GetMinSize() << ",size=" << leaf->GetSize()
        << "</TD></TR>\n";
    out << "<TR>";
    for (int i = 0; i < leaf->GetSize(); i++) {
      Row ans;
      processor_.DeserializeToKey(leaf->KeyAt(i), ans, schema);
      out << "<TD>" << ans.GetField(0)->toString() << "</TD>\n";
    }
    out << "</TR>";
    // Print table end
    out << "</TABLE>>];\n";
    // Print Leaf node link if there is a next page
    if (leaf->GetNextPageId() != INVALID_PAGE_ID) {
      out << leaf_prefix << leaf->GetPageId() << " -> " << leaf_prefix << leaf->GetNextPageId() << ";\n";
      out << "{rank=same " << leaf_prefix << leaf->GetPageId() << " " << leaf_prefix << leaf->GetNextPageId() << "};\n";
    }

    // Print parent links if there is a parent
    if (leaf->GetParentPageId() != INVALID_PAGE_ID) {
      out << internal_prefix << leaf->GetParentPageId() << ":p" << leaf->GetPageId() << " -> " << leaf_prefix
          << leaf->GetPageId() << ";\n";
    }
  } else {
    auto *inner = reinterpret_cast<InternalPage *>(page);
    // Print node name
    out << internal_prefix << inner->GetPageId();
    // Print node properties
    out << "[shape=plain color=pink ";  // why not?
    // Print data of the node
    out << "label=<<TABLE BORDER=\"0\" CELLBORDER=\"1\" CELLSPACING=\"0\" CELLPADDING=\"4\">\n";
    // Print data
    out << "<TR><TD COLSPAN=\"" << inner->GetSize() << "\">P=" << inner->GetPageId()
        << ",Parent=" << inner->GetParentPageId() << "</TD></TR>\n";
    out << "<TR><TD COLSPAN=\"" << inner->GetSize() << "\">"
        << "max_size=" << inner->GetMaxSize() << ",min_size=" << inner->GetMinSize() << ",size=" << inner->GetSize()
        << "</TD></TR>\n";
    out << "<TR>";
    for (int i = 0; i < inner->GetSize(); i++) {
      out << "<TD PORT=\"p" << inner->ValueAt(i) << "\">";
      if (i > 0) {
        Row ans;
        processor_.DeserializeToKey(inner->KeyAt(i), ans, schema);
        out << ans.GetField(0)->toString();
      } else {
        out << " ";
      }
      out << "</TD>\n";
    }
    out << "</TR>";
    // Print table end
    out << "</TABLE>>];\n";
    // Print Parent link
    if (inner->GetParentPageId() != INVALID_PAGE_ID) {
      out << internal_prefix << inner->GetParentPageId() << ":p" << inner->GetPageId() << " -> " << internal_prefix
          << inner->GetPageId() << ";\n";
    }
    // Print leaves
    for (int i = 0; i < inner->GetSize(); i++) {
      auto child_page = reinterpret_cast<BPlusTreePage *>(bpm->FetchPage(inner->ValueAt(i))->GetData());
      ToGraph(child_page, bpm, out, schema);
      if (i > 0) {
        auto sibling_page = reinterpret_cast<BPlusTreePage *>(bpm->FetchPage(inner->ValueAt(i - 1))->GetData());
        if (!sibling_page->IsLeafPage() && !child_page->IsLeafPage()) {
          out << "{rank=same " << internal_prefix << sibling_page->GetPageId() << " " << internal_prefix
              << child_page->GetPageId() << "};\n";
        }
        bpm->UnpinPage(sibling_page->GetPageId(), false);
      }
    }
  }
  bpm->UnpinPage(page->GetPageId(), false);
}

/**
 * This function is for debug only, you don't need to modify
 */
void BPlusTree::ToString(BPlusTreePage *page, BufferPoolManager *bpm) const {
  if (page->IsLeafPage()) {
    auto *leaf = reinterpret_cast<LeafPage *>(page);
    std::cout << "Leaf Page: " << leaf->GetPageId() << " parent: " << leaf->GetParentPageId()
              << " next: " << leaf->GetNextPageId() << std::endl;
    for (int i = 0; i < leaf->GetSize(); i++) {
      std::cout << leaf->KeyAt(i) << ",";
    }
    std::cout << std::endl;
    std::cout << std::endl;
  } else {
    auto *internal = reinterpret_cast<InternalPage *>(page);
    std::cout << "Internal Page: " << internal->GetPageId() << " parent: " << internal->GetParentPageId() << std::endl;
    for (int i = 0; i < internal->GetSize(); i++) {
      std::cout << internal->KeyAt(i) << ": " << internal->ValueAt(i) << ",";
    }
    std::cout << std::endl;
    std::cout << std::endl;
    for (int i = 0; i < internal->GetSize(); i++) {
      ToString(reinterpret_cast<BPlusTreePage *>(bpm->FetchPage(internal->ValueAt(i))->GetData()), bpm);
      bpm->UnpinPage(internal->ValueAt(i), false);
    }
  }
}

bool BPlusTree::Check() {
  bool all_unpinned = buffer_pool_manager_->CheckAllUnpinned();
  if (!all_unpinned) {
    LOG(ERROR) << "problem in page unpin" << endl;
  }
  return all_unpinned;
}
