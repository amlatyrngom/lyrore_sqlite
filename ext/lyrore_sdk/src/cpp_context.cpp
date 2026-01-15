/*
** Lyrore C++ Plugin SDK - Plugin Manager Implementation
** 
** This file implements the C++ plugin manager that handles plugin loading
** and hook invocation. It provides the C ABI interface for SQLite integration.
*/

#include "lyrore_plugin.hpp"
#include <dlfcn.h>
#include <vector>
#include <string>
#include <cstring>
#include <memory>

// Include whereInt.h for WhereLoop/WhereLoopBuilder definitions
// This is needed in the SDK implementation but not exposed to plugins
extern "C" {
#include "whereInt.h"
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
    // nOut is LogEst (log2(x) * 10)
    return sqlite3LogEstToInt(loop->nOut);
}

void EstimateContext::set_cardinality(int64_t rows) {
    WhereLoop* loop = static_cast<WhereLoop*>(loop_);
    if (rows < 1) rows = 1;
    loop->nOut = sqlite3LogEst(static_cast<u64>(rows));
}

bool EstimateContext::is_full_scan() const {
    WhereLoop* loop = static_cast<WhereLoop*>(loop_);
    return !(loop->wsFlags & WHERE_INDEXED);
}

bool EstimateContext::is_index_scan() const {
    WhereLoop* loop = static_cast<WhereLoop*>(loop_);
    return (loop->wsFlags & WHERE_INDEXED) != 0;
}


// Get number of WHERE clause terms
int EstimateContext::num_where_terms() const {
    WhereLoopBuilder* builder = static_cast<WhereLoopBuilder*>(builder_);
    if (!builder || !builder->pWC) return 0;
    return builder->pWC->nTerm;
}

// Get a specific WHERE term as LyExpr by index
LyExprPtr EstimateContext::get_where_term(int index) const {
    WhereLoopBuilder* builder = static_cast<WhereLoopBuilder*>(builder_);
    if (!builder || !builder->pWC) return nullptr;
    if (index < 0 || index >= builder->pWC->nTerm) return nullptr;
    Expr* pExpr = builder->pWC->a[index].pExpr;
    if (!pExpr) return nullptr;
    return LyExpr::from_sqlite(pExpr);
}

// Get all WHERE terms as LyExpr vector  
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


int64_t PostQueryContext::exec_time_us() const {
    // Would need internal tracking - return 0 for now
    return 0;
}

int64_t PostQueryContext::vm_steps() const {
    // Would need internal tracking - return 0 for now
    return 0;
}

} // namespace lyrore


// ===== Plugin Entry Management =====
// Note: These are at global scope to match C ABI declarations

struct PluginEntry {
    void* handle;
    lyrore::Plugin* plugin;
    std::string path;
    
    PluginEntry() : handle(nullptr), plugin(nullptr) {}
    ~PluginEntry() {
        if (plugin) {
            plugin->onShutdown();
            // Find destroy function
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


// ===== C++ Context - at global scope to match C ABI =====

struct LyroreCppContext {
    sqlite3* db;
    std::vector<std::unique_ptr<PluginEntry>> plugins;
    
    explicit LyroreCppContext(sqlite3* db_) : db(db_) {}
    
    int load_plugin(const char* path) {
        // Open the shared library with RTLD_GLOBAL so plugin can call SQLite APIs
        void* handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
        if (!handle) {
            return SQLITE_ERROR;
        }
        
        // Find the plugin factory function
        lyrore::Plugin* (*create)() = (lyrore::Plugin*(*)())dlsym(handle, "lyrore_create_plugin");
        if (!create) {
            dlclose(handle);
            return SQLITE_ERROR;
        }
        
        // Create the plugin
        lyrore::Plugin* plugin = create();
        if (!plugin) {
            dlclose(handle);
            return SQLITE_ERROR;
        }
        
        // Initialize the plugin
        plugin->onInit(db);
        
        // Store the plugin
        auto entry = std::make_unique<PluginEntry>();
        entry->handle = handle;
        entry->plugin = plugin;
        entry->path = path;
        plugins.push_back(std::move(entry));
        
        return SQLITE_OK;
    }
    
    void invoke_preopt(Parse* pParse, Select* pSelect) {
        if (!pSelect) return;
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
        (void)iDb;  // May use later for schema-specific hooks
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
};


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
