#include "index/b_plus_tree.h"

#include "common/instance.h"
#include "gtest/gtest.h"
#include "index/comparator.h"
#include "utils/tree_file_mgr.h"
#include "utils/utils.h"

static const std::string db_name = "bp_tree_insert_test.db";

TEST(BPlusTreeTests, SampleTest) {
  // Init engine
  // printf("1\n");
  DBStorageEngine engine(db_name);
  // printf("1.1\n");
  std::vector<Column *> columns = {
      new Column("int", TypeId::kTypeInt, 0, false, false),
  };
  // printf("1.2\n");
  Schema *table_schema = new Schema(columns);
  // printf("1.3\n");
  KeyManager KP(table_schema, 17);
  // printf("1.4\n");
  BPlusTree tree(0, engine.bpm_, KP);
  // printf("1.5\n");
  TreeFileManagers mgr("tree_");
  // Prepare data
  // printf("2\n");
  const int n = 2000;
  vector<GenericKey *> keys;
  vector<RowId> values;
  vector<GenericKey *> delete_seq;
  map<GenericKey *, RowId> kv_map;
  for (int i = 0; i < n; i++) {
    GenericKey *key = KP.InitKey();
    std::vector<Field> fields{Field(TypeId::kTypeInt, i)};
    KP.SerializeFromKey(key, Row(fields), table_schema);
    keys.push_back(key);
    values.push_back(RowId(i));
    delete_seq.push_back(key);
  }
  vector<GenericKey *> keys_copy(keys);
  // Shuffle data
  // printf("3\n");
  ShuffleArray(keys);
  ShuffleArray(values);
  ShuffleArray(delete_seq);
  // Map key value
  for (int i = 0; i < n; i++) {
    kv_map[keys[i]] = values[i];
  }
  // Insert data
  // printf("4\n");
  for (int i = 0; i < n; i++) {
    
    // printf("4. %d %d %d\n", i+1, 0, 0);
    tree.Insert(keys[i], values[i]);
  }
  // printf("4+\n");
  ASSERT_TRUE(tree.Check());
  // Print tree
  // printf("5\n");
  tree.PrintTree(mgr[0], table_schema);
  // Search keys
  // printf("6\n");
  vector<RowId> ans;
  for (int i = 0; i < n; i++) {
    tree.GetValue(keys_copy[i], ans);
    ASSERT_EQ(kv_map[keys_copy[i]], ans[i]);
  }
  ASSERT_TRUE(tree.Check());
  // Delete half keys
  // printf("7\n");
  for (int i = 0; i < n / 2; i++) {
    tree.Remove(delete_seq[i]);
  }
  tree.PrintTree(mgr[1], table_schema);
  // Check valid
  // printf("8\n");
  ans.clear();
  for (int i = 0; i < n / 2; i++) {
    ASSERT_FALSE(tree.GetValue(delete_seq[i], ans));
  }
  for (int i = n / 2; i < n; i++) {
    ASSERT_TRUE(tree.GetValue(delete_seq[i], ans));
    ASSERT_EQ(kv_map[delete_seq[i]], ans[ans.size() - 1]);
  }
}