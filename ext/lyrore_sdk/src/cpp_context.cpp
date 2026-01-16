/*
** Lyrore C++ Plugin SDK - Plugin Manager Implementation
** 
** This file implements the C++ plugin manager that handles plugin loading
** and hook invocation. It provides the C ABI interface for SQLite integration.
*/

#include "lyrore_plugin.hpp"
#include "lyrore_cabi.h"
#include "lyrore_pattern.hpp"
#include <dlfcn.h>
#include <vector>
#include <string>
#include <cstring>
#include <memory>
#include <unordered_map>

// Include whereInt.h for WhereLoop/WhereLoopBuilder definitions
extern "C" {
#include "whereInt.h"

// Forward declaration for pattern capture hook
void lyrore_pattern_capture_preopt(void* pSelect);
}

namespace lyrore {

// ===== Context Methods Implementation =====

std::string EstimateContext::table_name() const {
    WhereLoopBuilder* builder = static_cast<WhereLoopBuilder*>(builder_);
    WhereLoop* loop = static_cast<WhereLoop*>(loop_);
    if (!builder || !builder->pWInfo) return "";
    WhereInfo* wi = builder->pWInfo;
    int iTab = loop->iTab;
    if (iTab < 0 || !wi->pTabList) return "";
    if (iTab >= wi->pTabList->nSrc) return "";
    const char* name = wi->pTabList->a[iTab].zName;
    return name ? name : "";
}

int64_t EstimateContext::cardinality() const {
    WhereLoop* loop = static_cast<WhereLoop*>(loop_);
    return lyrore_sqlite3LogEstToInt(loop->nOut);
}

void EstimateContext::set_cardinality(int64_t rows) {
    WhereLoop* loop = static_cast<WhereLoop*>(loop_);
    if (rows < 1) rows = 1;
    loop->nOut = lyrore_sqlite3LogEst(static_cast<u64>(rows));
}

bool EstimateContext::is_full_scan() const {
    WhereLoop* loop = static_cast<WhereLoop*>(loop_);
    return !(loop->wsFlags & WHERE_INDEXED);
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

// Get a specific WHERE term as raw Expr* by index
Expr* EstimateContext::get_where_term(int index) const {
    WhereLoopBuilder* builder = static_cast<WhereLoopBuilder*>(builder_);
    if (!builder || !builder->pWC) return nullptr;
    if (index < 0 || index >= builder->pWC->nTerm) return nullptr;
    return builder->pWC->a[index].pExpr;
}

// Get all WHERE terms as raw Expr* vector
std::vector<Expr*> EstimateContext::get_where_terms() const {
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

int64_t PostQueryContext::exec_time_us() const {
    return 0;
}

int64_t PostQueryContext::vm_steps() const {
    return 0;
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

    // Per-query cross-hook state (simplified - no LyExprPtr)
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

    // Cross-hook state methods (simplified)
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
                               const std::map<std::string, LyValue>& params) {
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
