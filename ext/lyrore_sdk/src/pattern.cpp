/*
** Lyrore Pattern Engine Implementation
** 
** Provides SQL-based pattern matching for query templates.
** Uses SQLite's parser for pattern specification.
*/

#include "lyrore_pattern.hpp"
#include "lyrore_cabi.h"
#include <cstring>
#include <cstdio>
#include <thread>

extern "C" {
#include "sqliteInt.h"
}

namespace lyrore {
namespace pattern {

// Thread-local capture state
thread_local bool PatternCaptureState::capture_mode = false;
thread_local Select* PatternCaptureState::captured_select_dup = nullptr;
thread_local sqlite3* PatternCaptureState::capture_db = nullptr;

// Recursion depth guard
static thread_local int pattern_init_depth = 0;

bool in_pattern_init() {
    return pattern_init_depth > 0;
}

// ===== Pattern Implementation =====

Pattern::Pattern() = default;

Pattern::~Pattern() {
    if (pattern_select_ && db_) {
        lyrore_sqlite3SelectDelete(db_, pattern_select_);
        pattern_select_ = nullptr;
    }
}

Pattern::Pattern(Pattern&& other) noexcept 
    : db_(other.db_),
      pending_sql_(std::move(other.pending_sql_)),
      init_state_(other.init_state_.load()),
      init_failed_(other.init_failed_),
      pattern_select_(other.pattern_select_),
      pattern_where_(other.pattern_where_),
      param_positions_(std::move(other.param_positions_)),
      is_where_only_(other.is_where_only_) {
    other.pattern_select_ = nullptr;
    other.pattern_where_ = nullptr;
    other.db_ = nullptr;
    other.init_state_ = InitState::NOT_STARTED;
}

Pattern& Pattern::operator=(Pattern&& other) noexcept {
    if (this != &other) {
        if (pattern_select_ && db_) {
            lyrore_sqlite3SelectDelete(db_, pattern_select_);
        }
        db_ = other.db_;
        pending_sql_ = std::move(other.pending_sql_);
        init_state_.store(other.init_state_.load());
        init_failed_ = other.init_failed_;
        pattern_select_ = other.pattern_select_;
        pattern_where_ = other.pattern_where_;
        param_positions_ = std::move(other.param_positions_);
        is_where_only_ = other.is_where_only_;
        other.pattern_select_ = nullptr;
        other.pattern_where_ = nullptr;
        other.db_ = nullptr;
        other.init_state_ = InitState::NOT_STARTED;
    }
    return *this;
}

PatternPtr Pattern::from_query(sqlite3* db, const char* sql) {
    if (!db || !sql) return nullptr;
    auto pattern = std::shared_ptr<Pattern>(new Pattern());
    pattern->db_ = db;
    pattern->pending_sql_ = sql;
    pattern->is_where_only_ = false;
    return pattern;
}

PatternPtr Pattern::from_where_expr(sqlite3* db, const char* sql) {
    if (!db || !sql) return nullptr;
    auto pattern = std::shared_ptr<Pattern>(new Pattern());
    pattern->db_ = db;
    pattern->pending_sql_ = sql;
    pattern->is_where_only_ = true;
    return pattern;
}

bool Pattern::try_initialize() const {
    // Fast path: already completed
    InitState state = init_state_.load(std::memory_order_acquire);
    if (state == InitState::COMPLETED) {
        return true;
    }

    // If initialization is in progress (recursive), don't block
    if (state == InitState::IN_PROGRESS) {
        return false;
    }

    // Try to start initialization
    InitState expected = InitState::NOT_STARTED;
    if (init_state_.compare_exchange_strong(expected, InitState::IN_PROGRESS,
                                            std::memory_order_acq_rel)) {
        do_initialize();
        init_state_.store(InitState::COMPLETED, std::memory_order_release);
        return true;
    }

    // Another thread is initializing, spin-wait
    while (init_state_.load(std::memory_order_acquire) == InitState::IN_PROGRESS) {
        std::this_thread::yield();
    }
    return init_state_.load(std::memory_order_acquire) == InitState::COMPLETED;
}

void Pattern::do_initialize() const {
    if (pending_sql_.empty() || !db_) {
        init_failed_ = true;
        return;
    }

    // Recursion guard
    if (pattern_init_depth > 5) {
        init_failed_ = true;
        return;
    }

    pattern_init_depth++;

    // Set up capture mode
    PatternCaptureState::capture_mode = true;
    PatternCaptureState::captured_select_dup = nullptr;
    PatternCaptureState::capture_db = db_;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, pending_sql_.c_str(), -1, &stmt, nullptr);

