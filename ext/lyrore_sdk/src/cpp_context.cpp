/*
** Lyrore C++ Context Implementation
** 
** Implements context classes and C ABI entry points.
*/
#include "lyrore_plugin.hpp"
#include "lyrore_custom_op.hpp"

#include <dlfcn.h>
#include <iostream>
#include <cstring>

extern "C" {
#include "sqliteInt.h"
#include "whereInt.h"
#include "lyrore_cabi.h"
}

namespace lyrore {

// ===== EstimateContext Implementation =====

std::string EstimateContext::table_name() const {
    WhereLoop* loop = static_cast<WhereLoop*>(loop_);
    WhereLoopBuilder* builder = static_cast<WhereLoopBuilder*>(builder_);
    if (!loop || !builder || !builder->pWInfo || !builder->pWInfo->pTabList) return "";

    // WhereLoop::iTab is the index into pWInfo->pTabList->a[]
    int iTab = loop->iTab;
    SrcList* pTabList = builder->pWInfo->pTabList;
    if (iTab < 0 || iTab >= pTabList->nSrc) return "";

    SrcItem* pItem = &pTabList->a[iTab];
    if (!pItem || !pItem->pSTab) return "";

    return pItem->pSTab->zName ? pItem->pSTab->zName : "";
}

int64_t EstimateContext::cardinality() const {
    WhereLoop* loop = static_cast<WhereLoop*>(loop_);
    if (!loop) return 0;
    return (int64_t)lyrore_sqlite3LogEstToInt((LogEst_wrapper)loop->nOut);
}

void EstimateContext::set_cardinality(int64_t rows) {
    WhereLoop* loop = static_cast<WhereLoop*>(loop_);
    if (!loop) return;
    loop->nOut = (LogEst)lyrore_sqlite3LogEst((u64_wrapper)rows);
}

bool EstimateContext::is_full_scan() const {
    WhereLoop* loop = static_cast<WhereLoop*>(loop_);
    return loop && (loop->wsFlags & WHERE_IPK) != 0 && loop->nLTerm == 0;
}

bool EstimateContext::is_index_scan() const {
    WhereLoop* loop = static_cast<WhereLoop*>(loop_);
    return (loop->wsFlags & WHERE_INDEXED) != 0;
}

int EstimateContext::num_where_terms() const {
    WhereLoopBuilder* builder = static_cast<WhereLoopBuilder*>(builder_);
    if (!builder || !builder->pWC) return 0;
    return builder->pWC->nTerm;
}

LyExprPtr EstimateContext::get_where_term(int index) const {
    WhereLoopBuilder* builder = static_cast<WhereLoopBuilder*>(builder_);
    if (!builder || !builder->pWC) return nullptr;
    if (index < 0 || index >= builder->pWC->nTerm) return nullptr;
    return LyExpr::from_sqlite(builder->pWC->a[index].pExpr);
}

std::vector<LyExprPtr> EstimateContext::get_where_terms() const {
    std::vector<LyExprPtr> terms;
    WhereLoopBuilder* builder = static_cast<WhereLoopBuilder*>(builder_);
    if (!builder || !builder->pWC) return terms;
    for (int i = 0; i < builder->pWC->nTerm; i++) {
        Expr* pExpr = builder->pWC->a[i].pExpr;
        if (pExpr) {
            terms.push_back(LyExpr::from_sqlite(pExpr));
        }
    }
    return terms;
}

Expr* EstimateContext::get_where_term_raw(int index) const {
    WhereLoopBuilder* builder = static_cast<WhereLoopBuilder*>(builder_);
    if (!builder || !builder->pWC) return nullptr;
    if (index < 0 || index >= builder->pWC->nTerm) return nullptr;
    return builder->pWC->a[index].pExpr;
}

std::vector<Expr*> EstimateContext::get_where_terms_raw() const {
    std::vector<Expr*> terms;
    WhereLoopBuilder* builder = static_cast<WhereLoopBuilder*>(builder_);
    if (!builder || !builder->pWC) return terms;
    for (int i = 0; i < builder->pWC->nTerm; i++) {
        Expr* pExpr = builder->pWC->a[i].pExpr;
        if (pExpr) {
            terms.push_back(pExpr);
        }
    }
    return terms;
}

Select* EstimateContext::select_raw() {
    WhereLoopBuilder* builder = static_cast<WhereLoopBuilder*>(builder_);
    if (!builder || !builder->pWInfo) return nullptr;
    return builder->pWInfo->pSelect;
}



// ===== PostQueryContext Implementation (Enhanced) =====

StmtStats PostQueryContext::stmt_stats() const {
    StmtStats stats;

    // Get the statement from Vdbe
    Vdbe* v = static_cast<Vdbe*>(pVdbe_);
    if (!v) return stats;

    sqlite3_stmt* pStmt = reinterpret_cast<sqlite3_stmt*>(v);

    // Collect all statement-level statistics
    stats.fullscan_steps = sqlite3_stmt_status(pStmt, SQLITE_STMTSTATUS_FULLSCAN_STEP, 0);
    stats.sorts = sqlite3_stmt_status(pStmt, SQLITE_STMTSTATUS_SORT, 0);
    stats.autoindex_inserts = sqlite3_stmt_status(pStmt, SQLITE_STMTSTATUS_AUTOINDEX, 0);
    stats.vm_steps = sqlite3_stmt_status(pStmt, SQLITE_STMTSTATUS_VM_STEP, 0);
    stats.runs = sqlite3_stmt_status(pStmt, SQLITE_STMTSTATUS_RUN, 0);
    stats.filter_hits = sqlite3_stmt_status(pStmt, SQLITE_STMTSTATUS_FILTER_HIT, 0);
    stats.filter_misses = sqlite3_stmt_status(pStmt, SQLITE_STMTSTATUS_FILTER_MISS, 0);
    stats.memory_used = sqlite3_stmt_status(pStmt, SQLITE_STMTSTATUS_MEMUSED, 0);
    stats.reprepares = sqlite3_stmt_status(pStmt, SQLITE_STMTSTATUS_REPREPARE, 0);

    return stats;
}

std::vector<ScanStats> PostQueryContext::scan_stats() const {
    std::vector<ScanStats> result;

#ifdef SQLITE_ENABLE_STMT_SCANSTATUS
    Vdbe* v = static_cast<Vdbe*>(pVdbe_);
    if (!v) return result;

    sqlite3_stmt* pStmt = reinterpret_cast<sqlite3_stmt*>(v);

    int idx = 0;
    while (true) {
        ScanStats s;
        int rc;

        // Get scan ID
        rc = sqlite3_stmt_scanstatus_v2(pStmt, idx, SQLITE_SCANSTAT_SELECTID, 
                                         SQLITE_SCANSTAT_COMPLEX, &s.scan_id);
        if (rc != SQLITE_OK) break;  // No more scans

        // Get parent ID
        sqlite3_stmt_scanstatus_v2(pStmt, idx, SQLITE_SCANSTAT_PARENTID,
                                    SQLITE_SCANSTAT_COMPLEX, &s.parent_id);

        // Get table/index name
        const char* zName = nullptr;
        sqlite3_stmt_scanstatus_v2(pStmt, idx, SQLITE_SCANSTAT_NAME,
                                    SQLITE_SCANSTAT_COMPLEX, &zName);
        if (zName) s.name = zName;

        // Get EXPLAIN text
        const char* zExplain = nullptr;
        sqlite3_stmt_scanstatus_v2(pStmt, idx, SQLITE_SCANSTAT_EXPLAIN,
                                    SQLITE_SCANSTAT_COMPLEX, &zExplain);
        if (zExplain) s.explain = zExplain;

        // Get loop count
        sqlite3_int64 nLoop = 0;
        sqlite3_stmt_scanstatus_v2(pStmt, idx, SQLITE_SCANSTAT_NLOOP,
                                    SQLITE_SCANSTAT_COMPLEX, &nLoop);
        s.loops = nLoop;

        // Get visit count
        sqlite3_int64 nVisit = 0;
        sqlite3_stmt_scanstatus_v2(pStmt, idx, SQLITE_SCANSTAT_NVISIT,
                                    SQLITE_SCANSTAT_COMPLEX, &nVisit);
        s.visits = nVisit;

        // Get estimate
        sqlite3_stmt_scanstatus_v2(pStmt, idx, SQLITE_SCANSTAT_EST,
                                    SQLITE_SCANSTAT_COMPLEX, &s.estimate);

        // Get cycles (may not be available)
        sqlite3_int64 nCycle = -1;
        rc = sqlite3_stmt_scanstatus_v2(pStmt, idx, SQLITE_SCANSTAT_NCYCLE,
                                         SQLITE_SCANSTAT_COMPLEX, &nCycle);
        s.cycles = (rc == SQLITE_OK) ? nCycle : -1;

        result.push_back(s);
        idx++;
    }
#endif

    return result;
}

int PostQueryContext::scan_count() const {
#ifdef SQLITE_ENABLE_STMT_SCANSTATUS
    Vdbe* v = static_cast<Vdbe*>(pVdbe_);
    if (!v) return 0;

    sqlite3_stmt* pStmt = reinterpret_cast<sqlite3_stmt*>(v);

    int count = 0;
    int dummy;
    while (sqlite3_stmt_scanstatus_v2(pStmt, count, SQLITE_SCANSTAT_SELECTID,
                                       SQLITE_SCANSTAT_COMPLEX, &dummy) == SQLITE_OK) {
        count++;
    }
    return count;
#else
    return 0;
#endif
}

int64_t PostQueryContext::total_cycles() const {
#ifdef SQLITE_ENABLE_STMT_SCANSTATUS
    auto scans = scan_stats();
    int64_t total = 0;
    for (const auto& s : scans) {
        if (s.cycles >= 0) total += s.cycles;
    }
    return total > 0 ? total : -1;
#else
    return -1;
#endif
}

bool PostQueryContext::scanstatus_enabled() const {
#ifdef SQLITE_ENABLE_STMT_SCANSTATUS
    return true;
#else
    return false;
#endif
}

// Legacy methods
int64_t PostQueryContext::exec_time_us() const {
    return 0;  // Not available without profiling
}

int64_t PostQueryContext::vm_steps() const {
    return stmt_stats().vm_steps;
}


} // namespace lyrore


