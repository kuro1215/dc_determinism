// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <nlohmann/json.hpp>
#include <xxhash.h>
#include "digitalcurling/moves/shot.hpp"
#include "digitalcurling/plugins/handle_id.hpp"
#include "digitalcurling/plugins/plugin_manager.hpp"
#include "digitalcurling/plugins/plugin_type.hpp"
#include "digitalcurling/plugins/loader.h"

// --- Macros ---
// The message prefix is the name of the C API function currently running inside `catch_exceptions`,
// so the macros also work in the lambdas that run the function body.
#define DIGITALCURLING_LOADER_RETURN_ERROR(err_code, err_msg) \
    do { \
        digitalcurling::plugins::detail::last_error_message = \
            std::string(digitalcurling::plugins::detail::current_function_name) + ": " + std::string(err_msg); \
        return err_code; \
    } while (0)

#define DIGITALCURLING_LOADER_CHECK_POINTER(arg) \
    do { \
        if ((arg) == nullptr) { \
            if constexpr (std::string_view{#arg}.compare(0, 4, "out_") == 0) \
            { \
                DIGITALCURLING_LOADER_RETURN_ERROR(DIGITALCURLING_ERR_BUFFER_NULLPTR, std::string(#arg) + " is nullptr."); \
            } else { \
                DIGITALCURLING_LOADER_RETURN_ERROR(DIGITALCURLING_ERR_INVALID_ARGUMENT, std::string(#arg) + " is nullptr."); \
            } \
        } \
    } while (0)

#define DIGITALCURLING_LOADER_CHECK_PLUGIN_RESULT(result) \
    do { \
        if (!(result)) { \
            auto err = (result).GetError(); \
            digitalcurling::plugins::detail::last_error_message = \
                std::string(digitalcurling::plugins::detail::current_function_name) + ": " + std::string(err.what()); \
            return static_cast<DigitalCurling_ErrorCode>(err.code()); \
        } \
    } while (0)


// --- Internal Helper Classes ---
namespace digitalcurling::plugins::detail {
    static thread_local std::string last_error_message;
    static thread_local const char* current_function_name = "";

    // Runs `func` (including its argument checks) so that no exception crosses the C boundary.
    // The last error message is cleared when the call succeeds.
    template <typename F>
    DigitalCurling_ErrorCode catch_exceptions(const char* func_name, F&& func) {
        // Assigning the message may itself throw (bad_alloc); in that case the old message is kept.
        auto set_last_error = [&](const char* msg) noexcept {
            try {
                last_error_message = std::string(func_name) + ": " + std::string(msg);
            } catch (...) {
            }
        };

        current_function_name = func_name;
        try {
            const DigitalCurling_ErrorCode code = func();
            if (code == DIGITALCURLING_OK) last_error_message.clear();
            return code;
        } catch (const nlohmann::json::exception& e) {
            set_last_error(e.what());
            return DIGITALCURLING_ERR_JSON_DATA;
        } catch (const digitalcurling::plugins::plugin_error& e) {
            set_last_error(e.what());
            return static_cast<DigitalCurling_ErrorCode>(e.code());
        } catch (const std::bad_alloc& e) {
            set_last_error(e.what());
            return DIGITALCURLING_ERR_MEMORY_ALLOCATION;
        } catch (const std::invalid_argument& e) {
            set_last_error(e.what());
            return DIGITALCURLING_ERR_INVALID_ARGUMENT;
        } catch (const std::exception& e) {
            set_last_error(e.what());
            return DIGITALCURLING_ERR_OTHER_EXCEPTION;
        } catch (...) {
            set_last_error("unknown exception");
            return DIGITALCURLING_ERR_UNKNOWN;
        }
    }

    class SnapshotManager {
    public:
        static SnapshotManager& GetInstance() {
            static SnapshotManager instance;
            return instance;
        }

        enum class GetDataResult {
            Ok,
            NotFound,
            BufferTooSmall,
        };

        digitalcurling::plugins::HandleId Create(std::string&& data) {
            auto id = digitalcurling::plugins::HandleId::Generate();
            std::unique_lock lock(mutex_);
            map_.emplace(id, std::move(data));
            return id;
        }

        GetDataResult TryGetData(const digitalcurling::plugins::HandleId& id, char* out_buffer, size_t buffer_size) {
            std::shared_lock lock(mutex_);
            auto it = map_.find(id);
            if (it == map_.end()) return GetDataResult::NotFound;
            if (buffer_size < it->second.size() + 1) return GetDataResult::BufferTooSmall;

            std::memcpy(out_buffer, it->second.data(), it->second.size());
            out_buffer[it->second.size()] = '\0';
            return GetDataResult::Ok;
        }

        bool Destroy(const digitalcurling::plugins::HandleId& id) {
            std::unique_lock lock(mutex_);
            return map_.erase(id) > 0;
        }

    private:
        SnapshotManager() = default;
        ~SnapshotManager() {
            std::unique_lock lock(mutex_);
            map_.clear();
        }
        SnapshotManager(const SnapshotManager&) = delete;
        SnapshotManager& operator=(const SnapshotManager&) = delete;

        std::shared_mutex mutex_;
        std::unordered_map<digitalcurling::plugins::HandleId, std::string> map_;
    };

    class InstanceManager {
    public:
        static InstanceManager& GetInstance() {
            static InstanceManager instance;
            return instance;
        }

        void Register(const digitalcurling::plugins::HandleId& id, std::weak_ptr<digitalcurling::plugins::detail::PluginResource> plugin) {
            std::unique_lock lock(mutex_);
            map_.emplace(id, plugin);
        }

        template<PluginType Type>
        std::shared_ptr<typename digitalcurling::plugins::PluginTypeTraits<Type>::ResourceType> Get(const digitalcurling::plugins::HandleId& id) const {
            std::shared_lock lock(mutex_);
            auto it = map_.find(id);
            if (it == map_.end()) return nullptr;
            return std::dynamic_pointer_cast<typename digitalcurling::plugins::PluginTypeTraits<Type>::ResourceType>(it->second.lock());
        }

        bool Unregister(const digitalcurling::plugins::HandleId& id) {
            std::unique_lock lock(mutex_);
            return map_.erase(id) > 0;
        }
        void UnregisterAll(std::shared_ptr<digitalcurling::plugins::detail::PluginResource> resource) {
            std::unique_lock lock(mutex_);
            auto ids = resource->GetInstanceList().GetAllId();
            for (auto& id : ids) { map_.erase(id); }
        }

    private:
        mutable std::shared_mutex mutex_;
        std::unordered_map<digitalcurling::plugins::HandleId, std::weak_ptr<digitalcurling::plugins::detail::PluginResource>> map_;
    };

    class LoaderInternalAccessor {
    public:
        static std::shared_ptr<PlayerPluginResource> GetPlayerResource(
            const PluginManager& manager, const std::string& name) {
            return manager.GetPlayerPluginResource(name);
        }

        static std::shared_ptr<SimulatorPluginResource> GetSimulatorResource(
            const PluginManager& manager, const std::string& name) {
            return manager.GetSimulatorPluginResource(name);
        }
    };
}

// --- Type Aliases ---
using PluginManager = digitalcurling::plugins::PluginManager;
using PluginType = digitalcurling::plugins::PluginType;
using PluginId = digitalcurling::plugins::PluginId;
using InstanceManager = digitalcurling::plugins::detail::InstanceManager;
using SnapshotManager = digitalcurling::plugins::detail::SnapshotManager;

// --- Error Handling ---
DigitalCurling_ErrorCode dc_loader_get_last_error_message(char* out_buffer, size_t buffer_size, size_t* out_required_size) {
     if (!out_required_size) return DIGITALCURLING_ERR_INVALID_ARGUMENT;

    *out_required_size = digitalcurling::plugins::detail::last_error_message.length() + 1;
    if (!out_buffer) return DIGITALCURLING_OK;

    if (buffer_size < *out_required_size) return DIGITALCURLING_ERR_BUFFER_TOO_SMALL;
    std::memcpy(
        out_buffer,
        digitalcurling::plugins::detail::last_error_message.c_str(),
        *out_required_size
    );
    return DIGITALCURLING_OK;
}

// --- Snapshot ---
DigitalCurling_ErrorCode dc_loader_snapshot_get_data(DigitalCurling_HandleId snapshot_id, char* out_buffer, size_t buffer_size) {
    return digitalcurling::plugins::detail::catch_exceptions(__func__, [&]() {
        DIGITALCURLING_LOADER_CHECK_POINTER(out_buffer);

        auto id = digitalcurling::plugins::HandleId::FromValue(snapshot_id);
        switch (SnapshotManager::GetInstance().TryGetData(id, out_buffer, buffer_size)) {
            case SnapshotManager::GetDataResult::Ok:
                return DIGITALCURLING_OK;
            case SnapshotManager::GetDataResult::NotFound:
                DIGITALCURLING_LOADER_RETURN_ERROR(DIGITALCURLING_ERR_INSTANCE_NOT_FOUND, "Snapshot id is not found.");
            case SnapshotManager::GetDataResult::BufferTooSmall:
                DIGITALCURLING_LOADER_RETURN_ERROR(DIGITALCURLING_ERR_BUFFER_TOO_SMALL, "out_buffer is too small.");
        }
        DIGITALCURLING_LOADER_RETURN_ERROR(DIGITALCURLING_ERR_UNKNOWN, "Unexpected snapshot retrieval state.");
    });
}
DigitalCurling_ErrorCode dc_loader_destroy_snapshot(DigitalCurling_HandleId snapshot_id) {
    return digitalcurling::plugins::detail::catch_exceptions(__func__, [&]() {

        auto id = digitalcurling::plugins::HandleId::FromValue(snapshot_id);
        auto success = SnapshotManager::GetInstance().Destroy(id);

        if (!success)
            DIGITALCURLING_LOADER_RETURN_ERROR(DIGITALCURLING_ERR_INSTANCE_NOT_FOUND, "Snapshot id is not found.");
        return DIGITALCURLING_OK;
    });
}

// --- Core Plugin Management ---
DigitalCurling_ErrorCode dc_loader_load_plugin(const char* plugin_path, DigitalCurling_PluginType* out_plugin_type, char* out_plugin_name, size_t out_name_buffer_size) {
    return digitalcurling::plugins::detail::catch_exceptions(__func__, [&]() {
        DIGITALCURLING_LOADER_CHECK_POINTER(plugin_path);
        DIGITALCURLING_LOADER_CHECK_POINTER(out_plugin_type);
        DIGITALCURLING_LOADER_CHECK_POINTER(out_plugin_name);

        std::filesystem::path decoded_path;
        try {
            decoded_path = std::filesystem::u8path(plugin_path);
        } catch (const std::exception&) {
            throw digitalcurling::plugins::plugin_error{
                DIGITALCURLING_ERR_FAILED_TO_LOAD_PLUGIN,
                "Plugin path is not valid UTF-8."
            };
        }
        auto resource = digitalcurling::plugins::detail::LoadPluginResource(decoded_path);
        const auto type = resource->GetType();
        const auto name = resource->GetName();
        if (name.length() >= out_name_buffer_size)
            DIGITALCURLING_LOADER_RETURN_ERROR(DIGITALCURLING_ERR_BUFFER_TOO_SMALL, "out_plugin_name buffer is too small.");

        PluginManager::GetInstance().RegisterPlugin(std::move(resource));
        *out_plugin_type = static_cast<DigitalCurling_PluginType>(type);
        std::memcpy(out_plugin_name, name.c_str(), name.length() + 1);
        return DIGITALCURLING_OK;
    });
}
DigitalCurling_ErrorCode dc_loader_get_loaded_plugins_snapshot(DigitalCurling_PluginType plugin_type, DigitalCurling_HandleId* out_snapshot_id, size_t* out_snapshot_size) {
    return digitalcurling::plugins::detail::catch_exceptions(__func__, [&]() {
        DIGITALCURLING_LOADER_CHECK_POINTER(out_snapshot_id);

        auto type = static_cast<digitalcurling::plugins::PluginType>(plugin_type);
        auto plugins = PluginManager::GetInstance().GetLoadedPlugins(type);

        nlohmann::json ids = nlohmann::json::array();
        ids.get_ptr<nlohmann::json::array_t*>()->reserve(plugins.size());
        for (const auto& [type, name] : plugins) ids.push_back(name);
        std::string snapshot_data = ids.dump();

        if (out_snapshot_size) *out_snapshot_size = snapshot_data.size() + 1;
        auto id = SnapshotManager::GetInstance().Create(std::move(snapshot_data));
        *out_snapshot_id = id.value();
        return DIGITALCURLING_OK;
    });
}
DigitalCurling_ErrorCode dc_loader_is_plugin_loaded(DigitalCurling_PluginType plugin_type, const char* plugin_name, bool* out_loaded) {
    return digitalcurling::plugins::detail::catch_exceptions(__func__, [&]() {
        DIGITALCURLING_LOADER_CHECK_POINTER(plugin_name);
        DIGITALCURLING_LOADER_CHECK_POINTER(out_loaded);

        auto type = static_cast<digitalcurling::plugins::PluginType>(plugin_type);
        *out_loaded = PluginManager::GetInstance().IsPluginLoaded(type, plugin_name);
        return DIGITALCURLING_OK;
    });
}

// --- Creator Common Functions ---
DigitalCurling_ErrorCode dc_loader_creator_get_state_snapshot(DigitalCurling_HandleId creator_id, DigitalCurling_HandleId* out_snapshot_id, size_t* out_snapshot_size) {
    return digitalcurling::plugins::detail::catch_exceptions(__func__, [&]() {
        DIGITALCURLING_LOADER_CHECK_POINTER(out_snapshot_id);

        auto id = digitalcurling::plugins::HandleId::FromValue(creator_id);
        std::shared_ptr<digitalcurling::plugins::detail::PluginResource> resource;
        if (auto player_res = InstanceManager::GetInstance().Get<PluginType::player>(id)) {
            resource = player_res;
        } else if (auto sim_res = InstanceManager::GetInstance().Get<PluginType::simulator>(id)) {
            resource = sim_res;
        }

        if (!resource)
            DIGITALCURLING_LOADER_RETURN_ERROR(DIGITALCURLING_ERR_INSTANCE_NOT_FOUND, "Creator instance not found.");

        auto result = resource->object_creator_get_state.ExecuteRaw(id);
        DIGITALCURLING_LOADER_CHECK_PLUGIN_RESULT(result);

        auto snapshot_data = result.GetValue();
        if (out_snapshot_size) *out_snapshot_size = snapshot_data.size() + 1;
        id = SnapshotManager::GetInstance().Create(std::move(snapshot_data));
        *out_snapshot_id = id.value();
        return DIGITALCURLING_OK;
    });
}

DigitalCurling_ErrorCode dc_loader_creator_compute_hash(DigitalCurling_HandleId creator_id, DigitalCurling_Uuid* out_hash) {
    return digitalcurling::plugins::detail::catch_exceptions(__func__, [&]() {
        DIGITALCURLING_LOADER_CHECK_POINTER(out_hash);

        auto id = digitalcurling::plugins::HandleId::FromValue(creator_id);
        std::shared_ptr<digitalcurling::plugins::detail::PluginResource> resource;
        if (auto player_res = InstanceManager::GetInstance().Get<PluginType::player>(id)) {
            resource = player_res;
        } else if (auto sim_res = InstanceManager::GetInstance().Get<PluginType::simulator>(id)) {
            resource = sim_res;
        }

        if (!resource)
            DIGITALCURLING_LOADER_RETURN_ERROR(DIGITALCURLING_ERR_INSTANCE_NOT_FOUND, "Creator instance not found.");

        auto result = resource->object_creator_get_state.ExecuteRaw(id);
        DIGITALCURLING_LOADER_CHECK_PLUGIN_RESULT(result);

        std::string state_json = result.GetValue();
        XXH128_hash_t hash = XXH3_128bits(state_json.data(), state_json.size());

        XXH128_canonical_t canonical;
        XXH128_canonicalFromHash(&canonical, hash);
        std::copy(std::begin(canonical.digest), std::end(canonical.digest), std::begin(out_hash->bytes));

        // Apply uuidv8 variant and version bits
        out_hash->bytes[6] = (out_hash->bytes[6] & 0x0F) | 0x80; // version
        out_hash->bytes[8] = (out_hash->bytes[8] & 0x3F) | 0x80; // variant
        return DIGITALCURLING_OK;
    });
}

// --- Player/Simulator Function Template ---
template <typename T>
DigitalCurling_ErrorCode create_plugin_object_impl(
    const char* func_name, const char* plugin_name, const char* json_config, DigitalCurling_HandleId* out_id
) {
    return digitalcurling::plugins::detail::catch_exceptions(func_name, [&]() {
        std::optional<digitalcurling::plugins::HandleId> id;
        std::shared_ptr<digitalcurling::plugins::detail::PluginResource> resource;

        constexpr auto Type = digitalcurling::plugins::PluginTypeFor<T>::value;
        if constexpr (Type == PluginType::player) {
            resource = digitalcurling::plugins::detail::LoaderInternalAccessor::GetPlayerResource(PluginManager::GetInstance(), plugin_name);
            if (!resource) DIGITALCURLING_LOADER_RETURN_ERROR(DIGITALCURLING_ERR_PLUGIN_NOT_FOUND, "Player plugin not found.");
        } else if constexpr (Type == PluginType::simulator) {
            resource = digitalcurling::plugins::detail::LoaderInternalAccessor::GetSimulatorResource(PluginManager::GetInstance(), plugin_name);
            if (!resource) DIGITALCURLING_LOADER_RETURN_ERROR(DIGITALCURLING_ERR_PLUGIN_NOT_FOUND, "Simulator plugin not found.");
        } else {
            static_assert(digitalcurling::plugins::always_false<T>, "Unsupported Type");
        }

        if constexpr (std::is_base_of_v<digitalcurling::plugins::FactoryHandle, T>) {
            id = resource->create_factory.Execute(json_config);
        } else if constexpr (std::is_base_of_v<digitalcurling::plugins::StorageHandle, T>) {
            id = resource->create_storage.Execute(json_config);
        } else {
            static_assert(digitalcurling::plugins::always_false<T>, "Unsupported Type");
        }

        if (!id.has_value())
            DIGITALCURLING_LOADER_RETURN_ERROR(DIGITALCURLING_ERR_UNKNOWN, "Failed to create plugin object instance.");

        InstanceManager::GetInstance().Register(*id, resource);
        *out_id = id->value();
        return DIGITALCURLING_OK;
    });
}

template <PluginType Type>
DigitalCurling_ErrorCode target_load_impl(const char* func_name, DigitalCurling_HandleId target_id, DigitalCurling_HandleId storage_id) {
    return digitalcurling::plugins::detail::catch_exceptions(func_name, [&]() {
        auto target = digitalcurling::plugins::HandleId::FromValue(target_id);
        auto storage = digitalcurling::plugins::HandleId::FromValue(storage_id);

        auto resource = InstanceManager::GetInstance().Get<Type>(target);
        if (!resource) DIGITALCURLING_LOADER_RETURN_ERROR(DIGITALCURLING_ERR_INSTANCE_NOT_FOUND, digitalcurling::ToString(Type, true) + " plugin not found.");

        auto result = resource->load.ExecuteRaw(target, storage);
        DIGITALCURLING_LOADER_CHECK_PLUGIN_RESULT(result);
        return DIGITALCURLING_OK;
    });
}
template <PluginType Type>
DigitalCurling_ErrorCode target_save_impl(const char* func_name, DigitalCurling_HandleId target_id, DigitalCurling_HandleId storage_id) {
    return digitalcurling::plugins::detail::catch_exceptions(func_name, [&]() {
        auto target = digitalcurling::plugins::HandleId::FromValue(target_id);
        auto storage = digitalcurling::plugins::HandleId::FromValue(storage_id);

        auto resource = InstanceManager::GetInstance().Get<Type>(target);
        if (!resource) DIGITALCURLING_LOADER_RETURN_ERROR(DIGITALCURLING_ERR_INSTANCE_NOT_FOUND, digitalcurling::ToString(Type, true) + " plugin not found.");

        auto result = resource->save.ExecuteRaw(target, storage);
        DIGITALCURLING_LOADER_CHECK_PLUGIN_RESULT(result);
        return DIGITALCURLING_OK;
    });
}

// --- Player Instance Management ---
DigitalCurling_ErrorCode dc_loader_create_player_factory(const char* plugin_name, const char* json_config, DigitalCurling_HandleId* out_factory_id) {
    return digitalcurling::plugins::detail::catch_exceptions(__func__, [&]() {
        DIGITALCURLING_LOADER_CHECK_POINTER(plugin_name);
        DIGITALCURLING_LOADER_CHECK_POINTER(out_factory_id);

        return create_plugin_object_impl<digitalcurling::players::IPlayerFactory>(
            __func__, plugin_name, json_config, out_factory_id
        );
    });
}
DigitalCurling_ErrorCode dc_loader_create_player_storage(const char* plugin_name, const char* json_config, DigitalCurling_HandleId* out_storage_id) {
    return digitalcurling::plugins::detail::catch_exceptions(__func__, [&]() {
        DIGITALCURLING_LOADER_CHECK_POINTER(plugin_name);
        DIGITALCURLING_LOADER_CHECK_POINTER(out_storage_id);

        return create_plugin_object_impl<digitalcurling::players::IPlayerStorage>(
            __func__, plugin_name, json_config, out_storage_id
        );
    });
}
DigitalCurling_ErrorCode dc_loader_create_player(DigitalCurling_HandleId creator_id, DigitalCurling_HandleId* out_player_id) {
    return digitalcurling::plugins::detail::catch_exceptions(__func__, [&]() {
        DIGITALCURLING_LOADER_CHECK_POINTER(out_player_id);

        auto id = digitalcurling::plugins::HandleId::FromValue(creator_id);
        auto resource = InstanceManager::GetInstance().Get<PluginType::player>(id);
        if (!resource)
            DIGITALCURLING_LOADER_RETURN_ERROR(DIGITALCURLING_ERR_INSTANCE_NOT_FOUND, "Player creator instance not found.");

        auto result = resource->create_target.ExecuteRaw(id);
        DIGITALCURLING_LOADER_CHECK_PLUGIN_RESULT(result);

        *out_player_id = result.GetValue().value();
        InstanceManager::GetInstance().Register(result.GetValue(), resource);
        return DIGITALCURLING_OK;
    });
}
DigitalCurling_ErrorCode dc_loader_remove_player_instance(DigitalCurling_HandleId instance_id) {
    return digitalcurling::plugins::detail::catch_exceptions(__func__, [&]() {

        auto id = digitalcurling::plugins::HandleId::FromValue(instance_id);
        auto resource = InstanceManager::GetInstance().Get<PluginType::player>(id);
        if (!resource || !resource->GetInstanceList().Remove(id))
            DIGITALCURLING_LOADER_RETURN_ERROR(DIGITALCURLING_ERR_INSTANCE_NOT_FOUND, "Player instance not found.");

        InstanceManager::GetInstance().Unregister(id);
        return DIGITALCURLING_OK;
    });
}

// --- Player Functions ---
DigitalCurling_ErrorCode dc_loader_player_load(DigitalCurling_HandleId player_id, DigitalCurling_HandleId storage_id) {
    return digitalcurling::plugins::detail::catch_exceptions(__func__, [&]() {

        return target_load_impl<PluginType::player>(__func__, player_id, storage_id);
    });
}
DigitalCurling_ErrorCode dc_loader_player_save(DigitalCurling_HandleId player_id, DigitalCurling_HandleId storage_id) {
    return digitalcurling::plugins::detail::catch_exceptions(__func__, [&]() {

        return target_save_impl<PluginType::player>(__func__, player_id, storage_id);
    });
}
DigitalCurling_ErrorCode dc_loader_player_get_gender(DigitalCurling_HandleId player_id, DigitalCurling_Gender* out_gender) {
    return digitalcurling::plugins::detail::catch_exceptions(__func__, [&]() {
        DIGITALCURLING_LOADER_CHECK_POINTER(out_gender);

        auto id = digitalcurling::plugins::HandleId::FromValue(player_id);
        auto resource = InstanceManager::GetInstance().Get<PluginType::player>(id);
        if (!resource)
            DIGITALCURLING_LOADER_RETURN_ERROR(DIGITALCURLING_ERR_INSTANCE_NOT_FOUND, "Player plugin not found.");

        auto result = resource->get_gender.ExecuteRaw(id);
        DIGITALCURLING_LOADER_CHECK_PLUGIN_RESULT(result);
        *out_gender = result.GetValue();
        return DIGITALCURLING_OK;
    });
}
DigitalCurling_ErrorCode dc_loader_player_play(DigitalCurling_HandleId player_id, const DigitalCurling_Shot* shot_info, DigitalCurling_Shot* out_shot) {
    return digitalcurling::plugins::detail::catch_exceptions(__func__, [&]() {
        DIGITALCURLING_LOADER_CHECK_POINTER(shot_info);
        DIGITALCURLING_LOADER_CHECK_POINTER(out_shot);

        auto id = digitalcurling::plugins::HandleId::FromValue(player_id);
        auto resource = InstanceManager::GetInstance().Get<PluginType::player>(id);
        if (!resource)
            DIGITALCURLING_LOADER_RETURN_ERROR(DIGITALCURLING_ERR_INSTANCE_NOT_FOUND, "Player plugin not found.");

        auto result = resource->play.ExecuteRaw(id, shot_info);
        DIGITALCURLING_LOADER_CHECK_PLUGIN_RESULT(result);
        *out_shot = result.GetValue();
        return DIGITALCURLING_OK;
    });
}

// --- Simulator Instance Management ---
DigitalCurling_ErrorCode dc_loader_create_simulator_factory(const char* plugin_name, const char* json_config, DigitalCurling_HandleId* out_factory_id) {
    return digitalcurling::plugins::detail::catch_exceptions(__func__, [&]() {
        DIGITALCURLING_LOADER_CHECK_POINTER(plugin_name);
        DIGITALCURLING_LOADER_CHECK_POINTER(out_factory_id);

        return create_plugin_object_impl<digitalcurling::simulators::ISimulatorFactory>(
            __func__, plugin_name, json_config, out_factory_id
        );
    });
}
DigitalCurling_ErrorCode dc_loader_create_simulator_storage(const char* plugin_name, const char* json_config, DigitalCurling_HandleId* out_storage_id) {
    return digitalcurling::plugins::detail::catch_exceptions(__func__, [&]() {
        DIGITALCURLING_LOADER_CHECK_POINTER(plugin_name);
        DIGITALCURLING_LOADER_CHECK_POINTER(out_storage_id);

        return create_plugin_object_impl<digitalcurling::simulators::ISimulatorStorage>(
            __func__, plugin_name, json_config, out_storage_id
        );
    });
}
DigitalCurling_ErrorCode dc_loader_create_simulator(DigitalCurling_HandleId creator_id, DigitalCurling_HandleId* out_simulator_id) {
    return digitalcurling::plugins::detail::catch_exceptions(__func__, [&]() {
        DIGITALCURLING_LOADER_CHECK_POINTER(out_simulator_id);

        auto id = digitalcurling::plugins::HandleId::FromValue(creator_id);
        auto resource = InstanceManager::GetInstance().Get<PluginType::simulator>(id);
        if (!resource)
            DIGITALCURLING_LOADER_RETURN_ERROR(DIGITALCURLING_ERR_INSTANCE_NOT_FOUND, "Simulator creator instance not found.");

        auto result = resource->create_target.ExecuteRaw(id);
        DIGITALCURLING_LOADER_CHECK_PLUGIN_RESULT(result);

        *out_simulator_id = result.GetValue().value();
        InstanceManager::GetInstance().Register(result.GetValue(), resource);
        return DIGITALCURLING_OK;
    });
}
DigitalCurling_ErrorCode dc_loader_remove_simulator_instance(DigitalCurling_HandleId instance_id) {
    return digitalcurling::plugins::detail::catch_exceptions(__func__, [&]() {

        auto id = digitalcurling::plugins::HandleId::FromValue(instance_id);
        auto resource = InstanceManager::GetInstance().Get<PluginType::simulator>(id);
        if (!resource || !resource->GetInstanceList().Remove(id))
            DIGITALCURLING_LOADER_RETURN_ERROR(DIGITALCURLING_ERR_INSTANCE_NOT_FOUND, "Simulator instance not found.");

        InstanceManager::GetInstance().Unregister(id);
        return DIGITALCURLING_OK;
    });
}

// --- Simulator Functions ---
DigitalCurling_ErrorCode dc_loader_simulator_load(DigitalCurling_HandleId simulator_id, DigitalCurling_HandleId storage_id) {
    return digitalcurling::plugins::detail::catch_exceptions(__func__, [&]() {

        return target_load_impl<PluginType::simulator>(__func__, simulator_id, storage_id);
    });
}
DigitalCurling_ErrorCode dc_loader_simulator_save(DigitalCurling_HandleId simulator_id, DigitalCurling_HandleId storage_id) {
    return digitalcurling::plugins::detail::catch_exceptions(__func__, [&]() {

        return target_save_impl<PluginType::simulator>(__func__, simulator_id, storage_id);
    });
}
DigitalCurling_ErrorCode dc_loader_simulator_set_stones(DigitalCurling_HandleId simulator_id, const DigitalCurling_StoneCoordinate* stones) {
    return digitalcurling::plugins::detail::catch_exceptions(__func__, [&]() {
        DIGITALCURLING_LOADER_CHECK_POINTER(stones);

        auto id = digitalcurling::plugins::HandleId::FromValue(simulator_id);
        auto resource = InstanceManager::GetInstance().Get<PluginType::simulator>(id);
        if (!resource)
            DIGITALCURLING_LOADER_RETURN_ERROR(DIGITALCURLING_ERR_INSTANCE_NOT_FOUND, "Simulator instance not found.");

        auto result = resource->set_stones.ExecuteRaw(id, stones);
        DIGITALCURLING_LOADER_CHECK_PLUGIN_RESULT(result);
        return DIGITALCURLING_OK;
    });
}
DigitalCurling_ErrorCode dc_loader_simulator_set_stone(DigitalCurling_HandleId simulator_id, const unsigned char stone_index, const DigitalCurling_StoneState* stone) {
    return digitalcurling::plugins::detail::catch_exceptions(__func__, [&]() {

        auto id = digitalcurling::plugins::HandleId::FromValue(simulator_id);
        auto resource = InstanceManager::GetInstance().Get<PluginType::simulator>(id);
        if (!resource)
            DIGITALCURLING_LOADER_RETURN_ERROR(DIGITALCURLING_ERR_INSTANCE_NOT_FOUND, "Simulator instance not found.");

        auto result = resource->set_stone.ExecuteRaw(id, stone_index, stone);
        DIGITALCURLING_LOADER_CHECK_PLUGIN_RESULT(result);
        return DIGITALCURLING_OK;
    });
}
DigitalCurling_ErrorCode dc_loader_simulator_step(DigitalCurling_HandleId simulator_id, const int frames, const float sheet_width) {
    return digitalcurling::plugins::detail::catch_exceptions(__func__, [&]() {

        auto id = digitalcurling::plugins::HandleId::FromValue(simulator_id);
        auto resource = InstanceManager::GetInstance().Get<PluginType::simulator>(id);
        if (!resource)
            DIGITALCURLING_LOADER_RETURN_ERROR(DIGITALCURLING_ERR_INSTANCE_NOT_FOUND, "Simulator instance not found.");

        if ((resource->GetCapabilities() & DIGITALCURLING_PLUGIN_CAPABILITY_STEP) == 0)
            DIGITALCURLING_LOADER_RETURN_ERROR(DIGITALCURLING_ERR_UNSUPPORTED_OPERATION, "The simulator does not support stepping.");

        auto result = resource->step.ExecuteRaw(id, frames, sheet_width);
        DIGITALCURLING_LOADER_CHECK_PLUGIN_RESULT(result);
        return DIGITALCURLING_OK;
    });
}
DigitalCurling_ErrorCode dc_loader_simulator_simulate(DigitalCurling_HandleId simulator_id, const float sheet_width) {
    return digitalcurling::plugins::detail::catch_exceptions(__func__, [&]() {
        auto id = digitalcurling::plugins::HandleId::FromValue(simulator_id);
        auto resource = InstanceManager::GetInstance().Get<PluginType::simulator>(id);
        if (!resource)
            DIGITALCURLING_LOADER_RETURN_ERROR(DIGITALCURLING_ERR_INSTANCE_NOT_FOUND, "Simulator instance not found.");

        auto result = resource->simulate.ExecuteRaw(id, sheet_width);
        DIGITALCURLING_LOADER_CHECK_PLUGIN_RESULT(result);
        return DIGITALCURLING_OK;
    });
}
DigitalCurling_ErrorCode dc_loader_simulator_get_stones(DigitalCurling_HandleId simulator_id, DigitalCurling_StoneCoordinate* out_stones) {
    return digitalcurling::plugins::detail::catch_exceptions(__func__, [&]() {
        DIGITALCURLING_LOADER_CHECK_POINTER(out_stones);

        auto id = digitalcurling::plugins::HandleId::FromValue(simulator_id);
        auto resource = InstanceManager::GetInstance().Get<PluginType::simulator>(id);
        if (!resource)
            DIGITALCURLING_LOADER_RETURN_ERROR(DIGITALCURLING_ERR_INSTANCE_NOT_FOUND, "Simulator instance not found.");

        auto result = resource->get_stones.ExecuteRaw(id);
        DIGITALCURLING_LOADER_CHECK_PLUGIN_RESULT(result);
        *out_stones = result.GetValue();
        return DIGITALCURLING_OK;
    });
}
DigitalCurling_ErrorCode dc_loader_simulator_are_all_stones_stopped(DigitalCurling_HandleId simulator_id, bool* out_are_stopped) {
    return digitalcurling::plugins::detail::catch_exceptions(__func__, [&]() {
        DIGITALCURLING_LOADER_CHECK_POINTER(out_are_stopped);

        auto id = digitalcurling::plugins::HandleId::FromValue(simulator_id);
        auto resource = InstanceManager::GetInstance().Get<PluginType::simulator>(id);
        if (!resource)
            DIGITALCURLING_LOADER_RETURN_ERROR(DIGITALCURLING_ERR_INSTANCE_NOT_FOUND, "Simulator instance not found.");

        if ((resource->GetCapabilities() & DIGITALCURLING_PLUGIN_CAPABILITY_STEP) == 0)
            DIGITALCURLING_LOADER_RETURN_ERROR(DIGITALCURLING_ERR_UNSUPPORTED_OPERATION, "The simulator does not support stepping.");

        auto result = resource->are_all_stones_stopped.ExecuteRaw(id);
        DIGITALCURLING_LOADER_CHECK_PLUGIN_RESULT(result);
        *out_are_stopped = result.GetValue();
        return DIGITALCURLING_OK;
    });
}
DigitalCurling_ErrorCode dc_loader_simulator_get_seconds_per_frame(DigitalCurling_HandleId simulator_id, float* out_seconds) {
    return digitalcurling::plugins::detail::catch_exceptions(__func__, [&]() {
        DIGITALCURLING_LOADER_CHECK_POINTER(out_seconds);

        auto id = digitalcurling::plugins::HandleId::FromValue(simulator_id);
        auto resource = InstanceManager::GetInstance().Get<PluginType::simulator>(id);
        if (!resource)
            DIGITALCURLING_LOADER_RETURN_ERROR(DIGITALCURLING_ERR_INSTANCE_NOT_FOUND, "Simulator instance not found.");

        if ((resource->GetCapabilities() & DIGITALCURLING_PLUGIN_CAPABILITY_STEP) == 0)
            DIGITALCURLING_LOADER_RETURN_ERROR(DIGITALCURLING_ERR_UNSUPPORTED_OPERATION, "The simulator does not support stepping.");

        auto result = resource->get_seconds_per_frame.ExecuteRaw(id);
        DIGITALCURLING_LOADER_CHECK_PLUGIN_RESULT(result);
        *out_seconds = result.GetValue();
        return DIGITALCURLING_OK;
    });
}
DigitalCurling_ErrorCode dc_loader_simulator_get_capabilities(DigitalCurling_HandleId simulator_id, DigitalCurling_PluginCapabilities* out_capabilities) {
    return digitalcurling::plugins::detail::catch_exceptions(__func__, [&]() {
        DIGITALCURLING_LOADER_CHECK_POINTER(out_capabilities);

        auto id = digitalcurling::plugins::HandleId::FromValue(simulator_id);
        auto resource = InstanceManager::GetInstance().Get<PluginType::simulator>(id);
        if (!resource) DIGITALCURLING_LOADER_RETURN_ERROR(DIGITALCURLING_ERR_INSTANCE_NOT_FOUND, "Simulator instance not found.");
        *out_capabilities = resource->GetCapabilities();
        return DIGITALCURLING_OK;
    });
}
DigitalCurling_ErrorCode dc_loader_simulator_calculate_shot(DigitalCurling_HandleId simulator_id, const DigitalCurling_Vector2* target_position,
                                                            float target_speed, float angular_velocity, DigitalCurling_Shot* out_shot) {
    return digitalcurling::plugins::detail::catch_exceptions(__func__, [&]() {
        DIGITALCURLING_LOADER_CHECK_POINTER(target_position);
        DIGITALCURLING_LOADER_CHECK_POINTER(out_shot);

        auto id = digitalcurling::plugins::HandleId::FromValue(simulator_id);
        auto resource = InstanceManager::GetInstance().Get<PluginType::simulator>(id);
        if (!resource)
            DIGITALCURLING_LOADER_RETURN_ERROR(DIGITALCURLING_ERR_INSTANCE_NOT_FOUND, "Simulator instance not found.");
        if ((resource->GetCapabilities() & DIGITALCURLING_PLUGIN_CAPABILITY_CALCULATE_SHOT) == 0) {
            DIGITALCURLING_LOADER_RETURN_ERROR(DIGITALCURLING_ERR_UNSUPPORTED_OPERATION, "The simulator does not support calculating a shot.");
        }

        auto result = resource->calculate_shot.ExecuteRaw(id, target_position, target_speed, angular_velocity);
        DIGITALCURLING_LOADER_CHECK_PLUGIN_RESULT(result);
        *out_shot = result.GetValue();
        return DIGITALCURLING_OK;
    });
}
