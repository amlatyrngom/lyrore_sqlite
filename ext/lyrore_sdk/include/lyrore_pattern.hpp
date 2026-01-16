#ifndef LYRORE_PATTERN_HPP
#define LYRORE_PATTERN_HPP

#include "lyrore_plugin.hpp"
#include <string>
#include <map>
#include <vector>
#include <memory>
#include <optional>
#include <variant>
#include <atomic>

// Forward declarations for SQLite types
struct Select;
struct Expr;
struct SrcList;
struct ExprList;

namespace lyrore {
namespace pattern {

class Pattern;
using PatternPtr = std::shared_ptr<Pattern>;

/**
 * Thread-local state for pattern AST capture.
 * Used internally during Pattern::from_query() lazy initialization.
 */
struct PatternCaptureState {
    static thread_local bool capture_mode;
    static thread_local Select* captured_select_dup;  // OWNED dupped copy, valid after hook
    static thread_local sqlite3* capture_db;
};

/**
 * Result of a pattern match operation.
 */
class MatchResult {
public:
    MatchResult() = default;

    operator bool() const { return matched_; }
    bool matched() const { return matched_; }

    template<typename T>
    T get(const char* param) const;

    LyExprPtr get_expr(const char* param) const;

    const std::map<std::string, LyValue>& params() const { return params_; }
    const std::map<std::string, LyExprPtr>& expr_params() const { return expr_params_; }

    void set_matched(bool m) { matched_ = m; }
    void set_param(const std::string& name, const LyValue& val) { params_[name] = val; }
    void set_expr_param(const std::string& name, const LyExprPtr& expr) { expr_params_[name] = expr; }

    int& auto_param_index() { return auto_param_index_; }

private:
    bool matched_ = false;
    std::map<std::string, LyValue> params_;
    std::map<std::string, LyExprPtr> expr_params_;
    int auto_param_index_ = 0;
};

// Template specializations declared
template<> int64_t MatchResult::get<int64_t>(const char* param) const;
template<> double MatchResult::get<double>(const char* param) const;
template<> std::string MatchResult::get<std::string>(const char* param) const;
template<> int MatchResult::get<int>(const char* param) const;

/**
 * Initialization state for non-blocking lazy init.
 */
enum class InitState : int {
    NOT_STARTED = 0,
    IN_PROGRESS = 1,
    COMPLETED = 2
};

/**
 * SQL-based pattern for matching query structures.
 * Uses SQLite's own parser for pattern specification.
 */
class Pattern {
public:
    Pattern();
    ~Pattern();

    // Non-copyable, moveable
    Pattern(const Pattern&) = delete;
    Pattern& operator=(const Pattern&) = delete;
    Pattern(Pattern&& other) noexcept;
    Pattern& operator=(Pattern&& other) noexcept;

    /**
     * Create pattern from full query SQL.
     * Uses LAZY initialization - actual parsing deferred to first match().
     */
    static PatternPtr from_query(sqlite3* db, const char* sql);

    /**
     * Create pattern from WHERE expression via wrapper query.
     * Uses LAZY initialization - actual parsing deferred to first match().
     */
    static PatternPtr from_where_expr(sqlite3* db, const char* sql);

    /**
     * Match against a Select* (full query match).
     */
    MatchResult match(const Select* pSelect) const;

    /**
     * Match against an expression (WHERE clause match).
     */
    MatchResult match_expr(const Expr* pExpr) const;
    MatchResult match_expr(const LyExprPtr& expr) const;

    /**
     * Get number of parameters in pattern.
     */
    int num_params() const;

    /**
     * Check if pattern is valid (initialized successfully).
     * Safe to call during recursive pattern init - returns false if init in progress.
     */
    bool is_valid() const;

    /**
     * Debug string representation.
     */
    std::string debug_string() const;

private:
    // Lazy initialization support
    sqlite3* db_ = nullptr;
    std::string pending_sql_;
    mutable std::atomic<InitState> init_state_{InitState::NOT_STARTED};
    mutable bool init_failed_ = false;

    // Pattern AST storage (set during lazy init)
    mutable Select* pattern_select_ = nullptr;
    mutable Expr* pattern_where_ = nullptr;
    mutable std::vector<Expr*> param_positions_;
    bool is_where_only_ = false;

    // Lazy initialization - returns true if init was performed (or already done)
    bool try_initialize() const;
    void do_initialize() const;

    // Internal matching methods
    bool matchSelect(const Select* pPattern, const Select* pQuery, MatchResult& result) const;
    bool matchFromClause(const SrcList* pPattern, const SrcList* pQuery, MatchResult& result) const;
    bool matchExprList(const ExprList* pPattern, const ExprList* pQuery, MatchResult& result) const;
    bool matchExpr(const Expr* pPattern, const Expr* pQuery, MatchResult& result) const;
    bool extractParameter(const Expr* pPattern, const Expr* pQuery, MatchResult& result) const;

    // Parameter indexing
    void indexParameters() const;
    void indexParametersInExpr(Expr* pExpr) const;
    void indexParametersInSelect(Select* pSelect) const;
};

/**
 * Check if we're currently inside pattern initialization.
 * Used to prevent recursive pattern matching during init.
 */
bool in_pattern_init();

} // namespace pattern
} // namespace lyrore

#endif /* LYRORE_PATTERN_HPP */