// ===== Plugin Entry Management =====

struct PluginEntry {
    void* handle;
    lyrore::Plugin* plugin;
    std::string path;

    PluginEntry() : handle(nullptr), plugin(nullptr) {}
    ~PluginEntry() {
        if (plugin) {
            plugin->onShutdown();
            void (*destroy)(lyrore::Plugin*) = (void(*)(lyrore::Plugin*))dlsym(handle, "lyrore_destroy_plugin");
            if (destroy) {
                destroy(plugin);
            } else {
                delete plugin;
            }
        }
        if (handle) {
            dlclose(handle);
        }
    }
};


// ===== C++ Context =====

struct LyroreCppContext {
    sqlite3* db;
    std::vector<std::unique_ptr<PluginEntry>> plugins;

    // Per-query cross-hook state
    struct QueryState {
        int template_id = -1;
        std::map<std::string, lyrore::LyValue> params;
    };
    std::unordered_map<Select*, QueryState> query_states_;

    explicit LyroreCppContext(sqlite3* db_) : db(db_) {}

    int load_plugin(const char* path) {
        void* handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
        if (!handle) {
            return SQLITE_ERROR;
        }

        lyrore::Plugin* (*create)() = (lyrore::Plugin*(*)())dlsym(handle, "lyrore_create_plugin");
        if (!create) {
            dlclose(handle);
            return SQLITE_ERROR;
        }

        lyrore::Plugin* plugin = create();
        if (!plugin) {
            dlclose(handle);
            return SQLITE_ERROR;
        }

        plugin->context_ = this;
        plugin->onInit(db);

        auto entry = std::make_unique<PluginEntry>();
        entry->handle = handle;
        entry->plugin = plugin;
        entry->path = path;
        plugins.push_back(std::move(entry));

        return SQLITE_OK;
    }

