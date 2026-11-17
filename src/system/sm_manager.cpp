// Copyright (c) 2023-2027 Renmin University of China
// SPDX-License-Identifier: MulanPSL-2.0

#include "sm_manager.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <sstream>
#include <system_error>
#include <utility>

#include "common/context.h"
#include "common/wire_result.h"
#include "index/index_manager.h"
#include "record/rm_manager.h"
#include "record/rm_scan.h"
#include "storage/buffer_pool_manager.h"
#include "storage/disk_manager.h"

namespace {

namespace fs = std::filesystem;

/**
 * @brief 将数据库名约束为当前目录下的单个路径分量。
 * @throws RMDBError 名称为空、包含父目录或包含路径分隔符。
 */
fs::path database_path(const std::string& db_name) {
    fs::path path(db_name);
    if (path.empty() || path != path.filename() || path == "." || path == "..") {
        throw RMDBError("Invalid database name: " + db_name);
    }
    return path;
}

/**
 * @brief 序列化后再覆盖元数据文件，避免序列化失败时提前清空旧文件。
 * @throws InternalError 序列化、打开或写入失败。
 */
void write_meta_file(const fs::path& path, const DbMeta& db) {
    std::ostringstream serialized;
    serialized << db;
    if (!serialized) {
        throw InternalError("cannot serialize database metadata");
    }

    std::ofstream output(path, std::ios::out | std::ios::trunc);
    if (!output) {
        throw InternalError("cannot open database metadata: " + path.string());
    }
    output << serialized.str();
    output.flush();
    if (!output) {
        throw InternalError("cannot write database metadata: " + path.string());
    }
}

/**
 * @brief 将系统命令的字符串结果转换成统一的 WireResultSet。
 * @throws InternalError 上下文为空、行列不匹配或结果超过教学版缓存上限。
 */
void set_string_result(Context* context,
                       std::vector<std::string> column_names,
                       std::vector<std::vector<std::string>> row_values) {
    if (context == nullptr) {
        throw InternalError("utility result requires a request context");
    }

    std::vector<WireResultColumn> columns;
    columns.reserve(column_names.size());
    for (auto& name : column_names) {
        columns.push_back({.name = std::move(name), .type = TYPE_STRING});
    }
    context->result().set_columns(std::move(columns));

    for (auto& values : row_values) {
        std::vector<WireResultCell> row;
        row.reserve(values.size());
        for (auto& value : values) {
            row.push_back({.type = TYPE_STRING, .str_val = std::move(value)});
        }
        context->result().add_row(std::move(row));
    }
}

}  // namespace

bool SmManager::is_dir(const std::string& db_name) const { return fs::is_directory(database_path(db_name)); }

void SmManager::create_db(const std::string& db_name) const {
    const fs::path db_path = database_path(db_name);
    if (fs::exists(db_path)) {
        throw DatabaseExistsError(db_name);
    }

    if (!fs::create_directory(db_path)) {
        throw DatabaseExistsError(db_name);
    }

    try {
        DbMeta new_db;
        new_db.name_ = db_name;
        write_meta_file(db_path / DB_META_NAME, new_db);
        disk_manager_->create_file((db_path / LOG_FILE_NAME).string());
    } catch (...) {
        // 只回滚本次刚创建的目录，不触碰任何既有路径。
        std::error_code ignored;
        fs::remove_all(db_path, ignored);
        throw;
    }
}

void SmManager::drop_db(const std::string& db_name) {
    const fs::path db_path = database_path(db_name);
    if (!fs::is_directory(db_path) || !fs::is_regular_file(db_path / DB_META_NAME)) {
        throw DatabaseNotFoundError(db_name);
    }
    if (db_.name_ == db_name) {
        throw InternalError("cannot drop an open database: " + db_name);
    }
    if (fs::remove_all(db_path) == 0) {
        throw DatabaseNotFoundError(db_name);
    }
}

void SmManager::open_db(const std::string& db_name) {
    // TODO(Lab 3): 加载数据库元数据，并打开所有表文件和索引文件。
    const fs::path db_path = database_path(db_name);
    if (!fs::is_directory(db_path)) {
        throw DatabaseNotFoundError(db_name);
    }

    // 1. 读取元数据文件
    std::ifstream ifs(db_path / DB_META_NAME);
    if (!ifs) {
        throw InternalError("cannot open database metadata");
    }
    ifs >> db_;
    ifs.close();

    // 2. 切换到数据库目录
    fs::current_path(db_path);

    try {
        // 3. 打开所有表文件
        for (const auto& [tab_name, tab] : db_.tabs_) {
            fhs_.emplace(tab_name, rm_manager_->open_file(tab_name));
        }

        // 4. 打开所有索引文件
        for (const auto& [tab_name, tab] : db_.tabs_) {
            for (const auto& index : tab.indexes) {
                std::string ix_name = index_manager_->make_index_name(tab_name, index.cols);
                // 用 index.cols 打开
                indexes_.emplace(ix_name, index_manager_->open_index(tab_name, index.cols));
            }
        }
    } catch (...) {
        // 异常回滚
        fhs_.clear();
        indexes_.clear();
        db_ = DbMeta();
        fs::current_path(db_path.parent_path());
        throw;
    }
}

