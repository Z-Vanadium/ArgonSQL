#include "executor/execute_engine.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <chrono>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <utility>

#include "common/result_writer.h"
#include "executor/executors/delete_executor.h"
#include "executor/executors/index_scan_executor.h"
#include "executor/executors/insert_executor.h"
#include "executor/executors/seq_scan_executor.h"
#include "executor/executors/update_executor.h"
#include "executor/executors/values_executor.h"
#include "glog/logging.h"
#include "planner/planner.h"
#include "executor/session_context.h"
#include "utils/utils.h"

extern "C" {
int yyparse(void);
#include "parser/minisql_lex.h"
#include "parser/parser.h"
}

namespace {
constexpr uint32_t kCatalogMetadataMagic = 89849;
constexpr uint32_t kObjectMetadataMagic = 344528;

// ExecuteEngine 的旧执行函数签名没有携带会话和输出流。把这两个值放在
// worker 线程的请求上下文中，可以保留旧执行器接口，同时避免修改所有
// executor/planner 的调用链。每个 TCP 请求都在一个线程内完成，因此不会
// 与其它连接串写输出或 USE DATABASE 状态。
thread_local SessionContext *g_active_session = nullptr;
thread_local std::ostream *g_active_output = nullptr;

bool HasValidCatalogMetadata(const std::string &database_path) {
  // DiskManager 的物理布局是：元数据页(0)、位图页(1)、Catalog 元数据页(2)。
  // 先检查 Catalog 魔数，再交给 DBStorageEngine 打开，避免把普通测试文件
  // 或损坏文件误判为数据库而触发 CatalogMeta 的断言。
  std::ifstream database_file(database_path, std::ios::binary);
  if (!database_file) {
    return false;
  }
  const auto read_uint32 = [&database_file](std::streamoff offset, uint32_t *value) {
    database_file.seekg(offset);
    if (!database_file) {
      return false;
    }
    database_file.read(reinterpret_cast<char *>(value), sizeof(*value));
    return database_file.gcount() == static_cast<std::streamsize>(sizeof(*value));
  };

  uint32_t magic = 0;
  if (!read_uint32(static_cast<std::streamoff>(2) * PAGE_SIZE, &magic) || magic != kCatalogMetadataMagic) {
    return false;
  }

  // CatalogMeta 后面依次保存 table_meta_pages_ 和 index_meta_pages_。
  // 除了 Catalog 自身，还检查每个引用的元数据页，避免加载“Catalog
  // 已落盘但表元数据仍未落盘”的半成品数据库。
  uint32_t table_count = 0;
  uint32_t index_count = 0;
  if (!read_uint32(static_cast<std::streamoff>(2) * PAGE_SIZE + 4, &table_count) ||
      !read_uint32(static_cast<std::streamoff>(2) * PAGE_SIZE + 8, &index_count)) {
    return false;
  }
  if (table_count > PAGE_SIZE / 8 || index_count > PAGE_SIZE / 8 ||
      table_count + index_count > (PAGE_SIZE - 12) / 8) {
    return false;
  }

  std::streamoff metadata_offset = static_cast<std::streamoff>(2) * PAGE_SIZE + 12;
  for (uint32_t i = 0; i < table_count + index_count; ++i) {
    uint32_t object_id = 0;
    uint32_t logical_page_id = 0;
    if (!read_uint32(metadata_offset, &object_id) || !read_uint32(metadata_offset + 4, &logical_page_id)) {
      return false;
    }
    (void)object_id;
    metadata_offset += 8;

    const uint32_t extent_id = logical_page_id / DiskManager::BITMAP_SIZE;
    const uint32_t page_offset = logical_page_id % DiskManager::BITMAP_SIZE;
    const std::streamoff physical_page =
        1 + static_cast<std::streamoff>(extent_id) * (DiskManager::BITMAP_SIZE + 1) + 1 + page_offset;
    uint32_t object_magic = 0;
    if (!read_uint32(physical_page * PAGE_SIZE, &object_magic) || object_magic != kObjectMetadataMagic) {
      return false;
    }
  }
  return true;
}
}  // namespace

