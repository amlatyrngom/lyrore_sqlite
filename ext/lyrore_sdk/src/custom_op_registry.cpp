/*
** Lyrore Custom Operator Registry
** 
** Per-connection storage for registered custom operators.
** Handles lookup and matching against query patterns.
*/

#include "lyrore_custom_op.hpp"
#include <unordered_map>
#include <mutex>

extern "C" {
#include "sqlite3.h"
}

namespace lyrore {

// ============================================================
// Per-Connection Registry Storage
// ============================================================

namespace {

// Global storage of per-connection registries
// Protected by mutex for thread safety
std::mutex g_registry_mutex;
std::unordered_map<sqlite3*, std::unique_ptr<CustomOpRegistry>> g_registries;

} // anonymous namespace


// ============================================================
// CustomOpRegistry Implementation
// ============================================================

CustomOpRegistry& CustomOpRegistry::for_db(sqlite3* db) {
    std::lock_guard<std::mutex> lock(g_registry_mutex);

    auto it = g_registries.find(db);
    if (it != g_registries.end()) {
        return *it->second;
    }

    // Create new registry for this connection
    auto registry = std::unique_ptr<CustomOpRegistry>(new CustomOpRegistry());
    auto& ref = *registry;
    g_registries[db] = std::move(registry);
    return ref;
}

void CustomOpRegistry::cleanup(sqlite3* db) {
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    g_registries.erase(db);
}

CustomOpEntry* CustomOpRegistry::register_op(std::unique_ptr<CustomOpEntry> entry) {
    // Get raw pointer before moving
    CustomOpEntry* ptr = entry.get();
    entries_.push_back(std::move(entry));
    return ptr;  // Return stable pointer to heap-allocated entry
}

const CustomOpEntry* CustomOpRegistry::find_match(Select* pSelect) const {
    if (!pSelect) return nullptr;

    for (const auto& entry : entries_) {
        if (!entry || !entry->pattern) continue;

        // Try to match the pattern against the Select*
        auto match = entry->pattern->match(pSelect);
        if (match.matched()) {
            return entry.get();
        }
    }

    return nullptr;
}

} // namespace lyrore