void SmManager::flush_meta() const { write_meta_file(DB_META_NAME, db_); }

void SmManager::show_database(Context* context) const { set_string_result(context, {"Database"}, {{db_.name_}}); }

void SmManager::close_db() {
    // TODO(Lab 3): 刷盘并关闭所有表、索引和数据库元数据。

    if (db_.name_.empty()) {
        return; 
    }
    
    // 1. 刷盘
    flush_meta();
    
    // 2. 关闭表文件（unique_ptr 析构自动处理）
    fhs_.clear();
    
    // 3. 关闭索引文件（必须显式调用 close_index）
    for (auto& [ix_name, ix] : indexes_) {
        index_manager_->close_index(ix.get());
    }
    indexes_.clear();
    
    // 4. 清理元数据并切回上一级目录
    db_.tabs_.clear();
    db_.name_.clear();
    fs::current_path("..");
}

void SmManager::show_tables(Context* context) {
    std::vector<std::vector<std::string>> rows;
    rows.reserve(db_.tabs_.size());
    // C++20：`map | std::views::values` 只遍历 value（表元数据），不关心 key。
    // 等价于 for (const auto& [name, tab] : db_.tabs_) { ... 使用 tab ... }
    for (const auto& tab : db_.tabs_ | std::views::values) {
        rows.push_back({tab.name});
    }
    set_string_result(context, {"Tables"}, std::move(rows));
}

void SmManager::desc_table(const std::string& tab_name, Context* context) {
    const TabMeta& tab = db_.get_table(tab_name);

    std::vector<std::vector<std::string>> rows;
    rows.reserve(tab.cols.size());
    for (const auto& col : tab.cols) {
        rows.push_back({col.name, coltype2str(col.type), col.index ? "YES" : "NO"});
    }
    set_string_result(context, {"Field", "Type", "Index"}, std::move(rows));
}

void SmManager::create_table(const std::string& tab_name, const std::vector<ColDef>& col_defs, Context* context) {
    if (db_.is_table(tab_name)) {
        throw TableExistsError(tab_name);
    }
    // C++20：unordered_map::contains，避免 find/end 比较。
    if (fhs_.contains(tab_name)) {
        throw InternalError("Record handle already exists: " + tab_name);
    }
    if (col_defs.empty()) {
        throw InvalidRecordSizeError(0);
    }

    std::vector<std::string> col_names;
    col_names.reserve(col_defs.size());
    int record_size = 0;
    // C++17 结构化绑定：ColDef 是聚合类型，可拆成 name/type/len。
    // 等价于 for (const auto& col_def : col_defs) { 使用 col_def.name 等 }
    for (const auto& [name, type, len] : col_defs) {
        // C++20 ranges::find：直接对容器查找，无需 begin()/end()。
        if (std::ranges::find(col_names, name) != col_names.end()) {
            throw ColumnExistsError(name);
        }
        col_names.push_back(name);

        const bool fixed_length_mismatch = (type == TYPE_INT && len != static_cast<int>(sizeof(int))) ||
                                           (type == TYPE_FLOAT && len != static_cast<int>(sizeof(float)));
        if (len <= 0 || fixed_length_mismatch) {
            throw InvalidColLengthError(len);
        }
        if (len > RM_MAX_RECORD_SIZE) {
            throw InvalidRecordSizeError(len);
        }
        const int next_record_size = record_size + len;
        if (next_record_size > RM_MAX_RECORD_SIZE) {
            throw InvalidRecordSizeError(next_record_size);
        }
        record_size = next_record_size;
    }

    // 字段按声明顺序连续存放，offset 是字段在一条记录中的起始位置。
    int curr_offset = 0;
    TabMeta tab;
    tab.name = tab_name;
    tab.cols.reserve(col_defs.size());
    // 同上：结构化绑定展开 ColDef 的三个成员。
    for (const auto& [name, type, len] : col_defs) {
        ColMeta col;
        col.tab_name = tab_name;
        col.name = name;
        col.type = type;
        col.len = len;
        col.offset = curr_offset;
        curr_offset += len;
        tab.cols.push_back(std::move(col));
    }

    // 先构造候选目录，只有文件句柄和 db.meta 都准备成功后才替换当前目录。
    DbMeta next_db = db_;
    next_db.tabs_[tab_name] = tab;
    rm_manager_->create_file(tab_name, record_size);

    try {
        fhs_.emplace(tab_name, rm_manager_->open_file(tab_name));
        write_meta_file(DB_META_NAME, next_db);
        db_ = std::move(next_db);
    } catch (...) {
        // 只清理本次建表产生的句柄和文件，再保留原目录供调用者继续使用。
        if (auto handle = fhs_.find(tab_name); handle != fhs_.end()) {
            rm_manager_->close_file(handle->second.get());
            fhs_.erase(handle);
        }
        rm_manager_->destroy_file(tab_name);
        throw;
    }
}