ExecuteEngine::ExecuteEngine() {
  char path[] = "./databases";
  DIR *dir;
  if ((dir = opendir(path)) == nullptr) {
    mkdir("./databases", 0777);
    dir = opendir(path);
  }
  if (dir == nullptr) {
    throw std::runtime_error("Failed to open database directory.");
  }

  // ExecuteEngine 的 dbs_ 只保存当前进程打开的数据库对象。Server 重启后
  // 这些对象会被销毁，因此启动时必须重新扫描 databases/，按照磁盘文件
  // 重新构造 DBStorageEngine，才能让 SHOW/USE 找回之前创建的数据库。
  struct dirent *entry;
  while ((entry = readdir(dir)) != nullptr) {
    const std::string database_name = entry->d_name;
    if (database_name == "." || database_name == ".." || database_name.front() == '.') {
      continue;
    }

    // 只加载普通数据库文件，忽略目录、临时文件和其他特殊文件，避免
    // 把测试产物或目录误当作数据库打开。
    const std::string database_path = std::string(path) + "/" + database_name;
    struct stat file_status {};
    if (stat(database_path.c_str(), &file_status) != 0 || !S_ISREG(file_status.st_mode)) {
      continue;
    }
    if (!HasValidCatalogMetadata(database_path)) {
      LOG(WARNING) << "Skipping non-ArgonSQL database file: " << database_path;
      continue;
    }

    try {
      dbs_.emplace(database_name, new DBStorageEngine(database_name, false));
    } catch (const std::exception &ex) {
      // 单个数据库损坏时记录错误并继续启动，避免一个坏文件阻塞整个
      // Server；后续可以增加 CHECK/REPAIR 命令处理该数据库。
      LOG(ERROR) << "Failed to load database " << database_name << ": " << ex.what();
    }
  }
  closedir(dir);
}

ExecuteResult ExecuteEngine::Execute(pSyntaxNode ast, SessionContext *session) {
  ExecuteResult result;
  std::ostringstream output;

  // 通过线程局部请求状态把输出和当前数据库传给旧执行路径；不再修改
  // 进程级 std::cout，也不再把某个会话的数据库写入共享 current_db_。
  SessionContext *previous_session = g_active_session;
  std::ostream *previous_output = g_active_output;
  g_active_session = session;
  g_active_output = &output;

  // 只读查询可以共享访问数据库注册表；会改变 Catalog、Buffer Pool 或
  // 数据库生命周期的请求使用排他锁。这里的锁覆盖一次完整 SQL，保证
  // 同一数据库上的 DDL/DML 不与其它请求交错。
  std::unique_ptr<std::shared_lock<std::shared_mutex>> read_lock;
  std::unique_ptr<std::unique_lock<std::shared_mutex>> write_lock;
  const bool read_only = ast != nullptr &&
                         (ast->type_ == kNodeShowDB || ast->type_ == kNodeUseDB ||
                          ast->type_ == kNodeShowTables || ast->type_ == kNodeShowIndexes ||
                          ast->type_ == kNodeSelect);
  if (read_only) {
    read_lock = std::make_unique<std::shared_lock<std::shared_mutex>>(dbs_latch_);
  } else {
    write_lock = std::make_unique<std::unique_lock<std::shared_mutex>>(dbs_latch_);
  }

  try {
    result.status = Execute(ast);
    // 一些旧命令（例如 QUIT 或错误状态）通过 ExecuteInformation 输出
    // 附加提示；此时输出仍被重定向，因此也会进入 result.output。
    ExecuteInformation(result.status);
  } catch (const std::exception &ex) {
    result.status = DB_FAILED;
    Output() << "Error Encountered in ExecuteEngine: " << ex.what() << std::endl;
  }

  g_active_session = previous_session;
  g_active_output = previous_output;
  result.output = output.str();
  return result;
}

std::ostream &ExecuteEngine::Output() {
  return g_active_output == nullptr ? std::cout : *g_active_output;
}