    void invoke_preopt(Parse* pParse, Select* pSelect) {
        if (!pSelect) return;

        // Check if pattern capture mode is active
        lyrore_pattern_capture_preopt(pSelect);

        lyrore::PreOptContext ctx(db, pParse, pSelect);

        for (auto& entry : plugins) {
            entry->plugin->onPreOpt(ctx);
        }

        lyrore::rewrite_custom_ops(ctx);
    }

    void invoke_estimate(void* pBuilder, void* pLoop) {
        lyrore::EstimateContext ctx(db, pBuilder, pLoop);
        for (auto& entry : plugins) {
            entry->plugin->onEstimate(ctx);
        }
    }

    void invoke_analyze(int iDb) {
        (void)iDb;
        lyrore::AnalyzeContext ctx(db);
        for (auto& entry : plugins) {
            entry->plugin->onAnalyze(ctx);
        }
    }

    void invoke_postquery(void* pVdbe) {
        lyrore::PostQueryContext ctx(db, pVdbe);
        for (auto& entry : plugins) {
            entry->plugin->onPostQuery(ctx);
        }
    }

    void set_query_state(Select* sel, int template_id, 
                        const std::map<std::string, lyrore::LyValue>& params) {
        auto& state = query_states_[sel];
        state.template_id = template_id;
        state.params = params;
    }