void SmManager::drop_table(const std::string& tab_name, Context* context) {
    // TODO(Lab 3): 删除表的索引、记录文件、句柄和元数据。

    // 1. 获取表元数据（如果不存在会抛异常）
    const TabMeta& tab = db_.get_table(tab_name);

    // 2. 收集并删除该表上所有的索引
    std::vector<std::vector<std::string>> cols_to_drop;
    for (const auto& index : tab.indexes) {
        std::vector<std::string> col_names;
        for (const auto& col : index.cols) {
            col_names.push_back(col.name);
        }
        cols_to_drop.push_back(col_names);
    }
    for (const auto& cols : cols_to_drop) {
        drop_index(tab_name, cols, context);
    }

    // 3. 关闭并删除表文件
    auto it = fhs_.find(tab_name);
    if (it != fhs_.end()) {
        rm_manager_->close_file(it->second.get()); // 显式关闭
        fhs_.erase(it); // 从 map 中移除
    }
    rm_manager_->destroy_file(tab_name); // 物理删除文件

    // 4. 移除元数据并刷盘
    db_.tabs_.erase(tab_name);
    flush_meta();
}

void SmManager::create_index(const std::string& tab_name,
                             const std::vector<std::string>& col_names,
                             [[maybe_unused]] bool unique,
                             Context* context) {
    // TODO(Lab 3): 用 IndexMeta::make(表, 列, unique) 构造定义，再交给 IndexManager::create_index。
    // 表中已有的记录也要插入新索引。若声明了 UNIQUE 而已有数据存在重复 key，
    // 建索引应失败：清理已创建的索引文件和句柄，不修改元数据，再把异常抛给上层。
    const TabMeta& tab = db_.get_table(tab_name);

    std::string ix_name = index_manager_->make_index_name(tab_name, col_names);
    if (indexes_.contains(ix_name)) {
        throw RMDBError("Index already exists: " + ix_name);
    }

    // 1. 提取 ColMeta
    std::vector<ColMeta> cols;
    cols.reserve(col_names.size());
    for (const auto& col_name : col_names) {
        cols.push_back(*tab.get_col(col_name));
    }

    // 2. 直接用框架提供的 make 方法构造元数据
    IndexMeta index_meta = IndexMeta::make(tab_name, std::move(cols), unique);

    // 3. 创建并打开索引文件
    index_manager_->create_index(index_meta);
    auto ix = index_manager_->open_index(tab_name, index_meta.cols);

    // 4. 遍历已有表数据，批量插入索引
    auto* fh = fhs_[tab_name].get();
    RmScan scan(fh);
    try {
        for (; !scan.is_end(); scan.next()) {
            Rid rid = scan.rid();
            auto rec = fh->get_record(rid, context);

            // 直接用框架提供的 make_key 提取键值，省去手动拼装
            std::vector<char> key_data = index_meta.make_key(rec->data);
            
            // 如果是 unique 且冲突，底层 B+ 树会抛异常
            ix->insert_entry(key_data.data(), rid, nullptr);
        }
    } catch (...) {
        // 【核心回滚】冲突或异常，关闭文件、销毁文件、抛错
        index_manager_->close_index(ix.get());
        index_manager_->destroy_index(tab_name, index_meta.cols);
        throw;
    }

    // 5. 成功后更新内存元数据和句柄
    indexes_.emplace(ix_name, std::move(ix));
    
    TabMeta& mutable_tab = db_.tabs_[tab_name];
    mutable_tab.indexes.push_back(index_meta);
    flush_meta();
}

void SmManager::drop_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context) {
    // TODO(Lab 3): 关闭并删除索引，同时维护索引元数据。
    std::string ix_name = index_manager_->make_index_name(tab_name, col_names);
    
    auto it = indexes_.find(ix_name);
    if (it == indexes_.end()) {
        throw RMDBError("Index does not exist: " + ix_name);
    }

    // 1. 关闭句柄，删除物理文件
    index_manager_->close_index(it->second.get());
    indexes_.erase(it);
    index_manager_->destroy_index(tab_name, col_names);

    // 2. 从元数据中移除
    TabMeta& tab = db_.tabs_[tab_name];
    auto& idxs = tab.indexes;
    idxs.erase(std::remove_if(idxs.begin(), idxs.end(), [&](const IndexMeta& idx) {
        if (idx.cols.size() != col_names.size()) return false;
        for (size_t i = 0; i < col_names.size(); ++i) {
            if (idx.cols[i].name != col_names[i]) return false;
        }
        return true;
    }), idxs.end());

    // 3. 刷盘
    flush_meta();
}

void SmManager::drop_index(const std::string& tab_name, const std::vector<ColMeta>& cols, Context* context) {
    // TODO(Lab 3): 与按字段名删除索引的重载共享核心逻辑。

    // 框架本身就有接收 vector<ColMeta> 的 make_index_name 和 destroy_index
    // 我们提取出 col_names 然后转调上面的函数即可
    std::vector<std::string> col_names;
    col_names.reserve(cols.size());
    for (const auto& col : cols) {
        col_names.push_back(col.name);
    }
    drop_index(tab_name, col_names, context);
}