const std::string &ExecuteEngine::CurrentDatabase() const {
  return g_active_session == nullptr ? current_db_ : g_active_session->GetCurrentDatabase();
}

void ExecuteEngine::SetCurrentDatabase(std::string database) {
  if (g_active_session == nullptr) {
    current_db_ = std::move(database);
  } else {
    g_active_session->SetCurrentDatabase(std::move(database));
  }
}

std::unique_ptr<AbstractExecutor> ExecuteEngine::CreateExecutor(ExecuteContext *exec_ctx,
                                                                const AbstractPlanNodeRef &plan) {
  switch (plan->GetType()) {
    // Create a new sequential scan executor
    case PlanType::SeqScan: {
      return std::make_unique<SeqScanExecutor>(exec_ctx, dynamic_cast<const SeqScanPlanNode *>(plan.get()));
    }
    // Create a new index scan executor
    case PlanType::IndexScan: {
      return std::make_unique<IndexScanExecutor>(exec_ctx, dynamic_cast<const IndexScanPlanNode *>(plan.get()));
    }
    // Create a new update executor
    case PlanType::Update: {
      auto update_plan = dynamic_cast<const UpdatePlanNode *>(plan.get());
      auto child_executor = CreateExecutor(exec_ctx, update_plan->GetChildPlan());
      return std::make_unique<UpdateExecutor>(exec_ctx, update_plan, std::move(child_executor));
    }
      // Create a new delete executor
    case PlanType::Delete: {
      auto delete_plan = dynamic_cast<const DeletePlanNode *>(plan.get());
      auto child_executor = CreateExecutor(exec_ctx, delete_plan->GetChildPlan());
      return std::make_unique<DeleteExecutor>(exec_ctx, delete_plan, std::move(child_executor));
    }
    case PlanType::Insert: {
      auto insert_plan = dynamic_cast<const InsertPlanNode *>(plan.get());
      auto child_executor = CreateExecutor(exec_ctx, insert_plan->GetChildPlan());
      return std::make_unique<InsertExecutor>(exec_ctx, insert_plan, std::move(child_executor));
    }
    case PlanType::Values: {
      return std::make_unique<ValuesExecutor>(exec_ctx, dynamic_cast<const ValuesPlanNode *>(plan.get()));
    }
    default:
      throw std::logic_error("Unsupported plan type.");
  }
}

dberr_t ExecuteEngine::ExecutePlan(const AbstractPlanNodeRef &plan, std::vector<Row> *result_set, Txn *txn,
                                   ExecuteContext *exec_ctx) {
  // Construct the executor for the abstract plan node
  auto executor = CreateExecutor(exec_ctx, plan);

  try {
    executor->Init();
    RowId rid{};
    Row row{};
    while (executor->Next(&row, &rid)) {
      if (result_set != nullptr) {
        result_set->push_back(row);
      }
    }
  } catch (const exception &ex) {
    Output() << "Error Encountered in Executor Execution: " << ex.what() << std::endl;
    if (result_set != nullptr) {
      result_set->clear();
    }
    return DB_FAILED;
  }
  return DB_SUCCESS;
}

