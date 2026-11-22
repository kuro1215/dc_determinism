#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <vector>
#include <variant>
#include "digitalcurling/plugins/handle_id.hpp"
#include "digitalcurling/plugins/i_plugin_object.hpp"

namespace digitalcurling::plugins::detail {

struct PluginInstanceList {
public:
    template<typename T>
    std::optional<HandleId> Add(std::shared_ptr<T> instance) {
        std::unique_lock lock(mutex_);
        auto id = HandleId::Generate();

        bool inserted = false;
        if constexpr (std::is_base_of_v<FactoryHandle, T>) {
            inserted = instances_.emplace(id, std::static_pointer_cast<FactoryHandle>(instance)).second;
        } else if constexpr (std::is_base_of_v<StorageHandle, T>) {
            inserted = instances_.emplace(id, std::static_pointer_cast<StorageHandle>(instance)).second;
        } else if constexpr (std::is_base_of_v<TargetHandle, T>) {
            inserted = instances_.emplace(id, std::static_pointer_cast<TargetHandle>(instance)).second;
        } else {
            static_assert(always_false<T>, "Unsupported type for Add");
        }
        if (!inserted) return std::nullopt;
        return id;
    }

    template<typename T>
    std::shared_ptr<T> Get(const HandleId& id) const {
        std::shared_lock lock(mutex_);
        auto it = instances_.find(id);
        if (it == instances_.end()) return nullptr;

        return std::visit([](auto&& ptr) -> std::shared_ptr<T> {
            return std::dynamic_pointer_cast<T>(ptr);
        }, it->second);
    }

    std::vector<HandleId> GetAllId() const {
        std::shared_lock lock(mutex_);
        std::vector<HandleId> result;
        for (const auto& [id, info] : instances_) {
            result.push_back(id);
        }
        return result;
    }

    bool Exists(const HandleId& id) const {
        std::shared_lock lock(mutex_);
        return instances_.find(id) != instances_.end();
    }

    void Clear() {
        std::unique_lock lock(mutex_);
        instances_.clear();
    }
    bool Remove(const HandleId& id) {
        std::unique_lock lock(mutex_);
        return instances_.erase(id) > 0;
    }

private:
    using InstanceVariant = std::variant<
        std::shared_ptr<FactoryHandle>,
        std::shared_ptr<StorageHandle>,
        std::shared_ptr<TargetHandle>
    >;

    mutable std::shared_mutex mutex_;
    std::unordered_map<HandleId, InstanceVariant> instances_;
};

} // namespace digitalcurling::plugins::detail
