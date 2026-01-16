/*
** Lyrore Pattern Engine Implementation
** Key fix: Uses atomic state instead of std::call_once to prevent deadlock
** during recursive pattern initialization.
*/

#include "lyrore_pattern.hpp"
#include "lyrore_cabi.h"
#include <cstring>
#include <cstdio>
#include <iostream>
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
    
    // If initialization is in progress (on this thread via recursion), don't block
    if (state == InitState::IN_PROGRESS) {
        return false;
    }
    
    // Try to start initialization
    InitState expected = InitState::NOT_STARTED;
    if (init_state_.compare_exchange_strong(expected, InitState::IN_PROGRESS,
                                            std::memory_order_acq_rel)) {
        // We won the race, do the actual initialization
        do_initialize();
        init_state_.store(InitState::COMPLETED, std::memory_order_release);
        return true;
    }
    
    // Another thread is initializing, spin-wait (shouldn't happen in single-threaded SQLite)
    while (init_state_.load(std::memory_order_acquire) == InitState::IN_PROGRESS) {
        // Yield to prevent busy spinning
        std::this_thread::yield();
    }
    return init_state_.load(std::memory_order_acquire) == InitState::COMPLETED;
}

void Pattern::do_initialize() const {
    if (pending_sql_.empty() || !db_) {
        init_failed_ = true;
        return;
    }

    // Recursion guard (extra safety)
    if (pattern_init_depth > 5) {
        init_failed_ = true;
        return;
    }

    pattern_init_depth++;

    // Set up capture mode - the hook will do sqlite3SelectDup immediately
    PatternCaptureState::capture_mode = true;
    PatternCaptureState::captured_select_dup = nullptr;
    PatternCaptureState::capture_db = db_;

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, pending_sql_.c_str(), -1, &stmt, nullptr);

    // Get the already-dupped Select* from capture state
    Select* dupped = PatternCaptureState::captured_select_dup;

    // Clear capture state
    PatternCaptureState::capture_mode = false;
    PatternCaptureState::captured_select_dup = nullptr;
    PatternCaptureState::capture_db = nullptr;

    pattern_init_depth--;

    if (stmt) sqlite3_finalize(stmt);

    if (rc != SQLITE_OK || !dupped) {
        // If we got a dupped select but prepare failed, clean it up
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
    // If we're inside pattern init (recursive call), return false without blocking
    if (in_pattern_init()) {
        return false;
    }
    
    // Try to ensure initialized
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
    // Check if x is a subquery (IN/EXISTS/etc) or argument list
    if (ExprUseXSelect(pExpr)) {
        // x.pSelect is valid - recurse into subquery
        if (pExpr->x.pSelect) {
            indexParametersInSelect(pExpr->x.pSelect);
        }
    } else if (pExpr->x.pList) {
        // x.pList is valid - iterate function arguments
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
    if (pSelect->pSrc) {
        for (int i = 0; i < pSelect->pSrc->nSrc; i++) {
            // Access subquery via fg.isSubquery and u4.pSubq
            SrcItem* pItem = &pSelect->pSrc->a[i];
            if (pItem->fg.isSubquery && pItem->u4.pSubq && pItem->u4.pSubq->pSelect) {
                indexParametersInSelect(pItem->u4.pSubq->pSelect);
            }
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
    // Don't block during init
    if (in_pattern_init()) return 0;
    if (!try_initialize()) return 0;
    return static_cast<int>(param_positions_.size());
}

std::string Pattern::debug_string() const {
    if (in_pattern_init()) return "<pattern init in progress>";
    if (!try_initialize() || init_failed_) return "<invalid pattern>";

    std::string s = "Pattern{sql=\"" + pending_sql_ + "\", params=" + 
                    std::to_string(param_positions_.size()) + "}";
    return s;
}

// ===== Matching Implementation =====

MatchResult Pattern::match(const Select* pQuery) const {
    MatchResult result;

    // Skip matching during pattern initialization to prevent recursion
    if (in_pattern_init()) {
        return result;
    }

    // Try to initialize - returns false if init is in progress (shouldn't happen here)
    if (!try_initialize()) {
        return result;
    }
    
    if (init_failed_ || !pattern_select_ || !pQuery) {
        return result;
    }

    if (matchSelect(pattern_select_, pQuery, result)) {
        result.set_matched(true);
    }
    return result;
}

MatchResult Pattern::match_expr(const Expr* pExpr) const {
    MatchResult result;

    if (in_pattern_init()) {
        return result;
    }

    if (!try_initialize()) {
        return result;
    }
    
    if (init_failed_ || !pattern_where_ || !pExpr) {
        return result;
    }

    if (matchExpr(pattern_where_, pExpr, result)) {
        result.set_matched(true);
    }
    return result;
}

MatchResult Pattern::match_expr(const LyExprPtr& expr) const {
    MatchResult result;
    if (in_pattern_init()) return result;
    if (!try_initialize()) return result;
    if (init_failed_ || !expr) return result;

    // Use the original sqlite Expr* stored in LyExpr
    if (expr->sqlite_expr) {
        if (matchExpr(pattern_where_, expr->sqlite_expr, result)) {
            result.set_matched(true);
        }
    }
    return result;
}

bool Pattern::matchSelect(const Select* pPattern, const Select* pQuery, MatchResult& result) const {
    if (!pPattern && !pQuery) return true;
    if (!pPattern || !pQuery) return false;

    // Match FROM clause
    if (!matchFromClause(pPattern->pSrc, pQuery->pSrc, result)) return false;

    // Match SELECT list
    if (!matchExprList(pPattern->pEList, pQuery->pEList, result)) return false;

    // Match WHERE clause
    if (!matchExpr(pPattern->pWhere, pQuery->pWhere, result)) return false;

    // Match GROUP BY
    if (!matchExprList(pPattern->pGroupBy, pQuery->pGroupBy, result)) return false;

    // Match HAVING
    if (!matchExpr(pPattern->pHaving, pQuery->pHaving, result)) return false;

    // Match ORDER BY
    if (!matchExprList(pPattern->pOrderBy, pQuery->pOrderBy, result)) return false;

    // Match LIMIT
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

        // Table identity via schema pointer (pSTab in current SQLite)
        if (pP->pSTab != pQ->pSTab) return false;

        // Join type must match
        if (pP->fg.jointype != pQ->fg.jointype) return false;

        // Recursively match subqueries (via fg.isSubquery and u4.pSubq)
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
    // NULL pattern matches NULL query
    if (!pPattern && !pQuery) return true;
    if (!pPattern || !pQuery) return false;

    // Parameter extraction: TK_VARIABLE in pattern matches any expression in query
    if (pPattern->op == TK_VARIABLE) {
        return extractParameter(pPattern, pQuery, result);
    }

    // Operator must match
    if (pPattern->op != pQuery->op) return false;

    // Column matching: use schema pointers (y.pTab + iColumn)
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
    if (patternIsSelect != queryIsSelect) return false;  // Structure mismatch
    
    if (patternIsSelect) {
        // Both have subqueries - match them
        if (!matchSelect(pPattern->x.pSelect, pQuery->x.pSelect, result)) return false;
    } else {
        // Both have argument lists (or neither)
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
            // Non-literal: capture as expression
            result.set_expr_param(param_name, LyExpr::from_sqlite(const_cast<Expr*>(pQuery)));
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

LyExprPtr MatchResult::get_expr(const char* param) const {
    auto it = expr_params_.find(param);
    if (it == expr_params_.end()) return nullptr;
    return it->second;
}

} // namespace pattern
} // namespace lyrore

// ===== C Interface for Capture Hook =====
extern "C" {

// Called from PreOpt hook in cpp_context.cpp
// This is called DURING prepare, while the Select* is still valid
void lyrore_pattern_capture_preopt(void* pSelect) {
    using namespace lyrore::pattern;

    if (!PatternCaptureState::capture_mode || !pSelect || !PatternCaptureState::capture_db) {
        return;
    }

    // Already captured something? Don't capture again
    if (PatternCaptureState::captured_select_dup) {
        return;
    }

    // CRITICAL: Dup the Select* NOW while it's still valid
    // After prepare returns, the original may be freed
    Select* pSel = static_cast<Select*>(pSelect);
    sqlite3* db = PatternCaptureState::capture_db;

    PatternCaptureState::captured_select_dup = lyrore_sqlite3SelectDup(db, pSel, 0);
}

} // extern "C"
