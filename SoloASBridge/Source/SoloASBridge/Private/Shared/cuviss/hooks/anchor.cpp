#include <Shared/cuviss/hooks/anchor.h>

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#if !defined(CUVISS_HOOKS_NO_EXCEPTIONS) || !CUVISS_HOOKS_NO_EXCEPTIONS
    #include <stdexcept>
#endif

namespace {

using Callback = std::function<void(void*)>;
using KeyId = std::uint64_t;
using RegistrationId = std::uint64_t;

struct CallbackEntry {
    std::shared_ptr<class RegistrationState> state;
    std::shared_ptr<const Callback> callback;
};

using CallbackMap = std::unordered_map<KeyId, CallbackEntry>;

class RegistrationState {
public:
    explicit RegistrationState(RegistrationId id) : id_(id) {}

    bool IsActive() const noexcept {
        return active_.load(std::memory_order_acquire);
    }

    void Deactivate() noexcept {
        active_.store(false, std::memory_order_release);
    }

    const RegistrationId id_;

private:
    std::atomic<bool> active_{true};
};

struct Registry {
    Registry() : callbacks(std::make_shared<CallbackMap>()) {}

    std::unordered_map<std::string, std::weak_ptr<const void> > interned_keys;
    std::shared_ptr<const CallbackMap> callbacks;
    KeyId next_key_id = 1;
    RegistrationId next_registration_id = 1;
    std::mutex mutex;
};

std::shared_ptr<Registry> GetRegistryHandle() {
    static std::shared_ptr<Registry> registry = std::make_shared<Registry>();
    return registry;
}

}  // namespace

class Anchor::KeyState {
public:
    KeyState(KeyId id,
             std::string name,
             ContextContract context_contract,
             std::weak_ptr<Registry> registry)
        : id_(id),
          name_(std::move(name)),
          context_contract_(context_contract),
          registry_(std::move(registry)) {}

    ~KeyState() {
        const std::shared_ptr<Registry> registry = registry_.lock();
        if (!registry) {
            return;
        }

        std::lock_guard<std::mutex> lock(registry->mutex);
        const auto interned_key = registry->interned_keys.find(name_);
        if (interned_key != registry->interned_keys.end() &&
            interned_key->second.expired()) {
            registry->interned_keys.erase(interned_key);
        }
    }

    const KeyId id_;
    const std::string name_;
    const ContextContract context_contract_;

private:
    const std::weak_ptr<Registry> registry_;
};

namespace {

void RemoveRegistrationSnapshot(
    const std::shared_ptr<Registry>& registry,
    const Anchor::Key& key,
    const std::shared_ptr<RegistrationState>& state) {
    std::shared_ptr<const CallbackMap> retired;
    {
        std::lock_guard<std::mutex> lock(registry->mutex);
        retired = std::atomic_load(&registry->callbacks);
        const auto existing = retired->find(key->id_);
        if (existing == retired->end() || existing->second.state != state) {
            return;
        }

        const std::shared_ptr<CallbackMap> next =
            std::make_shared<CallbackMap>(*retired);
        next->erase(key->id_);

        const std::shared_ptr<const CallbackMap> published = next;
        std::atomic_store(&registry->callbacks, published);
    }
}

void RemoveRegistration(const std::shared_ptr<Registry>& registry,
                        Anchor::Key key,
                        const std::shared_ptr<RegistrationState>& state) noexcept {
#if defined(CUVISS_HOOKS_NO_EXCEPTIONS) && CUVISS_HOOKS_NO_EXCEPTIONS
    RemoveRegistrationSnapshot(registry, key, state);
#else
    try {
        RemoveRegistrationSnapshot(registry, key, state);
    } catch (...) {
        // Token destruction must never throw. If allocation fails while
        // removing a snapshot, the process remains memory-safe and the
        // registry releases the remaining entry during normal teardown.
    }
#endif
}

}  // namespace

class Anchor::TokenState {
public:
    TokenState(std::shared_ptr<Registry> registry,
               Key key,
               std::shared_ptr<RegistrationState> state)
        : registry_(std::move(registry)), key_(std::move(key)), state_(std::move(state)) {}

    ~TokenState() {
        if (published_) {
            state_->Deactivate();
            RemoveRegistration(std::move(registry_), std::move(key_), state_);
        }
    }

