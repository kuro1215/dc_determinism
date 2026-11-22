// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Define the WrapperBase template class

#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include "digitalcurling/plugins/handle_id.hpp"
#include "digitalcurling/plugins/plugin_type.hpp"
#include "digitalcurling/plugins/detail/plugin_resource.hpp"

namespace digitalcurling::plugins {

/// @brief Base class for wrappers around plugin objects
/// @tparam TBase Base class of the wrapped object
template <typename TBase>
class WrapperBase : public TBase {
public:
    virtual ~WrapperBase() {
        if (auto resource = owner_resource_.lock()) {
            resource->GetInstanceList().Remove(instance_id_);
        }
    }

    /// @cond Doxygen_Suppress
    // Delete copy constructors
    WrapperBase(WrapperBase const&) = delete;
    WrapperBase& operator = (WrapperBase const&) = delete;
    /// @endcond

    /// @brief Get the instance ID
    /// @returns Instance ID
    HandleId GetInstanceId() const { return instance_id_; }

    /// @inheritdoc
    virtual const char* GetId() const noexcept override { return id_.c_str(); }

protected:
    /// @brief The plugin type of this wrapper class
    static constexpr PluginType Type = PluginTypeFor<TBase>::value;
    /// @brief The resource type that manages this wrapper class
    using OwnerResource = typename PluginTypeTraits<Type>::ResourceType;

    /// @brief Constructor
    /// @param id Plugin ID
    /// @param instance_id Instance ID
    /// @param owner_resource Reference to the resource that owns the actual instance
    WrapperBase(std::string id, HandleId instance_id, std::weak_ptr<OwnerResource> owner_resource)
        : id_(std::move(id)), instance_id_(std::move(instance_id)), owner_resource_(std::move(owner_resource)) {}

    /// @brief Get the plugin ID
    /// @return Plugin ID
    inline std::string const& GetPluginId() const { return id_; }

    /// @brief Utility to execute a function that operates on a plugin resource
    /// @tparam T Return type of the function
    /// @param func Function to execute
    /// @return Return value of the function
    template<typename T>
    T ExecuteResourceFunc(std::function<T(std::shared_ptr<OwnerResource>)> func) const {
        std::lock_guard<std::mutex> lock(resource_mutex_);
        return func(GetOwnerResource());
    }

private:
    std::string id_;
    HandleId instance_id_;
    std::weak_ptr<OwnerResource> owner_resource_;
    mutable std::mutex resource_mutex_;

    inline std::shared_ptr<OwnerResource> GetOwnerResource() const {
        auto resource = owner_resource_.lock();
        if (!resource)
            throw std::runtime_error("Owner resource has been destroyed for " + ToString(Type) + ": " + id_);
        return resource;
    }
};

} // namespace digitalcurling::plugins
