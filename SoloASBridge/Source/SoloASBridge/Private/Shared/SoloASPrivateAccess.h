#pragma once

#include <functional>
#include <type_traits>
#include <utility>

namespace SoloASBridge::PrivateAccess
{
/**
 * Mechanism only: tags and explicit instantiations stay in adapter .cpp files.
 * Tags declare Type and friend Type Resolve(Tag) noexcept in this namespace.
 * Public adapters expose bounded operations, never tags or raw engine access.
 * Bitfields are unsupported; non-exported functions need a proven link target.
 * Fixed-version semantic admission remains the responsibility of the adapter.
 */
template<class Tag, typename Tag::Type Member>
struct TMember final
{
    friend typename Tag::Type Resolve(Tag) noexcept { return Member; }
};

/** Binds a typed operation without exposing an inaccessible nested member type. */
template<class Tag, auto Member>
struct TBoundOperation final
{
    friend typename Tag::Type Resolve(Tag) noexcept
    {
        return &Tag::template Invoke<Member>;
    }
};

/** Data access is lvalue-only; adapters must not export unrestricted engine references. */
template<class Tag, class Object>
requires std::is_member_object_pointer_v<typename Tag::Type>
decltype(auto) Get(Object& Target) noexcept
{
    return Target.*Resolve(Tag{});
}

template<class Tag, class Object>
void Get(const Object&&) = delete;

template<class Tag, class Object, class... Args>
requires (std::is_member_function_pointer_v<typename Tag::Type>
    && std::is_invocable_v<typename Tag::Type, Object&&, Args&&...>)
decltype(auto) Call(Object&& Target, Args&&... Arguments)
    noexcept(std::is_nothrow_invocable_v<typename Tag::Type, Object&&, Args&&...>)
{
    return std::invoke(Resolve(Tag{}), std::forward<Object>(Target), std::forward<Args>(Arguments)...);
}

template<class Tag, class... Args>
requires (std::is_pointer_v<typename Tag::Type>
    && std::is_function_v<std::remove_pointer_t<typename Tag::Type>>
    && std::is_invocable_v<typename Tag::Type, Args&&...>)
decltype(auto) CallStatic(Args&&... Arguments)
    noexcept(std::is_nothrow_invocable_v<typename Tag::Type, Args&&...>)
{
    return std::invoke(Resolve(Tag{}), std::forward<Args>(Arguments)...);
}
}