    std::optional<int> get_query_template_id(Select* sel) const {
        auto it = query_states_.find(sel);
        if (it == query_states_.end()) return std::nullopt;
        return it->second.template_id;
    }

    const std::map<std::string, lyrore::LyValue>* get_query_params(Select* sel) const {
        auto it = query_states_.find(sel);
        if (it == query_states_.end()) return nullptr;
        return &it->second.params;
    }

    void clear_query_state(Select* sel) {
        query_states_.erase(sel);
    }
};


// ===== Plugin Template State Method Implementations =====

namespace lyrore {

void Plugin::set_template_match(Select* sel, int template_id, 
                               const std::map<std::string, LyValue>& params,
                               const std::map<std::string, LyExprPtr>& expr_params) {
    (void)expr_params;
    if (!context_) return;
    context_->set_query_state(sel, template_id, params);
}

std::optional<int> Plugin::get_template_id(Select* sel) const {
    if (!context_) return std::nullopt;
    return context_->get_query_template_id(sel);
}

const std::map<std::string, LyValue>* Plugin::get_template_params(Select* sel) const {
    if (!context_) return nullptr;
    return context_->get_query_params(sel);
}

const std::map<std::string, LyExprPtr>* Plugin::get_template_expr_params(Select* sel) const {
    // Not implemented in current context storage
    (void)sel;
    return nullptr;
}

void Plugin::clear_template_match(Select* sel) {
    if (!context_) return;
    context_->clear_query_state(sel);
}

} // namespace lyrore


// ===== C ABI Entry Points =====

extern "C" {

LyroreCppContext* lyrore_cpp_create(sqlite3* db) {
    return new LyroreCppContext(db);
}

void lyrore_cpp_destroy(LyroreCppContext* ctx) {
    if (!ctx) return;
    lyrore::CustomOpRegistry::cleanup(ctx->db);
    delete ctx;
}

int lyrore_cpp_load_plugin(LyroreCppContext* ctx, const char* path) {
    if (!ctx || !path) return SQLITE_ERROR;
    return ctx->load_plugin(path);
}

int lyrore_cpp_invoke_preopt(LyroreCppContext* ctx, void* pParse, void* pSelect) {
    if (!ctx) return SQLITE_OK;
    ctx->invoke_preopt(static_cast<Parse*>(pParse), static_cast<Select*>(pSelect));
    return SQLITE_OK;
}

void lyrore_cpp_invoke_estimate(LyroreCppContext* ctx, void* pBuilder, void* pLoop) {
    if (!ctx) return;
    ctx->invoke_estimate(pBuilder, pLoop);
}

void lyrore_cpp_invoke_analyze(LyroreCppContext* ctx, int iDb) {
    if (!ctx) return;
    ctx->invoke_analyze(iDb);
}

void lyrore_cpp_invoke_postquery(LyroreCppContext* ctx, void* pVdbe) {
    if (!ctx) return;
    ctx->invoke_postquery(pVdbe);
}

} // extern "C"