    // Get the dupped Select* from capture state
    Select* dupped = PatternCaptureState::captured_select_dup;

    // Clear capture state
    PatternCaptureState::capture_mode = false;
    PatternCaptureState::captured_select_dup = nullptr;
    PatternCaptureState::capture_db = nullptr;

    pattern_init_depth--;

    if (stmt) sqlite3_finalize(stmt);

    if (rc != SQLITE_OK || !dupped) {
        if (dupped && db_) {
            lyrore_sqlite3SelectDelete(db_, dupped);
        }
        init_failed_ = true;
        return;
    }

    // Take ownership of the dupped Select*
    pattern_select_ = dupped;
    pattern_where_ = pattern_select_->pWhere;
    indexParameters();

    init_failed_ = false;
}

bool Pattern::is_valid() const {
    if (in_pattern_init()) {
        return false;
    }

    if (!try_initialize()) {
        return false;
    }

    return !init_failed_ && pattern_select_ != nullptr;
}

void Pattern::indexParameters() const {
    param_positions_.clear();
    if (pattern_select_) {
        indexParametersInSelect(pattern_select_);
    }
}

void Pattern::indexParametersInExpr(Expr* pExpr) const {
    if (!pExpr) return;
    if (pExpr->op == TK_VARIABLE) {
        param_positions_.push_back(pExpr);
    }
    indexParametersInExpr(pExpr->pLeft);
    indexParametersInExpr(pExpr->pRight);

    if (ExprUseXSelect(pExpr)) {
        if (pExpr->x.pSelect) {
            indexParametersInSelect(pExpr->x.pSelect);
        }
    } else if (pExpr->x.pList) {
        for (int i = 0; i < pExpr->x.pList->nExpr; i++) {
            indexParametersInExpr(pExpr->x.pList->a[i].pExpr);
        }
    }
}

void Pattern::indexParametersInSelect(Select* pSelect) const {
    if (!pSelect) return;
    indexParametersInExpr(pSelect->pWhere);
    if (pSelect->pEList) {
        for (int i = 0; i < pSelect->pEList->nExpr; i++) {
            indexParametersInExpr(pSelect->pEList->a[i].pExpr);
        }
    }
    if (pSelect->pGroupBy) {
        for (int i = 0; i < pSelect->pGroupBy->nExpr; i++) {
            indexParametersInExpr(pSelect->pGroupBy->a[i].pExpr);
        }
    }
    indexParametersInExpr(pSelect->pHaving);
    if (pSelect->pOrderBy) {
        for (int i = 0; i < pSelect->pOrderBy->nExpr; i++) {
            indexParametersInExpr(pSelect->pOrderBy->a[i].pExpr);
        }
    }
    indexParametersInExpr(pSelect->pLimit);
}

int Pattern::num_params() const {
    if (!is_valid()) return 0;
    return static_cast<int>(param_positions_.size());
}

std::string Pattern::debug_string() const {
    if (!is_valid()) return "<invalid pattern>";
    return pending_sql_;
}

MatchResult Pattern::match(const Select* pSelect) const {
    MatchResult result;
    if (!is_valid() || !pSelect) return result;

    if (matchSelect(pattern_select_, pSelect, result)) {
        result.set_matched(true);
    }
    return result;
}

MatchResult Pattern::match_expr(const Expr* pExpr) const {
    MatchResult result;
    if (!is_valid() || !pExpr) return result;

    // For WHERE-only patterns, match against pattern's WHERE clause
    if (is_where_only_ && pattern_where_) {
        if (matchExpr(pattern_where_, pExpr, result)) {
            result.set_matched(true);
        }
    }
    return result;
}

bool Pattern::matchSelect(const Select* pPattern, const Select* pQuery, MatchResult& result) const {
    if (!pPattern && !pQuery) return true;
    if (!pPattern || !pQuery) return false;

    if (!matchFromClause(pPattern->pSrc, pQuery->pSrc, result)) return false;
    if (!matchExprList(pPattern->pEList, pQuery->pEList, result)) return false;
    if (!matchExpr(pPattern->pWhere, pQuery->pWhere, result)) return false;
    if (!matchExprList(pPattern->pGroupBy, pQuery->pGroupBy, result)) return false;
    if (!matchExpr(pPattern->pHaving, pQuery->pHaving, result)) return false;
    if (!matchExprList(pPattern->pOrderBy, pQuery->pOrderBy, result)) return false;
    if (!matchExpr(pPattern->pLimit, pQuery->pLimit, result)) return false;

    return true;
}

bool Pattern::matchFromClause(const SrcList* pPattern, const SrcList* pQuery, MatchResult& result) const {
    if (!pPattern && !pQuery) return true;
    if (!pPattern || !pQuery) return false;
    if (pPattern->nSrc != pQuery->nSrc) return false;

    for (int i = 0; i < pPattern->nSrc; i++) {
        const SrcItem* pP = &pPattern->a[i];
        const SrcItem* pQ = &pQuery->a[i];

        // Table identity via schema pointer
        if (pP->pSTab != pQ->pSTab) return false;

        // Join type must match
        if (pP->fg.jointype != pQ->fg.jointype) return false;

        // Recursively match subqueries
        bool pHasSub = pP->fg.isSubquery && pP->u4.pSubq && pP->u4.pSubq->pSelect;
        bool qHasSub = pQ->fg.isSubquery && pQ->u4.pSubq && pQ->u4.pSubq->pSelect;
        if (pHasSub || qHasSub) {
            if (pHasSub != qHasSub) return false;
            if (pHasSub && !matchSelect(pP->u4.pSubq->pSelect, pQ->u4.pSubq->pSelect, result)) {
                return false;
            }
        }
    }
    return true;
}

bool Pattern::matchExprList(const ExprList* pPattern, const ExprList* pQuery, MatchResult& result) const {
    if (!pPattern && !pQuery) return true;
    if (!pPattern || !pQuery) return false;
    if (pPattern->nExpr != pQuery->nExpr) return false;

    for (int i = 0; i < pPattern->nExpr; i++) {
        if (!matchExpr(pPattern->a[i].pExpr, pQuery->a[i].pExpr, result)) return false;
    }
    return true;
}

bool Pattern::matchExpr(const Expr* pPattern, const Expr* pQuery, MatchResult& result) const {
    if (!pPattern && !pQuery) return true;
    if (!pPattern || !pQuery) return false;

    // Parameter extraction: TK_VARIABLE matches any expression
    if (pPattern->op == TK_VARIABLE) {
        return extractParameter(pPattern, pQuery, result);
    }

    // Operator must match
    if (pPattern->op != pQuery->op) return false;

    // Column matching: use schema pointers
    if (pPattern->op == TK_COLUMN) {
        return (pPattern->y.pTab == pQuery->y.pTab &&
                pPattern->iColumn == pQuery->iColumn);
    }

    // Function: name must match
    if (pPattern->op == TK_FUNCTION || pPattern->op == TK_AGG_FUNCTION) {
        if (!pPattern->u.zToken || !pQuery->u.zToken) return false;
        if (lyrore_sqlite3StrICmp(pPattern->u.zToken, pQuery->u.zToken) != 0) return false;
    }

    // Literal values must match
    if (pPattern->op == TK_INTEGER) {
        if (pPattern->u.iValue != pQuery->u.iValue) return false;
    }
    if (pPattern->op == TK_STRING) {
        if (!pPattern->u.zToken || !pQuery->u.zToken) return false;
        if (strcmp(pPattern->u.zToken, pQuery->u.zToken) != 0) return false;
    }

    // Recursively match children
    if (!matchExpr(pPattern->pLeft, pQuery->pLeft, result)) return false;
    if (!matchExpr(pPattern->pRight, pQuery->pRight, result)) return false;

    // Match argument lists or subqueries
    bool patternIsSelect = ExprUseXSelect(pPattern);
    bool queryIsSelect = ExprUseXSelect(pQuery);
    if (patternIsSelect != queryIsSelect) return false;

    if (patternIsSelect) {
        if (!matchSelect(pPattern->x.pSelect, pQuery->x.pSelect, result)) return false;
    } else {
        if (!matchExprList(pPattern->x.pList, pQuery->x.pList, result)) return false;
    }

    return true;
}

bool Pattern::extractParameter(const Expr* pPattern, const Expr* pQuery, MatchResult& result) const {
    std::string param_name;
    if (pPattern->u.zToken) {
        param_name = pPattern->u.zToken;
    } else {
        param_name = "?";
    }

    // Normalize "?" to "$1", "$2", etc.
    if (param_name == "?" || param_name.empty()) {
        param_name = "$" + std::to_string(++result.auto_param_index());
    } else if (param_name[0] == '?') {
        param_name = "$" + param_name.substr(1);
    }

    // Extract value based on query expression type
    LyValue val;
    switch (pQuery->op) {
        case TK_INTEGER:
            val = (int64_t)pQuery->u.iValue;
            result.set_param(param_name, val);
            return true;
        case TK_FLOAT:
            if (pQuery->u.zToken) {
                val = strtod(pQuery->u.zToken, nullptr);
                result.set_param(param_name, val);
            }
            return true;
        case TK_STRING:
            if (pQuery->u.zToken) {
                val = std::string(pQuery->u.zToken);
                result.set_param(param_name, val);
            }
            return true;
        case TK_NULL:
            val = std::monostate{};
            result.set_param(param_name, val);
            return true;
        default:
            // Non-literal expressions: just indicate match succeeded
            // The caller should use raw Expr* access if they need the expression
            return true;
    }
}

// ===== MatchResult Template Specializations =====

template<>
int64_t MatchResult::get<int64_t>(const char* param) const {
    auto it = params_.find(param);
    if (it == params_.end()) return 0;
    if (auto* v = std::get_if<int64_t>(&it->second)) return *v;
    if (auto* v = std::get_if<double>(&it->second)) return static_cast<int64_t>(*v);
    return 0;
}

template<>
double MatchResult::get<double>(const char* param) const {
    auto it = params_.find(param);
    if (it == params_.end()) return 0.0;
    if (auto* v = std::get_if<double>(&it->second)) return *v;
    if (auto* v = std::get_if<int64_t>(&it->second)) return static_cast<double>(*v);
    return 0.0;
}

template<>
std::string MatchResult::get<std::string>(const char* param) const {
    auto it = params_.find(param);
    if (it == params_.end()) return "";
    if (auto* v = std::get_if<std::string>(&it->second)) return *v;
    return "";
}

template<>
int MatchResult::get<int>(const char* param) const {
    return static_cast<int>(get<int64_t>(param));
}

} // namespace pattern
} // namespace lyrore

// ===== C Interface for Capture Hook =====
extern "C" {

void lyrore_pattern_capture_preopt(void* pSelect) {
    using namespace lyrore::pattern;

    if (!PatternCaptureState::capture_mode || !pSelect || !PatternCaptureState::capture_db) {
        return;
    }

    if (PatternCaptureState::captured_select_dup) {
        return;
    }

    Select* pSel = static_cast<Select*>(pSelect);
    sqlite3* db = PatternCaptureState::capture_db;

    PatternCaptureState::captured_select_dup = lyrore_sqlite3SelectDup(db, pSel, 0);
}

} // extern "C"
