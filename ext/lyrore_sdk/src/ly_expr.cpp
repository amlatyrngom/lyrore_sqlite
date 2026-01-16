/*
** Lyrore C++ Plugin SDK - LyExpr Implementation
** 
** This file implements the LyExpr class for SQLite AST manipulation.
** Critical focus: Memory-safe from_sqlite/to_sqlite conversions.
*/

#include "lyrore_plugin.hpp"
#include "lyrore_cabi.h"
#include <stdexcept>
#include <sstream>

namespace lyrore {

// ===== CONVERSION: from_sqlite =====

LyExprPtr LyExpr::from_sqlite(Expr* pExpr) {
    if (!pExpr) return nullptr;
    
    auto out = std::make_shared<LyExpr>();
    out->op = static_cast<LyOp>(pExpr->op);
    
    // Store original for column binding preservation
    out->sqlite_expr = pExpr;
    
    // Extract token/value based on expression type
    switch (pExpr->op) {
        case TK_INTEGER:
            // Check EP_IntValue flag to determine value storage
            if (pExpr->flags & EP_IntValue) {
                out->value = static_cast<int64_t>(pExpr->u.iValue);
            } else if (pExpr->u.zToken) {
                out->value = static_cast<int64_t>(std::stoll(pExpr->u.zToken));
                out->token = pExpr->u.zToken;
            }
            break;
            
        case TK_FLOAT:
            if (pExpr->u.zToken) {
                out->value = std::stod(pExpr->u.zToken);
                out->token = pExpr->u.zToken;
            }
            break;
            
        case TK_STRING:
            if (pExpr->u.zToken) {
                out->value = std::string(pExpr->u.zToken);
                out->token = pExpr->u.zToken;
            }
            break;
            
        case TK_BLOB:
            if (pExpr->u.zToken) {
                // Blob is stored as hex string with X prefix
                out->token = pExpr->u.zToken;
            }
            break;
            
        case TK_NULL:
            out->value = std::monostate{};
            break;
            
        case TK_COLUMN:
            out->column_index = pExpr->iColumn;
            out->table_cursor = pExpr->iTable;
            // Extract table name if available
            if (pExpr->y.pTab) {
                out->table_name = pExpr->y.pTab->zName ? pExpr->y.pTab->zName : "";
            }
            // Extract column name from token if available
            if (!(pExpr->flags & EP_IntValue) && pExpr->u.zToken) {
                out->column_name = pExpr->u.zToken;
            }
            break;
            
        case TK_FUNCTION:
        case TK_AGG_FUNCTION:
            // Function name is in token
            if (!(pExpr->flags & EP_IntValue) && pExpr->u.zToken) {
                out->token = pExpr->u.zToken;
            }
            // Function arguments are in x.pList
            if (pExpr->x.pList) {
                ExprList* pList = pExpr->x.pList;
                for (int i = 0; i < pList->nExpr; i++) {
                    auto arg = from_sqlite(pList->a[i].pExpr);
                    if (arg) out->args.push_back(arg);
                }
            }
            break;
            
        case TK_VARIABLE:
            if (!(pExpr->flags & EP_IntValue) && pExpr->u.zToken) {
                out->token = pExpr->u.zToken;
            }
            break;
            
        default:
            // For other tokens, get the token string if present
            if (!(pExpr->flags & EP_IntValue) && pExpr->u.zToken) {
                out->token = pExpr->u.zToken;
            }
            break;
    }
    
    // Recurse into children
    out->left = from_sqlite(pExpr->pLeft);
    out->right = from_sqlite(pExpr->pRight);
    
    return out;
}


// ===== CONVERSION: to_sqlite_new =====

Expr* LyExpr::to_sqlite_new(Parse* pParse) const {
    sqlite3* db = pParse->db;
    Expr* pNew = nullptr;
    
    int tk_op = static_cast<int>(op);
    
    switch (tk_op) {
        case TK_COLUMN:
            // CRITICAL: Use sqlite3ExprDup to preserve Table* binding
            if (sqlite_expr) {
                pNew = lyrore_sqlite3ExprDup(db, sqlite_expr, 0);
            } else {
                // Fallback: create column expr without binding (may fail)
                pNew = lyrore_sqlite3Expr(db, TK_COLUMN, column_name.c_str());
                if (pNew) {
                    pNew->iColumn = column_index;
                    pNew->iTable = table_cursor;
                }
            }
            break;
            
        case TK_INTEGER:
            if (std::holds_alternative<int64_t>(value)) {
                int64_t v = std::get<int64_t>(value);
                pNew = lyrore_sqlite3Expr(db, TK_INTEGER, nullptr);
                if (pNew) {
                    pNew->flags |= EP_IntValue;
                    pNew->u.iValue = static_cast<int>(v);
                }
            } else {
                pNew = lyrore_sqlite3Expr(db, TK_INTEGER, token.c_str());
            }
            break;
            
        case TK_FLOAT:
            pNew = lyrore_sqlite3Expr(db, TK_FLOAT, token.c_str());
            break;
            
        case TK_STRING:
            if (std::holds_alternative<std::string>(value)) {
                pNew = lyrore_sqlite3Expr(db, TK_STRING, std::get<std::string>(value).c_str());
            } else {
                pNew = lyrore_sqlite3Expr(db, TK_STRING, token.c_str());
            }
            break;
            
        case TK_NULL:
            pNew = lyrore_sqlite3Expr(db, TK_NULL, nullptr);
            break;
            
        case TK_FUNCTION:
        case TK_AGG_FUNCTION:
            {
                // Create function expression
                pNew = lyrore_sqlite3Expr(db, tk_op, token.c_str());
                if (!pNew) break;
                
                // Create argument list
                if (!args.empty()) {
                    ExprList* pList = nullptr;
                    for (const auto& arg : args) {
                        Expr* pArgExpr = arg->to_sqlite_new(pParse);
                        pList = lyrore_sqlite3ExprListAppend(pParse, pList, pArgExpr);
                    }
                    pNew->x.pList = pList;
                }
            }
            break;
            
        default:
            // Binary or unary operators
            pNew = lyrore_sqlite3Expr(db, tk_op, nullptr);
            break;
    }
    
    if (!pNew) return nullptr;
    
    // Recurse into children for non-function nodes
    if (tk_op != TK_FUNCTION && tk_op != TK_AGG_FUNCTION) {
        if (left) {
            pNew->pLeft = left->to_sqlite_new(pParse);
        }
        if (right) {
            pNew->pRight = right->to_sqlite_new(pParse);
        }
    }
    
    return pNew;
}


// ===== CONVERSION: to_sqlite (in-place) =====

void LyExpr::to_sqlite(Parse* pParse, Expr* pTarget) const {
    if (!pTarget) return;
    
    sqlite3* db = pParse->db;
    
    // Build new expression tree
    Expr* pNew = to_sqlite_new(pParse);
    if (!pNew) return;
    
    // Free OLD children of target
    lyrore_sqlite3ExprDelete(db, pTarget->pLeft);
    lyrore_sqlite3ExprDelete(db, pTarget->pRight);
    if (ExprUseXList(pTarget) && pTarget->x.pList) {
        lyrore_sqlite3ExprListDelete(db, pTarget->x.pList);
    }
    
    // Copy new structure into target, preserving target's address
    u8 old_op = pTarget->op;
    pTarget->op = pNew->op;
    pTarget->affExpr = pNew->affExpr;
    pTarget->flags = pNew->flags;
    pTarget->u = pNew->u;
    pTarget->pLeft = pNew->pLeft;
    pTarget->pRight = pNew->pRight;
    pTarget->x = pNew->x;
    pTarget->iTable = pNew->iTable;
    pTarget->iColumn = pNew->iColumn;
    pTarget->y = pNew->y;
    
    // CRITICAL: Detach children from pNew before freeing
    // to prevent double-free
    pNew->pLeft = nullptr;
    pNew->pRight = nullptr;
    pNew->x.pList = nullptr;
    pNew->x.pSelect = nullptr;
    pNew->y.pTab = nullptr;
    pNew->y.pWin = nullptr;
    
    // Free only the pNew wrapper (children now owned by pTarget)
    lyrore_sqlite3DbFree(db, pNew);
}


// ===== BUILDER HELPERS =====

LyExprPtr LyExpr::binary_op(LyOp op, LyExprPtr l, LyExprPtr r) {
    auto expr = std::make_shared<LyExpr>();
    expr->op = op;
    expr->left = l;
    expr->right = r;
    return expr;
}

LyExprPtr LyExpr::unary_op(LyOp op, LyExprPtr expr) {
    auto out = std::make_shared<LyExpr>();
    out->op = op;
    out->left = expr;
    return out;
}


// ===== ARITHMETIC BUILDERS =====

LyExprPtr LyExpr::add(LyExprPtr l, LyExprPtr r) {
    return binary_op(LyOp::ADD, l, r);
}

LyExprPtr LyExpr::subtract(LyExprPtr l, LyExprPtr r) {
    return binary_op(LyOp::SUB, l, r);
}

LyExprPtr LyExpr::multiply(LyExprPtr l, LyExprPtr r) {
    return binary_op(LyOp::MUL, l, r);
}

LyExprPtr LyExpr::divide(LyExprPtr l, LyExprPtr r) {
    return binary_op(LyOp::DIV, l, r);
}

LyExprPtr LyExpr::mod(LyExprPtr l, LyExprPtr r) {
    return binary_op(LyOp::MOD, l, r);
}


// ===== COMPARISON BUILDERS =====

LyExprPtr LyExpr::equals(LyExprPtr l, LyExprPtr r) {
    return binary_op(LyOp::EQ, l, r);
}

LyExprPtr LyExpr::not_equals(LyExprPtr l, LyExprPtr r) {
    return binary_op(LyOp::NE, l, r);
}

LyExprPtr LyExpr::less_than(LyExprPtr l, LyExprPtr r) {
    return binary_op(LyOp::LT, l, r);
}

LyExprPtr LyExpr::less_equal(LyExprPtr l, LyExprPtr r) {
    return binary_op(LyOp::LE, l, r);
}

LyExprPtr LyExpr::greater_than(LyExprPtr l, LyExprPtr r) {
    return binary_op(LyOp::GT, l, r);
}

LyExprPtr LyExpr::greater_equal(LyExprPtr l, LyExprPtr r) {
    return binary_op(LyOp::GE, l, r);
}


// ===== LOGICAL BUILDERS =====

LyExprPtr LyExpr::logical_and(LyExprPtr l, LyExprPtr r) {
    return binary_op(LyOp::AND, l, r);
}

LyExprPtr LyExpr::logical_or(LyExprPtr l, LyExprPtr r) {
    return binary_op(LyOp::OR, l, r);
}

LyExprPtr LyExpr::logical_not(LyExprPtr expr) {
    return unary_op(LyOp::NOT, expr);
}


// ===== LITERAL BUILDERS =====

LyExprPtr LyExpr::integer(int64_t val) {
    auto expr = std::make_shared<LyExpr>();
    expr->op = LyOp::INTEGER;
    expr->value = val;
    return expr;
}

LyExprPtr LyExpr::floating(double val) {
    auto expr = std::make_shared<LyExpr>();
    expr->op = LyOp::FLOAT;
    expr->value = val;
    expr->token = std::to_string(val);
    return expr;
}

LyExprPtr LyExpr::string(const std::string& val) {
    auto expr = std::make_shared<LyExpr>();
    expr->op = LyOp::STRING;
    expr->value = val;
    expr->token = val;
    return expr;
}

LyExprPtr LyExpr::null() {
    auto expr = std::make_shared<LyExpr>();
    expr->op = LyOp::NULL_VAL;
    return expr;
}


// ===== COLUMN BUILDER =====

LyExprPtr LyExpr::column(const LyExpr& source) {
    auto expr = std::make_shared<LyExpr>();
    expr->op = LyOp::COLUMN;
    expr->table_name = source.table_name;
    expr->column_name = source.column_name;
    expr->column_index = source.column_index;
    expr->table_cursor = source.table_cursor;
    // CRITICAL: Copy sqlite_expr for binding preservation
    expr->sqlite_expr = source.sqlite_expr;
    return expr;
}


// ===== FUNCTION BUILDER =====

LyExprPtr LyExpr::function(const std::string& name, std::vector<LyExprPtr> args) {
    auto expr = std::make_shared<LyExpr>();
    expr->op = LyOp::FUNCTION;
    expr->token = name;
    expr->args = std::move(args);
    return expr;
}


// ===== QUERY METHODS =====

bool LyExpr::is_function(const std::string& name) const {
    if (op != LyOp::FUNCTION && op != LyOp::AGG_FUNCTION) return false;
    if (name.empty()) return true;
    return token == name;
}

bool LyExpr::is_column() const {
    return op == LyOp::COLUMN;
}

bool LyExpr::is_literal() const {
    return op == LyOp::INTEGER || op == LyOp::FLOAT || 
           op == LyOp::STRING || op == LyOp::NULL_VAL ||
           op == LyOp::BLOB;
}

bool LyExpr::is_comparison() const {
    return op == LyOp::EQ || op == LyOp::NE ||
           op == LyOp::LT || op == LyOp::LE ||
           op == LyOp::GT || op == LyOp::GE;
}

bool LyExpr::is_arithmetic() const {
    return op == LyOp::ADD || op == LyOp::SUB ||
           op == LyOp::MUL || op == LyOp::DIV ||
           op == LyOp::MOD;
}

bool LyExpr::is_logical() const {
    return op == LyOp::AND || op == LyOp::OR || op == LyOp::NOT;
}

bool LyExpr::is_integer() const {
    return op == LyOp::INTEGER && std::holds_alternative<int64_t>(value);
}

bool LyExpr::is_float() const {
    return op == LyOp::FLOAT && std::holds_alternative<double>(value);
}

bool LyExpr::is_string() const {
    return op == LyOp::STRING && std::holds_alternative<std::string>(value);
}

bool LyExpr::is_null() const {
    return op == LyOp::NULL_VAL;
}

size_t LyExpr::nargs() const {
    return args.size();
}

LyExprPtr LyExpr::arg(size_t i) const {
    if (i >= args.size()) return nullptr;
    return args[i];
}

std::optional<int64_t> LyExpr::as_int() const {
    if (std::holds_alternative<int64_t>(value)) {
        return std::get<int64_t>(value);
    }
    return std::nullopt;
}

std::optional<double> LyExpr::as_float() const {
    if (std::holds_alternative<double>(value)) {
        return std::get<double>(value);
    }
    return std::nullopt;
}

std::optional<std::string> LyExpr::as_string() const {
    if (std::holds_alternative<std::string>(value)) {
        return std::get<std::string>(value);
    }
    return std::nullopt;
}



// ===== TREE OPERATIONS =====

LyExprPtr LyExpr::clone() const {
    auto out = std::make_shared<LyExpr>();
    out->op = op;
    out->value = value;
    out->token = token;
    out->table_name = table_name;
    out->column_name = column_name;
    out->column_index = column_index;
    out->table_cursor = table_cursor;
    out->sqlite_expr = sqlite_expr;  // Keep original reference for binding
    
    if (left) out->left = left->clone();
    if (right) out->right = right->clone();
    
    for (const auto& arg : args) {
        out->args.push_back(arg ? arg->clone() : nullptr);
    }
    
    return out;
}

std::string LyExpr::to_string() const {
    std::ostringstream ss;
    
    switch (op) {
        case LyOp::INTEGER:
            if (std::holds_alternative<int64_t>(value)) {
                ss << std::get<int64_t>(value);
            } else {
                ss << token;
            }
            break;
        case LyOp::FLOAT:
            if (std::holds_alternative<double>(value)) {
                ss << std::get<double>(value);
            } else {
                ss << token;
            }
            break;
        case LyOp::STRING:
            ss << "'" << (std::holds_alternative<std::string>(value) ? 
                         std::get<std::string>(value) : token) << "'";
            break;
        case LyOp::NULL_VAL:
            ss << "NULL";
            break;
        case LyOp::COLUMN:
            if (!table_name.empty()) ss << table_name << ".";
            ss << (column_name.empty() ? "col" + std::to_string(column_index) : column_name);
            break;
        case LyOp::FUNCTION:
        case LyOp::AGG_FUNCTION:
            ss << token << "(";
            for (size_t i = 0; i < args.size(); i++) {
                if (i > 0) ss << ", ";
                ss << (args[i] ? args[i]->to_string() : "?");
            }
            ss << ")";
            break;
        case LyOp::ADD:
            ss << "(" << (left ? left->to_string() : "?") << " + " 
               << (right ? right->to_string() : "?") << ")";
            break;
        case LyOp::SUB:
            ss << "(" << (left ? left->to_string() : "?") << " - " 
               << (right ? right->to_string() : "?") << ")";
            break;
        case LyOp::MUL:
            ss << "(" << (left ? left->to_string() : "?") << " * " 
               << (right ? right->to_string() : "?") << ")";
            break;
        case LyOp::DIV:
            ss << "(" << (left ? left->to_string() : "?") << " / " 
               << (right ? right->to_string() : "?") << ")";
            break;
        case LyOp::EQ:
            ss << (left ? left->to_string() : "?") << " = " 
               << (right ? right->to_string() : "?");
            break;
        case LyOp::NE:
            ss << (left ? left->to_string() : "?") << " != " 
               << (right ? right->to_string() : "?");
            break;
        case LyOp::LT:
            ss << (left ? left->to_string() : "?") << " < " 
               << (right ? right->to_string() : "?");
            break;
        case LyOp::LE:
            ss << (left ? left->to_string() : "?") << " <= " 
               << (right ? right->to_string() : "?");
            break;
        case LyOp::GT:
            ss << (left ? left->to_string() : "?") << " > " 
               << (right ? right->to_string() : "?");
            break;
        case LyOp::GE:
            ss << (left ? left->to_string() : "?") << " >= " 
               << (right ? right->to_string() : "?");
            break;
        case LyOp::AND:
            ss << "(" << (left ? left->to_string() : "?") << " AND " 
               << (right ? right->to_string() : "?") << ")";
            break;
        case LyOp::OR:
            ss << "(" << (left ? left->to_string() : "?") << " OR " 
               << (right ? right->to_string() : "?") << ")";
            break;
        case LyOp::NOT:
            ss << "NOT " << (left ? left->to_string() : "?");
            break;
        default:
            ss << "[op=" << static_cast<int>(op) << "]";
            if (left) ss << " left:" << left->to_string();
            if (right) ss << " right:" << right->to_string();
            break;
    }
    
    return ss.str();
}


} // namespace lyrore