dberr_t ExecuteEngine::Execute(pSyntaxNode ast) {
  if (ast == nullptr) {
    return DB_FAILED;
  }
  auto start_time = std::chrono::system_clock::now();
  unique_ptr<ExecuteContext> context(nullptr);
  if (!CurrentDatabase().empty()) {
    // Transaction belongs to the current Session, while Catalog/BufferPool
    // belong to the shared DBStorageEngine. Every executor therefore receives
    // both pieces of context instead of silently using a null transaction.
    Txn *txn = g_active_session == nullptr ? nullptr : g_active_session->GetTransaction();
    context = dbs_.at(CurrentDatabase())->MakeExecuteContext(txn);
  }
  switch (ast->type_) {
    case kNodeCreateDB:
      return ExecuteCreateDatabase(ast, context.get());
    case kNodeDropDB:
      return ExecuteDropDatabase(ast, context.get());
    case kNodeShowDB:
      return ExecuteShowDatabases(ast, context.get());
    case kNodeUseDB:
      return ExecuteUseDatabase(ast, context.get());
    case kNodeShowTables:
      return ExecuteShowTables(ast, context.get());
    case kNodeCreateTable:
      return ExecuteCreateTable(ast, context.get());
    case kNodeDropTable:
      return ExecuteDropTable(ast, context.get());
    case kNodeShowIndexes:
      return ExecuteShowIndexes(ast, context.get());
    case kNodeCreateIndex:
      return ExecuteCreateIndex(ast, context.get());
    case kNodeDropIndex:
      return ExecuteDropIndex(ast, context.get());
    case kNodeTrxBegin:
      return ExecuteTrxBegin(ast, context.get());
    case kNodeTrxCommit:
      return ExecuteTrxCommit(ast, context.get());
    case kNodeTrxRollback:
      return ExecuteTrxRollback(ast, context.get());
    case kNodeExecFile:
      return ExecuteExecfile(ast, context.get());
    case kNodeQuit:
      return ExecuteQuit(ast, context.get());
    default:
      break;
  }
  // Plan the query.
  Planner planner(context.get());
  std::vector<Row> result_set{};
  try {
    planner.PlanQuery(ast);
    // Execute the query.
    ExecutePlan(planner.plan_, &result_set, nullptr, context.get());
  } catch (const exception &ex) {
    Output() << "Error Encountered in Planner: " << ex.what() << std::endl;
    return DB_FAILED;
  }
  auto stop_time = std::chrono::system_clock::now();
  double duration_time =
      double((std::chrono::duration_cast<std::chrono::milliseconds>(stop_time - start_time)).count());
  // Return the result set as string.
  std::stringstream ss;
  ResultWriter writer(ss);

  if (planner.plan_->GetType() == PlanType::SeqScan || planner.plan_->GetType() == PlanType::IndexScan) {
    auto schema = planner.plan_->OutputSchema();
    auto num_of_columns = schema->GetColumnCount();
    if (!result_set.empty()) {
      // find the max width for each column
      vector<int> data_width(num_of_columns, 0);
      for (const auto &row : result_set) {
        for (uint32_t i = 0; i < num_of_columns; i++) {
          data_width[i] = max(data_width[i], int(row.GetField(i)->toString().size()));
        }
      }
      int k = 0;
      for (const auto &column : schema->GetColumns()) {
        data_width[k] = max(data_width[k], int(column->GetName().length()));
        k++;
      }
      // Generate header for the result set.
      writer.Divider(data_width);
      k = 0;
      writer.BeginRow();
      for (const auto &column : schema->GetColumns()) {
        writer.WriteHeaderCell(column->GetName(), data_width[k++]);
      }
      writer.EndRow();
      writer.Divider(data_width);

      // Transforming result set into strings.
      for (const auto &row : result_set) {
        writer.BeginRow();
        for (uint32_t i = 0; i < schema->GetColumnCount(); i++) {
          writer.WriteCell(row.GetField(i)->toString(), data_width[i]);
        }
        writer.EndRow();
      }
      writer.Divider(data_width);
    }
    writer.EndInformation(result_set.size(), duration_time, true);
  } else {
    writer.EndInformation(result_set.size(), duration_time, false);
  }
  Output() << writer.stream_.rdbuf();
  // todo:: use shared_ptr for schema
  if (ast->type_ == kNodeSelect)
      delete planner.plan_->OutputSchema();
  return DB_SUCCESS;
}

void ExecuteEngine::ExecuteInformation(dberr_t result) {
  switch (result) {
    case DB_ALREADY_EXIST:
      Output() << "Database already exists." << std::endl;
      break;
    case DB_NOT_EXIST:
      Output() << "Database not exists." << std::endl;
      break;
    case DB_TABLE_ALREADY_EXIST:
      Output() << "Table already exists." << std::endl;
      break;
    case DB_TABLE_NOT_EXIST:
      Output() << "Table not exists." << std::endl;
      break;
    case DB_INDEX_ALREADY_EXIST:
      Output() << "Index already exists." << std::endl;
      break;
    case DB_INDEX_NOT_FOUND:
      Output() << "Index not exists." << std::endl;
      break;
    case DB_COLUMN_NAME_NOT_EXIST:
      Output() << "Column not exists." << std::endl;
      break;
    case DB_KEY_NOT_FOUND:
      Output() << "Key not exists." << std::endl;
      break;
    case DB_QUIT:
      Output() << "Bye." << std::endl;
      break;
    default:
      break;
  }
}