    void MarkPublished() noexcept {
        published_ = true;
    }

    static void Destroy(TokenState* state) noexcept {
        delete state;
    }

private:
    std::shared_ptr<Registry> registry_;
    Key key_;
    std::shared_ptr<RegistrationState> state_;
    bool published_ = false;
};

Anchor::Key Anchor::MakeKey(const std::string& name) {
    const std::shared_ptr<Registry> registry = GetRegistryHandle();
    std::lock_guard<std::mutex> lock(registry->mutex);
    const auto existing = registry->interned_keys.find(name);
    if (existing != registry->interned_keys.end()) {
        const std::shared_ptr<const void> owner = existing->second.lock();
        if (owner) {
            const Key key(owner, static_cast<const KeyState*>(owner.get()));
            if (key->context_contract_.type_id != 0) {
#if defined(CUVISS_HOOKS_NO_EXCEPTIONS) && CUVISS_HOOKS_NO_EXCEPTIONS
                return Key();
#else
                throw std::invalid_argument(
                    "Anchor key name is already bound to a context contract");
#endif
            }
            return key;
        }
        registry->interned_keys.erase(existing);
    }

    if (registry->next_key_id == 0) {
#if defined(CUVISS_HOOKS_NO_EXCEPTIONS) && CUVISS_HOOKS_NO_EXCEPTIONS
        return Key();
#else
        throw std::overflow_error("Anchor key ID space exhausted");
#endif
    }
    const Key key =
        std::make_shared<KeyState>(
            registry->next_key_id++,
            name,
            ContextContract{0, 0, 0, 0},
            registry);
    registry->interned_keys.emplace(name, key);
    return key;
}

Anchor::Key Anchor::MakeContextKeyErased(
    const std::string& name,
    const ContextContract& contract) {
    if (contract.type_id == 0 || contract.version == 0 ||
        contract.size == 0 || contract.alignment == 0) {
#if defined(CUVISS_HOOKS_NO_EXCEPTIONS) && CUVISS_HOOKS_NO_EXCEPTIONS
        return Key();
#else
        throw std::invalid_argument(
            "Anchor context contract fields must not be zero");
#endif
    }

    const std::shared_ptr<Registry> registry = GetRegistryHandle();
    std::lock_guard<std::mutex> lock(registry->mutex);
    const auto existing = registry->interned_keys.find(name);
    if (existing != registry->interned_keys.end()) {
        const std::shared_ptr<const void> owner = existing->second.lock();
        if (owner) {
            const Key key(owner, static_cast<const KeyState*>(owner.get()));
            const ContextContract& existing_contract = key->context_contract_;
            if (existing_contract.type_id != contract.type_id ||
                existing_contract.version != contract.version ||
                existing_contract.size != contract.size ||
                existing_contract.alignment != contract.alignment) {
#if defined(CUVISS_HOOKS_NO_EXCEPTIONS) && CUVISS_HOOKS_NO_EXCEPTIONS
                return Key();
#else
                throw std::invalid_argument(
                    "Anchor key name has a different shape or context contract");
#endif
            }
            return key;
        }
        registry->interned_keys.erase(existing);
    }

    if (registry->next_key_id == 0) {
#if defined(CUVISS_HOOKS_NO_EXCEPTIONS) && CUVISS_HOOKS_NO_EXCEPTIONS
        return Key();
#else
        throw std::overflow_error("Anchor key ID space exhausted");
#endif
    }
    const Key key = std::make_shared<KeyState>(
        registry->next_key_id++, name, contract, registry);
    registry->interned_keys.emplace(name, key);
    return key;
}

Anchor::Token Anchor::Register(const Key& key, std::function<void()> callback) {
    if (!callback) {
#if defined(CUVISS_HOOKS_NO_EXCEPTIONS) && CUVISS_HOOKS_NO_EXCEPTIONS
        return RegisterContextErased(key, ContextCallback());
#else
        throw std::invalid_argument("Anchor callback must not be empty");
#endif
    }
    if (key && key->context_contract_.type_id != 0) {
#if defined(CUVISS_HOOKS_NO_EXCEPTIONS) && CUVISS_HOOKS_NO_EXCEPTIONS
        return RegisterContextErased(Key(), ContextCallback());
#else
        throw std::invalid_argument(
            "Anchor context key requires a context callback");
#endif
    }
    return RegisterContextErased(key, [callback](void*) {
        callback();
    });
}

