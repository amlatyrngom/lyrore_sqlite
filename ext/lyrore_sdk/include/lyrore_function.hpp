/*
** Lyrore Function Registry
**
** Provides C++ template-based function registration for SQLite.
** Supports INNOCUOUS flag for safe use in triggers and views.
*/

#ifndef LYRORE_FUNCTION_HPP
#define LYRORE_FUNCTION_HPP

#include <functional>
#include <string>
#include <tuple>
#include <type_traits>

extern "C" {
#include "sqlite3.h"
}

namespace lyrore {

// ============================================================
// Type Traits for SQLite Value Conversion
// ============================================================

template<typename T>
struct sqlite_value_traits;

template<>
struct sqlite_value_traits<int64_t> {
    static int64_t get(sqlite3_value* val) { return sqlite3_value_int64(val); }
    static void set(sqlite3_context* ctx, int64_t v) { sqlite3_result_int64(ctx, v); }
};

template<>
struct sqlite_value_traits<int> {
    static int get(sqlite3_value* val) { return sqlite3_value_int(val); }
    static void set(sqlite3_context* ctx, int v) { sqlite3_result_int(ctx, v); }
};

template<>
struct sqlite_value_traits<double> {
    static double get(sqlite3_value* val) { return sqlite3_value_double(val); }
    static void set(sqlite3_context* ctx, double v) { sqlite3_result_double(ctx, v); }
};

template<>
struct sqlite_value_traits<std::string> {
    static std::string get(sqlite3_value* val) {
        const unsigned char* text = sqlite3_value_text(val);
        return text ? std::string(reinterpret_cast<const char*>(text)) : std::string();
    }
    static void set(sqlite3_context* ctx, const std::string& v) {
        sqlite3_result_text(ctx, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
    }
};

// ============================================================
// Internal Implementation Details
// ============================================================

namespace detail {

// Function wrapper storage
template<typename Ret, typename... Args>
struct FunctionWrapper {
    std::function<Ret(Args...)> fn;

    static void call(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
        (void)argc;
        auto* wrapper = static_cast<FunctionWrapper*>(sqlite3_user_data(ctx));
        try {
            auto result = call_with_args<0>(wrapper->fn, argv, std::index_sequence_for<Args...>{});
            sqlite_value_traits<Ret>::set(ctx, result);
        } catch (const std::exception& e) {
            sqlite3_result_error(ctx, e.what(), -1);
        }
    }

    static void destroy(void* ptr) {
        delete static_cast<FunctionWrapper*>(ptr);
    }

private:
    template<size_t I, typename Fn, size_t... Is>
    static Ret call_with_args(Fn& fn, sqlite3_value** argv, std::index_sequence<Is...>) {
        return fn(sqlite_value_traits<std::tuple_element_t<Is, std::tuple<Args...>>>::get(argv[Is])...);
    }
};

// Void return specialization
template<typename... Args>
struct FunctionWrapper<void, Args...> {
    std::function<void(Args...)> fn;

    static void call(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
        (void)argc;
        auto* wrapper = static_cast<FunctionWrapper*>(sqlite3_user_data(ctx));
        try {
            call_with_args<0>(wrapper->fn, argv, std::index_sequence_for<Args...>{});
            sqlite3_result_null(ctx);
        } catch (const std::exception& e) {
            sqlite3_result_error(ctx, e.what(), -1);
        }
    }

    static void destroy(void* ptr) {
        delete static_cast<FunctionWrapper*>(ptr);
    }

private:
    template<size_t I, typename Fn, size_t... Is>
    static void call_with_args(Fn& fn, sqlite3_value** argv, std::index_sequence<Is...>) {
        fn(sqlite_value_traits<std::tuple_element_t<Is, std::tuple<Args...>>>::get(argv[Is])...);
    }
};

} // namespace detail

// ============================================================
// Public API: register_scalar_function
// ============================================================

/**
 * Register a C++ function as a SQLite scalar function.
 *
 * @param db            SQLite database connection
 * @param name          Function name in SQL
 * @param fn            C++ function/lambda to call
 * @param deterministic True if function always returns same result for same inputs
 * @param innocuous     True if function is safe to use in triggers/views (SQLITE_INNOCUOUS)
 *
 * Usage:
 *   int count = 0;
 *   register_scalar_function<int64_t, int64_t>(db, "inc",
 *       [&](int64_t x) { count++; return x; }, false, true);
 *
 *   // SQL:
 *   CREATE TRIGGER t AFTER INSERT ON d BEGIN SELECT inc(NEW.id); END;
 */
template<typename Ret, typename... Args>
void register_scalar_function(
    sqlite3* db,
    const char* name,
    std::function<Ret(Args...)> fn,
    bool deterministic = true,
    bool innocuous = true
) {
    int flags = SQLITE_UTF8;
    if (deterministic) flags |= SQLITE_DETERMINISTIC;
    if (innocuous) flags |= SQLITE_INNOCUOUS;

    auto* wrapper = new detail::FunctionWrapper<Ret, Args...>{std::move(fn)};

    sqlite3_create_function_v2(
        db,
        name,
        sizeof...(Args),
        flags,
        wrapper,
        &detail::FunctionWrapper<Ret, Args...>::call,
        nullptr,
        nullptr,
        &detail::FunctionWrapper<Ret, Args...>::destroy
    );
}

// Convenience overload for lambdas
template<typename Fn>
void register_scalar_function_lambda(
    sqlite3* db,
    const char* name,
    Fn&& fn,
    bool deterministic = true,
    bool innocuous = true
);

} // namespace lyrore

#endif // LYRORE_FUNCTION_HPP