dberr_t ExecuteEngine::ExecuteCreateDatabase(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteCreateDatabase" << std::endl;
#endif
  string db_name = ast->child_->val_;
  if (dbs_.find(db_name) != dbs_.end()) {
    return DB_ALREADY_EXIST;
  }
  dbs_.insert(make_pair(db_name, new DBStorageEngine(db_name, true)));
  return DB_SUCCESS;
}

dberr_t ExecuteEngine::ExecuteDropDatabase(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteDropDatabase" << std::endl;
#endif
  string db_name = ast->child_->val_;
  if (dbs_.find(db_name) == dbs_.end()) {
    return DB_NOT_EXIST;
  }
  remove(("./databases/" + db_name).c_str());
  delete dbs_[db_name];
  dbs_.erase(db_name);
  if (db_name == CurrentDatabase()) SetCurrentDatabase("");
  return DB_SUCCESS;
}

dberr_t ExecuteEngine::ExecuteShowDatabases(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteShowDatabases" << std::endl;
#endif
  if (dbs_.empty()) {
    Output() << "Empty set (0.00 sec)" << std::endl;
    return DB_SUCCESS;
  }
  int max_width = 8;
  for (const auto &itr : dbs_) {
    if (itr.first.length() > max_width) max_width = itr.first.length();
  }
  Output() << "+" << setfill('-') << setw(max_width + 2) << ""
       << "+" << endl;
  Output() << "| " << std::left << setfill(' ') << setw(max_width) << "Database"
       << " |" << endl;
  Output() << "+" << setfill('-') << setw(max_width + 2) << ""
       << "+" << endl;
  for (const auto &itr : dbs_) {
    Output() << "| " << std::left << setfill(' ') << setw(max_width) << itr.first << " |" << std::endl;
  }
  Output() << "+" << setfill('-') << setw(max_width + 2) << ""
       << "+" << endl;
  return DB_SUCCESS;
}

dberr_t ExecuteEngine::ExecuteUseDatabase(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteUseDatabase" << std::endl;
#endif
  string db_name = ast->child_->val_;
  if (dbs_.find(db_name) != dbs_.end()) {
    SetCurrentDatabase(db_name);
    Output() << "Database changed" << std::endl;
    return DB_SUCCESS;
  }
  return DB_NOT_EXIST;
}