Anchor::Token Anchor::RegisterContextErased(
    const Key& key,
    ContextCallback callback) {
    if (!key) {
#if defined(CUVISS_HOOKS_NO_EXCEPTIONS) && CUVISS_HOOKS_NO_EXCEPTIONS
        return Token(nullptr, &TokenState::Destroy);
#else
        throw std::invalid_argument("Anchor key must not be null");
#endif
    }
    if (!callback) {
#if defined(CUVISS_HOOKS_NO_EXCEPTIONS) && CUVISS_HOOKS_NO_EXCEPTIONS
        return Token(nullptr, &TokenState::Destroy);
#else
        throw std::invalid_argument("Anchor callback must not be empty");
#endif
    }
    const std::shared_ptr<const Callback> callback_state =
        std::make_shared<Callback>(std::move(callback));
    const std::shared_ptr<Registry> registry = GetRegistryHandle();

    std::unique_ptr<TokenState> state;
    {
        std::lock_guard<std::mutex> lock(registry->mutex);
        const std::shared_ptr<const CallbackMap> retired =
            std::atomic_load(&registry->callbacks);
        const auto existing = retired->find(key->id_);
        if (existing != retired->end() && existing->second.state->IsActive()) {
            return Token(nullptr, &TokenState::Destroy);
        }
        if (registry->next_registration_id == 0) {
#if defined(CUVISS_HOOKS_NO_EXCEPTIONS) && CUVISS_HOOKS_NO_EXCEPTIONS
            return Token(nullptr, &TokenState::Destroy);
#else
            throw std::overflow_error("Anchor registration ID space exhausted");
#endif
        }

        const std::shared_ptr<RegistrationState> registration_state =
            std::make_shared<RegistrationState>(registry->next_registration_id++);
        state.reset(new TokenState(registry, key, registration_state));
        const std::shared_ptr<CallbackMap> next =
            std::make_shared<CallbackMap>(*retired);
        for (auto it = next->begin(); it != next->end();) {
            if (!it->second.state->IsActive()) {
                it = next->erase(it);
            } else {
                ++it;
            }
        }
        next->emplace(key->id_, CallbackEntry{registration_state, callback_state});

        const std::shared_ptr<const CallbackMap> published = next;
        std::atomic_store(&registry->callbacks, published);
        state->MarkPublished();
    }
    return Token(state.release(), &TokenState::Destroy);
}

void Anchor::Unregister(Token& token) noexcept {
    token.reset();
}

bool Anchor::Execute(const Key& key) {
    if (!key) {
        return false;
    }
    if (key->context_contract_.type_id != 0) {
#if defined(CUVISS_HOOKS_NO_EXCEPTIONS) && CUVISS_HOOKS_NO_EXCEPTIONS
        return false;
#else
        throw std::invalid_argument(
            "Anchor context key requires a context value");
#endif
    }

    const std::shared_ptr<Registry> registry = GetRegistryHandle();
    const std::shared_ptr<const CallbackMap> snapshot =
        std::atomic_load(&registry->callbacks);
    const auto it = snapshot->find(key->id_);
    if (it == snapshot->end() || !it->second.state->IsActive()) {
        return false;
    }

    Callback callback = *it->second.callback;
    callback(nullptr);
    return true;
}

bool Anchor::ExecuteContextErased(const Key& key, void* context) {
    if (!key) {
        return false;
    }
    if (context == nullptr) {
#if defined(CUVISS_HOOKS_NO_EXCEPTIONS) && CUVISS_HOOKS_NO_EXCEPTIONS
        return false;
#else
        throw std::invalid_argument("Anchor context must not be null");
#endif
    }

    const std::shared_ptr<Registry> registry = GetRegistryHandle();
    const std::shared_ptr<const CallbackMap> snapshot =
        std::atomic_load(&registry->callbacks);
    const auto it = snapshot->find(key->id_);
    if (it == snapshot->end()) {
        return false;
    }
    if (!it->second.state->IsActive()) {
        return false;
    }

    Callback callback = *it->second.callback;
    callback(context);
    return true;
}
