/*
** Lyrore TableCursor Implementation
** 
** Provides read-only table iteration using prepared statements.
** This approach is safer and more stable than raw BtCursor access.
*/

#include "lyrore_custom_op.hpp"

extern "C" {
#include "sqlite3.h"
}

namespace lyrore {

// ============================================================
// TableCursor Implementation
// ============================================================

TableCursor::TableCursor() 
    : db_(nullptr), stmt_(nullptr), at_end_(true), num_cols_(0) {}

TableCursor::TableCursor(sqlite3* db, const char* table_name)
    : db_(db), stmt_(nullptr), at_end_(false), num_cols_(0) {

    if (!db || !table_name) {
        at_end_ = true;
        return;
    }

    // Build SELECT * FROM table query
    std::string sql = "SELECT * FROM \"";
    sql += table_name;
    sql += "\"";

    int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt_, nullptr);
    if (rc != SQLITE_OK || !stmt_) {
        stmt_ = nullptr;
        at_end_ = true;
        return;
    }

    num_cols_ = sqlite3_column_count(stmt_);

    // Step to first row (if any) - so cursor is immediately ready to read
    rc = sqlite3_step(stmt_);
    if (rc != SQLITE_ROW) {
        at_end_ = true;
    }
}

TableCursor::~TableCursor() {
    if (stmt_) {
        sqlite3_finalize(stmt_);
        stmt_ = nullptr;
    }
}

TableCursor::TableCursor(TableCursor&& other) noexcept
    : db_(other.db_)
    , stmt_(other.stmt_)
    , at_end_(other.at_end_)
    , num_cols_(other.num_cols_) {
    other.stmt_ = nullptr;
    other.at_end_ = true;
}

TableCursor& TableCursor::operator=(TableCursor&& other) noexcept {
    if (this != &other) {
        if (stmt_) {
            sqlite3_finalize(stmt_);
        }
        db_ = other.db_;
        stmt_ = other.stmt_;
        at_end_ = other.at_end_;
        num_cols_ = other.num_cols_;
        other.stmt_ = nullptr;
        other.at_end_ = true;
    }
    return *this;
}

bool TableCursor::valid() const {
    return stmt_ != nullptr;
}

bool TableCursor::eof() const {
    return at_end_;
}

bool TableCursor::next() {
    if (!stmt_ || at_end_) {
        return false;
    }
    
    int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) {
        return true;
    }
    
    at_end_ = true;
    return false;
}

void TableCursor::reset() {
    if (stmt_) {
        sqlite3_reset(stmt_);
        at_end_ = false;
    }
}

LyValue TableCursor::get_column(int col) {
    if (!stmt_ || at_end_ || col < 0 || col >= num_cols_) {
        return LyValue{};  // monostate = NULL
    }
    
    int type = sqlite3_column_type(stmt_, col);
    switch (type) {
        case SQLITE_INTEGER:
            return LyValue{static_cast<int64_t>(sqlite3_column_int64(stmt_, col))};
        
        case SQLITE_FLOAT:
            return LyValue{sqlite3_column_double(stmt_, col)};
        
        case SQLITE_TEXT: {
            const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt_, col));
            return LyValue{std::string(text ? text : "")};
        }
        
        case SQLITE_BLOB: {
            const void* blob = sqlite3_column_blob(stmt_, col);
            int size = sqlite3_column_bytes(stmt_, col);
            if (blob && size > 0) {
                const uint8_t* data = static_cast<const uint8_t*>(blob);
                return LyValue{std::vector<uint8_t>(data, data + size)};
            }
            return LyValue{std::vector<uint8_t>{}};
        }
        
        case SQLITE_NULL:
        default:
            return LyValue{};  // monostate = NULL
    }
}

std::vector<LyValue> TableCursor::get_row() {
    std::vector<LyValue> row;
    if (!stmt_ || at_end_) {
        return row;
    }
    
    row.reserve(num_cols_);
    for (int i = 0; i < num_cols_; i++) {
        row.push_back(get_column(i));
    }
    return row;
}

int64_t TableCursor::get_rowid() {
    // Note: For "SELECT *", the rowid is typically not included unless it's
    // a WITHOUT ROWID table. We'll return -1 if we can't determine it.
    // A more sophisticated implementation could use sqlite3_column_int64(stmt_, -1)
    // but that's not standard behavior.
    // For simplicity, we return -1 indicating rowid is not directly available.
    return -1;
}

int TableCursor::num_columns() const {
    return num_cols_;
}


// ============================================================
// CustomOperator Helper Methods
// ============================================================

TableCursor CustomOperator::open_table(const char* table_name) {
    return TableCursor(db_, table_name);
}

std::unique_ptr<ResultSet> CustomOperator::query(const char* sql) {
    if (!db_ || !sql) return nullptr;
    
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK || !stmt) {
        return nullptr;
    }
    return std::make_unique<ResultSet>(stmt);
}


// ============================================================
// LyValue <-> sqlite3_value/result Conversion
// ============================================================

LyValue sqlite3_value_to_lyvalue(void* value) {
    sqlite3_value* val = static_cast<sqlite3_value*>(value);
    if (!val) return LyValue{};
    
    int type = sqlite3_value_type(val);
    switch (type) {
        case SQLITE_INTEGER:
            return LyValue{static_cast<int64_t>(sqlite3_value_int64(val))};
        
        case SQLITE_FLOAT:
            return LyValue{sqlite3_value_double(val)};
        
        case SQLITE_TEXT: {
            const char* text = reinterpret_cast<const char*>(sqlite3_value_text(val));
            return LyValue{std::string(text ? text : "")};
        }
        
        case SQLITE_BLOB: {
            const void* blob = sqlite3_value_blob(val);
            int size = sqlite3_value_bytes(val);
            if (blob && size > 0) {
                const uint8_t* data = static_cast<const uint8_t*>(blob);
                return LyValue{std::vector<uint8_t>(data, data + size)};
            }
            return LyValue{std::vector<uint8_t>{}};
        }
        
        case SQLITE_NULL:
        default:
            return LyValue{};
    }
}

void set_sqlite3_result(void* ctx, const LyValue& val) {
    sqlite3_context* context = static_cast<sqlite3_context*>(ctx);
    if (!context) return;
    
    std::visit([context](auto&& arg) {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
            sqlite3_result_null(context);
        } else if constexpr (std::is_same_v<T, int64_t>) {
            sqlite3_result_int64(context, arg);
        } else if constexpr (std::is_same_v<T, double>) {
            sqlite3_result_double(context, arg);
        } else if constexpr (std::is_same_v<T, std::string>) {
            sqlite3_result_text(context, arg.c_str(), -1, SQLITE_TRANSIENT);
        } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
            sqlite3_result_blob(context, arg.data(), arg.size(), SQLITE_TRANSIENT);
        }
    }, val);
}

} // namespace lyrore