dberr_t ExecuteEngine::ExecuteShowTables(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteShowTables" << std::endl;
#endif
  if (CurrentDatabase().empty()) {
    Output() << "No database selected" << std::endl;
    return DB_FAILED;
  }
  vector<TableInfo *> tables;
  if (dbs_.at(CurrentDatabase())->catalog_mgr_->GetTables(tables) == DB_FAILED) {
    Output() << "Empty set (0.00 sec)" << std::endl;
    return DB_FAILED;
  }
  string table_in_db("Tables_in_" + CurrentDatabase());
  uint max_width = table_in_db.length();
  for (const auto &itr : tables) {
    if (itr->GetTableName().length() > max_width) max_width = itr->GetTableName().length();
  }
  Output() << "+" << setfill('-') << setw(max_width + 2) << ""
       << "+" << endl;
  Output() << "| " << std::left << setfill(' ') << setw(max_width) << table_in_db << " |" << std::endl;
  Output() << "+" << setfill('-') << setw(max_width + 2) << ""
       << "+" << endl;
  for (const auto &itr : tables) {
    Output() << "| " << std::left << setfill(' ') << setw(max_width) << itr->GetTableName() << " |" << std::endl;
  }
  Output() << "+" << setfill('-') << setw(max_width + 2) << ""
       << "+" << endl;
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t ExecuteEngine::ExecuteCreateTable(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteCreateTable" << std::endl;
#endif
  if (CurrentDatabase().empty() || context == nullptr) {
    Output() << "No database selected" << std::endl;
    return DB_NOT_EXIST;
  }

  // CREATE TABLE 的 AST 是：table_name -> column_definition_list。
  // 旧实现错误地把 table_name 当成数据库名重新打开 DBStorageEngine，
  // 导致 SQL 层无法真正创建表；这里将语法树转换成 Catalog 所需的 Schema。
  pSyntaxNode table_name_node = ast->child_;
  pSyntaxNode definition_list = table_name_node == nullptr ? nullptr : table_name_node->next_;
  if (table_name_node == nullptr || definition_list == nullptr) return DB_FAILED;

  std::vector<Column *> columns;
  uint32_t column_index = 0;
  for (pSyntaxNode definition = definition_list->child_; definition != nullptr; definition = definition->next_) {
    pSyntaxNode name_node = definition->child_;
    pSyntaxNode type_node = name_node == nullptr ? nullptr : name_node->next_;
    if (name_node == nullptr || type_node == nullptr || name_node->val_ == nullptr || type_node->val_ == nullptr) {
      for (auto *column : columns) delete column;
      return DB_FAILED;
    }
    const bool unique = definition->val_ != nullptr && std::string(definition->val_) == "unique";
    const std::string type_name = type_node->val_;
    if (type_name == "int") {
      columns.emplace_back(new Column(name_node->val_, TypeId::kTypeInt, column_index++, true, unique));
    } else if (type_name == "float") {
      columns.emplace_back(new Column(name_node->val_, TypeId::kTypeFloat, column_index++, true, unique));
    } else if (type_name == "char" && type_node->child_ != nullptr && type_node->child_->val_ != nullptr) {
      const uint32_t length = static_cast<uint32_t>(std::stoul(type_node->child_->val_));
      columns.emplace_back(new Column(name_node->val_, TypeId::kTypeChar, length, column_index++, true, unique));
    } else {
      for (auto *column : columns) delete column;
      return DB_FAILED;
    }
  }

  auto *schema = new TableSchema(columns);
  TableInfo *table_info = nullptr;
  const dberr_t result = context->GetCatalog()->CreateTable(table_name_node->val_, schema,
                                                             context->GetTransaction(), table_info);
  delete schema;
  if (result == DB_SUCCESS) Output() << "Query OK, 0 rows affected" << std::endl;
  return result;
}

/**
 * TODO: Student Implement
 */
dberr_t ExecuteEngine::ExecuteDropTable(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteDropTable" << std::endl;
#endif
  string db_name = ast->child_->val_;

  if(dbs_.find(db_name) == dbs_.end()){
    return DB_NOT_EXIST;
  }
  else{
    auto remove_path = ("./databases/" + db_name).c_str();
    remove(remove_path);
    delete dbs_[db_name];
    dbs_.erase(db_name);
    if(db_name == CurrentDatabase()){
      SetCurrentDatabase("");
    }
    return DB_SUCCESS;
  }
}

/**
 * TODO: Student Implement
 */
dberr_t ExecuteEngine::ExecuteShowIndexes(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteShowIndexes" << std::endl;
#endif
  if (dbs_.empty()) {
    Output() << "Empty set (0.00 sec)" << std::endl;
    return DB_SUCCESS;
  }
  int max_width = 8;
  for (const auto &itr : dbs_) {
    if (itr.first.length() > max_width){
      max_width = itr.first.length();
    }
  }
  Output() << "+" << setfill('-') << setw(max_width + 2) << ""
       << "+" << endl;
  Output() << "| " << std::left << setfill(' ') << setw(max_width) << "Database"
       << " |" << endl;
  Output() << "+" << setfill('-') << setw(max_width + 2) << ""
       << "+" << endl;
  for (const auto &itr : dbs_) {
    Output() << "| " << std::left << setfill(' ') << setw(max_width) << itr.first << " |" << std::endl;
  }
  Output() << "+" << setfill('-') << setw(max_width + 2) << ""
       << "+" << endl;
  return DB_SUCCESS;

}

/**
 * TODO: Student Implement
 */
dberr_t ExecuteEngine::ExecuteCreateIndex(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteCreateIndex" << std::endl;
#endif
  if(CurrentDatabase().empty()){
      Output() << "No database selected" << std::endl;
      return DB_NOT_EXIST;
  }
  else{
    std::vector<TableInfo*> tables;
    TableInfo* table_info = nullptr;
    dbs_.at(CurrentDatabase())->catalog_mgr_->GetTables(tables);

    auto table_name = ast->child_->next_->val_;
    bool table_is_exist = false;
    for(const auto& table:tables){
      if (table->GetTableName() == table_name) {
        table_is_exist = true;
        table_info = table;
        break;
      }
    }
    
    if (!table_is_exist) {
      Output() << "Table not exists." << std::endl;
      return DB_TABLE_NOT_EXIST;
    }

    auto index_name = ast->child_->val_;
    std::vector<IndexInfo*> indexs;
    dbs_.at(CurrentDatabase())->catalog_mgr_->GetTableIndexes(table_name, indexs);
    for (const auto &index:indexs) {
      if (index->GetIndexName() == index_name) {
        Output() << "Index " << index_name << " already exists in " << table_name << std::endl;
        return DB_INDEX_ALREADY_EXIST;
      }
    }

    std::string index_type;
    index_type = "btree";
    pSyntaxNode index_node = ast->child_;
    while (index_node->next_ != nullptr) {
      index_node = index_node->next_;
    }
    if (index_node->val_ != nullptr && string(index_node->val_) == "index type") {
      index_type = index_node->child_->val_;
    }

    std::vector<std::string> column_names;
    pSyntaxNode column_node = ast->child_->next_->next_->child_;
    while (column_node != nullptr) {
      column_names.emplace_back(column_node->val_);
      column_node = column_node->next_;
    }

    Txn txn;
    IndexInfo* index_info = nullptr;
    auto status = dbs_.at(CurrentDatabase())->catalog_mgr_->CreateIndex(table_name, index_name, column_names, &txn, index_info, index_type);
    if(status != DB_SUCCESS){
      return DB_SUCCESS;
    }

    TableIterator iter(table_info->GetTableHeap()->Begin(nullptr));
    for (; iter != table_info->GetTableHeap()->End(); ++iter) {
      Row row = *iter;
      std::vector<Column*> columns = index_info->GetIndexKeySchema()->GetColumns();
      ASSERT(columns.size() == 1, "InsertExecutor only support single column index");
      
      std::vector<Field> fields;
      fields.push_back(*(row.GetField(columns[0]->GetTableInd())));
      Row index_row(fields);
      index_row.SetRowId(row.GetRowId());
      index_info->GetIndex()->InsertEntry(index_row, row.GetRowId(), nullptr);
    }

    Output() << "Query OK, 0 rows affected" << std::endl;
    return DB_SUCCESS;
  }
  
}

/**
 * TODO: Student Implement
 */
dberr_t ExecuteEngine::ExecuteDropIndex(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteDropIndex" << std::endl;
#endif
  if(CurrentDatabase().empty()){
    Output()<<"No database selected"<<std::endl;
    return DB_NOT_EXIST;
  }
  else{
    std::vector<TableInfo*> tables;
    TableInfo* table_info = nullptr;
    dbs_.at(CurrentDatabase())->catalog_mgr_->GetTables(tables);
    auto index_name = ast->child_->val_;
    bool index_is_exist = false;

    for(const auto &table:tables){
      auto is_db_success=dbs_.at(CurrentDatabase())->catalog_mgr_->DropIndex(table->GetTableName(), index_name);
      if(is_db_success == DB_SUCCESS){
        index_is_exist = true;
      }
    }
    
    if(index_is_exist){
      Output() << "Index dropped successfully" << std::endl;
      return DB_SUCCESS;
    }
    else{
      Output() << "Index not found" << std::endl;
      return DB_FAILED;
    }
  }
}

dberr_t ExecuteEngine::ExecuteTrxBegin(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteTrxBegin" << std::endl;
#endif
  if (g_active_session == nullptr || context == nullptr || context->GetTxnManager() == nullptr) {
    Output() << "No database selected" << std::endl;
    return DB_FAILED;
  }
  if (g_active_session->GetTransaction() != nullptr) {
    Output() << "Transaction already active" << std::endl;
    return DB_FAILED;
  }
  Txn *txn = context->GetTxnManager()->Begin();
  g_active_session->SetTransaction(txn, context->GetTxnManager());
  Output() << "Transaction started" << std::endl;
  return DB_SUCCESS;
}

dberr_t ExecuteEngine::ExecuteTrxCommit(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteTrxCommit" << std::endl;
#endif
  if (g_active_session == nullptr || context == nullptr || g_active_session->GetTransaction() == nullptr) {
    Output() << "No active transaction" << std::endl;
    return DB_FAILED;
  }
  context->GetTxnManager()->Commit(g_active_session->GetTransaction());
  g_active_session->ClearTransaction();
  Output() << "Transaction committed" << std::endl;
  return DB_SUCCESS;
}

dberr_t ExecuteEngine::ExecuteTrxRollback(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteTrxRollback" << std::endl;
#endif
  if (g_active_session == nullptr || context == nullptr || g_active_session->GetTransaction() == nullptr) {
    Output() << "No active transaction" << std::endl;
    return DB_FAILED;
  }
  context->GetTxnManager()->Abort(g_active_session->GetTransaction());
  g_active_session->ClearTransaction();
  Output() << "Transaction rolled back" << std::endl;
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t ExecuteEngine::ExecuteExecfile(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteExecfile" << std::endl;
#endif

  auto start_time = std::chrono::system_clock::now();
  std::string file_name = ast->child_->val_;
  ifstream sql_file(file_name, ios::in);
  
  if (!sql_file.is_open()) {
    Output() << "Failed to open file: " << file_name << std::endl;
    return DB_FAILED;
  }

  std::string sql_line;
  while (getline(sql_file, sql_line))
  {
    if(sql_line.empty() || sql_line[0] == '#'){
      continue;
    }
    else{
      YY_BUFFER_STATE buffer;
      buffer = yy_scan_string(sql_line.c_str());

      if (buffer == nullptr) {
        LOG(ERROR) << "Failed to create yy buffer state." << std::endl;
        sql_file.close();
        return DB_FAILED;
      }

      yy_switch_to_buffer(buffer);
      MinisqlParserInit();
      yyparse();


      if (MinisqlParserGetError()) {
        Output() << "SQL Error: " << MinisqlParserGetErrorMessage() << std::endl;
        MinisqlParserFinish();
        yy_delete_buffer(buffer);
        yylex_destroy();
        sql_file.close();
        return DB_FAILED;
      }

      dberr_t result = Execute(MinisqlGetParserRootNode());
      if (result != DB_SUCCESS) {
        MinisqlParserFinish();
        yy_delete_buffer(buffer);
        yylex_destroy();
        sql_file.close();
        return result;
      }

      MinisqlParserFinish();
      yy_delete_buffer(buffer);
      yylex_destroy();
    }
  }
  
  auto end_time = std::chrono::system_clock::now();
  double duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count() / 1000.0;
  Output() << "Query OK. (" << duration << " sec)" << std::endl;
  sql_file.close();
  return DB_SUCCESS;
}

/**
 * TODO: Student Implement
 */
dberr_t ExecuteEngine::ExecuteQuit(pSyntaxNode ast, ExecuteContext *context) {
#ifdef ENABLE_EXECUTE_DEBUG
  LOG(INFO) << "ExecuteQuit" << std::endl;
#endif
  for(auto& entry:dbs_){
    DBStorageEngine*& db = entry.second;
    delete db;
  }
  return DB_QUIT;
}
