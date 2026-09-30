#include "recovery/log_rec.h"

// LogRec 的全局日志序列状态必须只有一份。放在独立编译单元中，
// 既符合 C++11 的规则，也保证不同执行器/线程看到同一个 LSN 链。
std::unordered_map<txn_id_t, lsn_t> LogRec::prev_lsn_map_{};
lsn_t LogRec::next_lsn_{0};
std::mutex LogRec::latch_{};
