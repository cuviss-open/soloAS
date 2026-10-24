#pragma once

#include "CoreMinimal.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#if !defined(CUVISS_HOOKS_NO_EXCEPTIONS) || !CUVISS_HOOKS_NO_EXCEPTIONS
    #include <stdexcept>
#endif

#if !defined(CUVISS_HOOKS_API)
    #define CUVISS_HOOKS_API_DEFINED_HERE
    #if defined(_WIN32)
        #if defined(CUVISS_HOOKS_BUILDING)
            #define CUVISS_HOOKS_API __declspec(dllexport)
        #else
            #define CUVISS_HOOKS_API __declspec(dllimport)
        #endif
    #elif defined(__GNUC__) || defined(__clang__)
        #define CUVISS_HOOKS_API __attribute__((visibility("default")))
    #else
        #define CUVISS_HOOKS_API
    #endif
#endif

// Register, Execute, and removal of different Tokens may be called
// concurrently. Access to one Token itself must be externally synchronized.
// Defining CUVISS_HOOKS_NO_EXCEPTIONS=1 makes recoverable contract failures
// return an empty Key, an empty Token, or false. Allocation failures are not
// recoverable through this API and follow the no-exception toolchain behavior.
class CUVISS_HOOKS_API Anchor {
private:
    class KeyState;
    class TokenState;
    using ContextCallback = std::function<void(void*)>;

public:
    // Opaque, reusable key produced by MakeKey.
    using Key = std::shared_ptr<const KeyState>;

    // Move-only opaque ownership pointer for one registration. It must be
    // retained while the callback is needed; destroying it removes the callback.
    using Token = std::unique_ptr<TokenState, void (*)(TokenState*)>;

    struct ContextContract {
        std::uint64_t type_id;
        std::uint32_t version;
        std::uint32_t size;
        std::uint32_t alignment;
    };

    template <typename Context>
    class ContextKey {
    public:
        ContextKey() = default;

        explicit operator bool() const noexcept {
            return static_cast<bool>(key_);
        }

    private:
        explicit ContextKey(Key key) : key_(std::move(key)) {}

        Key key_;
        friend class Anchor;
    };

    // Builds or retrieves an interned key for name. Throws
    // std::overflow_error when no new key ID remains. With
    // CUVISS_HOOKS_NO_EXCEPTIONS=1, recoverable contract failures return an
    // empty Key instead.
    static Key MakeKey(const std::string& name);

    // Builds or retrieves a typed context key. type_id and version form the
    // stable cross-module ABI identity; size and alignment are derived from
    // Context and validated before any callback can cast the context pointer.
    template <typename Context>
    static ContextKey<Context> MakeContextKey(
        const std::string& name,
        std::uint64_t type_id,
        std::uint32_t version = 1) {
        const ContextContract contract = {
            type_id,
            version,
            static_cast<std::uint32_t>(sizeof(Context)),
            static_cast<std::uint32_t>(alignof(Context))};
        return ContextKey<Context>(MakeContextKeyErased(name, contract));
    }

    // Rejects accidental direct use of nullptr at compile time.
    static Key MakeKey(std::nullptr_t) = delete;

    // Registers callback for key and returns its ownership token. The caller
    // must retain the returned Token: an ignored result is destroyed at the end
    // of the statement and immediately removes the registration. Exactly one
    // callback is active per key: if a callback is already active, Register
    // returns an empty Token and leaves that callback unchanged. Concurrent
    // registrations are serialized in an unspecified order, so one contender
    // receives the Token and the others receive empty Tokens.
    // Throws std::invalid_argument when key or callback is empty.
    // Throws std::overflow_error when no new registration ID remains.
    // In no-exception mode those failures return an empty Token.
    static Token Register(const Key& key, std::function<void()> callback);

    // Registers a typed callback for a matching context key. Context ownership
    // remains with Execute's caller and the callback must not retain it.
    template <typename Context, typename Callback>
    static Token Register(
        const ContextKey<Context>& key,
        Callback callback) {
        std::function<void(Context&)> callback_state(std::move(callback));
        if (!callback_state) {
#if defined(CUVISS_HOOKS_NO_EXCEPTIONS) && CUVISS_HOOKS_NO_EXCEPTIONS
            return RegisterContextErased(key.key_, ContextCallback());
#else
            throw std::invalid_argument("Anchor callback must not be empty");
#endif
        }
        return RegisterContextErased(
            key.key_,
            [callback_state](void* context) {
                callback_state(*static_cast<Context*>(context));
            });
    }

    // Rejects accidental direct use of nullptr at compile time.
    static Token Register(std::nullptr_t, std::function<void()>) = delete;
    static Token Register(const Key&, std::nullptr_t) = delete;
    static Token Register(std::nullptr_t, std::nullptr_t) = delete;
    template <typename Context>
    static Token Register(const ContextKey<Context>&, std::nullptr_t) = delete;

    // Explicitly removes the registration currently owned by token. This is
    // equivalent to token.reset(); it does not remove registrations by Key.
    // A callback already selected for Execute may still finish afterwards.
    // Concurrent access to this same Token must be externally synchronized.
    static void Unregister(Token& token) noexcept;

    // Invokes the active callback for key. Returns false when no callback is
    // registered. In the default mode callback exceptions propagate; in
    // no-exception mode callbacks must not throw.
    // The callback is copied from an immutable snapshot and runs without the
    // registry writer lock, so it may call Anchor functions. With concurrent
    // registration or token destruction, it observes either snapshot; a
    // callback already selected for execution may finish after unregistration.
    static bool Execute(const Key& key);

    // Invokes the active typed callback with caller-owned context.
    template <typename Context>
    static bool Execute(const ContextKey<Context>& key, Context& context) {
        return ExecuteContextErased(key.key_, &context);
    }

private:
    static Key MakeContextKeyErased(
        const std::string& name,
        const ContextContract& contract);
    static Token RegisterContextErased(
        const Key& key,
        ContextCallback callback);
    static bool ExecuteContextErased(const Key& key, void* context);
};

#ifdef CUVISS_HOOKS_API_DEFINED_HERE
    #undef CUVISS_HOOKS_API_DEFINED_HERE
    #undef CUVISS_HOOKS_API
#endif
