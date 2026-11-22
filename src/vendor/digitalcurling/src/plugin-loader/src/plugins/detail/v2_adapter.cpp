// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

// This C++ adapter owns opaque v2 values. No adapter type crosses the C ABI.
#include <cstddef>
#include <cstring>
#include <string.h> // strnlen
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "digitalcurling/plugins/detail/plugin_resource.hpp"
#include "utf8_path.hpp"

namespace digitalcurling::plugins::detail {
namespace {

constexpr DigitalCurling_ErrorCode kUnsupported = static_cast<DigitalCurling_ErrorCode>(14);

struct V2Backend {
    const DigitalCurling_PluginApiV2* api;
    std::optional<ModulePtr> module;
    PlayerApi player{};
    SimulatorApi simulator{};
};

char* host_copy(const char* value) {
    if (!value) return nullptr;
    const auto size = std::strlen(value);
    auto result = std::make_unique<char[]>(size + 1);
    std::memcpy(result.get(), value, size + 1);
    return result.release();
}

void host_free(char* value) { delete[] value; }

class RemoteString {
public:
    explicit RemoteString(V2Backend& backend) : backend_(backend) {}
    RemoteString(const RemoteString&) = delete;
    RemoteString& operator=(const RemoteString&) = delete;
    ~RemoteString() { if (value_) backend_.api->free_string(value_); }

    char** out() { return &value_; }
    const char* get() const { return value_; }
    std::unique_ptr<char[]> CopyToHost() const {
        if (!value_) return {};
        const auto size = std::strlen(value_);
        auto result = std::make_unique<char[]>(size + 1);
        std::memcpy(result.get(), value_, size + 1);
        return result;
    }

private:
    V2Backend& backend_;
    char* value_ = nullptr;
};

class RemoteHandle {
public:
    RemoteHandle(V2Backend& backend, DigitalCurling_PluginV2Destroy destroy)
        : backend_(backend), destroy_(destroy) {}
    RemoteHandle(const RemoteHandle&) = delete;
    RemoteHandle& operator=(const RemoteHandle&) = delete;
    ~RemoteHandle() { if (value_) destroy_(value_); }

    void** out() { return &value_; }
    explicit operator bool() const { return value_ != nullptr; }
    void* get() const { return value_; }
    void* release() { return std::exchange(value_, nullptr); }

private:
    V2Backend& backend_;
    DigitalCurling_PluginV2Destroy destroy_;
    void* value_ = nullptr;
};

struct CreatorBinding { PluginInstanceList* instances = nullptr; };

struct V2CreatorProxy {
    std::shared_ptr<V2Backend> backend;
    void* remote;
    V2CreatorProxy(std::shared_ptr<V2Backend> backend_, void* remote_)
        : backend(std::move(backend_)), remote(remote_) {}
    virtual ~V2CreatorProxy() = default;
};

struct V2FactoryProxy final : FactoryHandle, V2CreatorProxy {
    V2FactoryProxy(std::shared_ptr<V2Backend> backend, void* remote)
        : V2CreatorProxy(std::move(backend), remote) {}
    ~V2FactoryProxy() override { backend->api->destroy_factory(remote); }
};

struct V2StorageProxy final : StorageHandle, V2CreatorProxy {
    V2StorageProxy(std::shared_ptr<V2Backend> backend, void* remote)
        : V2CreatorProxy(std::move(backend), remote) {}
    ~V2StorageProxy() override { backend->api->destroy_storage(remote); }
};

struct V2TargetProxy {
    std::shared_ptr<V2Backend> backend;
    void* remote;
    V2TargetProxy(std::shared_ptr<V2Backend> backend_, void* remote_)
        : backend(std::move(backend_)), remote(remote_) {}
    virtual ~V2TargetProxy() = default;
};

struct V2PlayerProxy final : PlayerHandle, V2TargetProxy {
    V2PlayerProxy(std::shared_ptr<V2Backend> backend, void* remote)
        : V2TargetProxy(std::move(backend), remote) {}
    ~V2PlayerProxy() override { backend->api->destroy_target(remote); }
};

struct V2SimulatorProxy final : SimulatorHandle, V2TargetProxy {
    V2SimulatorProxy(std::shared_ptr<V2Backend> backend, void* remote)
        : V2TargetProxy(std::move(backend), remote) {}
    ~V2SimulatorProxy() override { backend->api->destroy_target(remote); }
};

template <class T, class U>
T* proxy(U* value) { return dynamic_cast<T*>(value); }

template <class T>
T* target(TargetHandle* value) { return dynamic_cast<T*>(value); }

DigitalCurling_ErrorCode finish(
    DigitalCurling_PluginV2Status status, const RemoteString& remote_error, char** out_error
) {
    if (out_error) {
        *out_error = status == DIGITALCURLING_PLUGIN_V2_OK ? nullptr : host_copy(remote_error.get());
    }
    return static_cast<DigitalCurling_ErrorCode>(status);
}

template <class P>
DigitalCurling_ErrorCode bad(P*, char** error) {
    if (error) *error = host_copy("Invalid v2 plugin handle.");
    return DIGITALCURLING_ERR_INVALID_ARGUMENT;
}

DigitalCurling_ErrorCode null_handle(char** error, const char* message) {
    if (error) *error = host_copy(message);
    return DIGITALCURLING_ERR_INVALID_DATA;
}

void destroy_creator(void* value) { delete static_cast<CreatorHandle*>(value); }
void destroy_target(void* value) { delete static_cast<TargetHandle*>(value); }

DigitalCurling_ErrorCode get_state(CreatorHandle* creator, char** out, char** error) {
    auto value = proxy<V2CreatorProxy>(creator);
    if (!value) return bad(creator, error);
    RemoteString result(*value->backend);
    RemoteString remote_error(*value->backend);
    const auto status = value->backend->api->object_creator_get_state(
        value->remote, result.out(), remote_error.out());
    auto host_result = status == DIGITALCURLING_PLUGIN_V2_OK ? result.CopyToHost() : nullptr;
    const auto final_status = finish(status, remote_error, error);
    if (out) *out = host_result.release();
    return final_status;
}

DigitalCurling_ErrorCode factory_set(FactoryHandle* creator, const char* json, char** error) {
    auto value = proxy<V2FactoryProxy>(creator);
    if (!value) return bad(creator, error);
    RemoteString remote_error(*value->backend);
    const auto status = value->backend->api->factory_set_state(value->remote, json, remote_error.out());
    return finish(status, remote_error, error);
}

DigitalCurling_ErrorCode storage_set(StorageHandle* creator, const char* json, char** error) {
    auto value = proxy<V2StorageProxy>(creator);
    if (!value) return bad(creator, error);
    RemoteString remote_error(*value->backend);
    const auto status = value->backend->api->storage_set_state(value->remote, json, remote_error.out());
    return finish(status, remote_error, error);
}

DigitalCurling_ErrorCode v2_create_target(CreatorHandle* creator, TargetHandle** out, char** error) {
    auto value = proxy<V2CreatorProxy>(creator);
    if (!value || !out) return bad(creator, error);
    RemoteHandle remote(*value->backend, value->backend->api->destroy_target);
    RemoteString remote_error(*value->backend);
    const auto status = value->backend->api->create_target(
        value->remote, remote.out(), remote_error.out());
    if (status != DIGITALCURLING_PLUGIN_V2_OK) return finish(status, remote_error, error);
    if (!remote) return null_handle(error, "v2 plugin returned a null target handle.");

    std::unique_ptr<TargetHandle> proxy_handle;
    if (value->backend->api->plugin_kind == DIGITALCURLING_PLUGIN_V2_PLAYER) {
        proxy_handle = std::make_unique<V2PlayerProxy>(value->backend, remote.get());
    } else {
        proxy_handle = std::make_unique<V2SimulatorProxy>(value->backend, remote.get());
    }
    remote.release();
    const auto result = finish(status, remote_error, error);
    *out = proxy_handle.release();
    return result;
}

template <class TargetProxy>
DigitalCurling_ErrorCode get_factory(
    TargetHandle* handle, FactoryHandle** out, char** error, DigitalCurling_PluginV2GetFactory callback
) {
    auto value = target<TargetProxy>(handle);
    if (!value || !out) return bad(handle, error);
    RemoteHandle remote(*value->backend, value->backend->api->destroy_factory);
    RemoteString remote_error(*value->backend);
    const auto status = callback(value->remote, remote.out(), remote_error.out());
    if (status != DIGITALCURLING_PLUGIN_V2_OK) return finish(status, remote_error, error);
    if (!remote) return null_handle(error, "v2 plugin returned a null factory handle.");

    auto proxy_handle = std::make_unique<V2FactoryProxy>(value->backend, remote.get());
    remote.release();
    const auto result = finish(status, remote_error, error);
    *out = proxy_handle.release();
    return result;
}

DigitalCurling_ErrorCode player_factory(PlayerHandle* handle, FactoryHandle** out, char** error) {
    auto value = target<V2PlayerProxy>(handle);
    if (!value) return bad(handle, error);
    return get_factory<V2PlayerProxy>(handle, out, error, value->backend->api->player->get_factory);
}

DigitalCurling_ErrorCode player_save(PlayerHandle* handle, StorageHandle* storage, char** error) {
    auto player = target<V2PlayerProxy>(handle);
    auto value = proxy<V2StorageProxy>(storage);
    if (!player || !value || player->backend.get() != value->backend.get()) return bad(handle, error);
    RemoteString remote_error(*player->backend);
    const auto status = player->backend->api->player->save(player->remote, value->remote, remote_error.out());
    return finish(status, remote_error, error);
}

DigitalCurling_ErrorCode player_load(PlayerHandle* handle, StorageHandle* storage, char** error) {
    auto player = target<V2PlayerProxy>(handle);
    auto value = proxy<V2StorageProxy>(storage);
    if (!player || !value || player->backend.get() != value->backend.get()) return bad(handle, error);
    RemoteString remote_error(*player->backend);
    const auto status = player->backend->api->player->load(player->remote, value->remote, remote_error.out());
    return finish(status, remote_error, error);
}

DigitalCurling_ErrorCode player_gender(PlayerHandle* handle, DigitalCurling_Gender* out, char** error) {
    auto value = target<V2PlayerProxy>(handle);
    if (!value || !out) return bad(handle, error);
    int32_t gender = 0;
    RemoteString remote_error(*value->backend);
    const auto status = value->backend->api->player->get_gender(value->remote, &gender, remote_error.out());
    if (status == DIGITALCURLING_PLUGIN_V2_OK) *out = static_cast<DigitalCurling_Gender>(gender);
    return finish(status, remote_error, error);
}

DigitalCurling_ErrorCode player_play(PlayerHandle* handle, const DigitalCurling_Shot* input,
                                     DigitalCurling_Shot* out, char** error) {
    auto value = target<V2PlayerProxy>(handle);
    if (!value || !input || !out) return bad(handle, error);
    DigitalCurling_PluginV2Shot request{
        input->translational_velocity, input->angular_velocity, input->release_angle};
    DigitalCurling_PluginV2Shot response{};
    RemoteString remote_error(*value->backend);
    const auto status = value->backend->api->player->play(
        value->remote, &request, &response, remote_error.out());
    if (status == DIGITALCURLING_PLUGIN_V2_OK) {
        *out = {response.translational_velocity, response.angular_velocity, response.release_angle};
    }
    return finish(status, remote_error, error);
}

DigitalCurling_ErrorCode sim_factory(SimulatorHandle* handle, FactoryHandle** out, char** error) {
    auto value = target<V2SimulatorProxy>(handle);
    if (!value) return bad(handle, error);
    return get_factory<V2SimulatorProxy>(handle, out, error, value->backend->api->simulator->get_factory);
}

DigitalCurling_ErrorCode sim_save(SimulatorHandle* handle, StorageHandle* storage, char** error) {
    auto simulator = target<V2SimulatorProxy>(handle);
    auto value = proxy<V2StorageProxy>(storage);
    if (!simulator || !value || simulator->backend.get() != value->backend.get()) return bad(handle, error);
    RemoteString remote_error(*simulator->backend);
    const auto status = simulator->backend->api->simulator->save(simulator->remote, value->remote, remote_error.out());
    return finish(status, remote_error, error);
}

DigitalCurling_ErrorCode sim_load(SimulatorHandle* handle, StorageHandle* storage, char** error) {
    auto simulator = target<V2SimulatorProxy>(handle);
    auto value = proxy<V2StorageProxy>(storage);
    if (!simulator || !value || simulator->backend.get() != value->backend.get()) return bad(handle, error);
    RemoteString remote_error(*simulator->backend);
    const auto status = simulator->backend->api->simulator->load(simulator->remote, value->remote, remote_error.out());
    return finish(status, remote_error, error);
}

DigitalCurling_ErrorCode sim_step(SimulatorHandle* handle, int frames, float width, char** error) {
    auto value = target<V2SimulatorProxy>(handle);
    if (!value) return bad(handle, error);
    if (!value->backend->api->simulator->step) return kUnsupported;
    RemoteString remote_error(*value->backend);
    const auto status = value->backend->api->simulator->step(value->remote, frames, width, remote_error.out());
    return finish(status, remote_error, error);
}

DigitalCurling_ErrorCode sim_simulate(SimulatorHandle* handle, float width, char** error) {
    auto value = target<V2SimulatorProxy>(handle);
    if (!value) return bad(handle, error);
    RemoteString remote_error(*value->backend);
    const auto status = value->backend->api->simulator->simulate(value->remote, width, remote_error.out());
    return finish(status, remote_error, error);
}

DigitalCurling_ErrorCode sim_stones(SimulatorHandle* handle, const DigitalCurling_StoneCoordinate* input,
                                    char** error) {
    auto value = target<V2SimulatorProxy>(handle);
    if (!value || !input) return bad(handle, error);
    DigitalCurling_PluginV2StoneCoordinate stones{};
    for (unsigned int index = 0; index < 16; ++index) {
        const auto& stone = input->stones[index];
        stones.stones[index] = {{stone.position.x, stone.position.y}, stone.angle,
                                {stone.translational_velocity.x, stone.translational_velocity.y},
                                stone.angular_velocity};
    }
    RemoteString remote_error(*value->backend);
    const auto status = value->backend->api->simulator->set_stones(value->remote, &stones, remote_error.out());
    return finish(status, remote_error, error);
}

DigitalCurling_ErrorCode sim_stone(SimulatorHandle* handle, unsigned char index,
                                   const DigitalCurling_StoneState* input, char** error) {
    auto value = target<V2SimulatorProxy>(handle);
    if (!value) return bad(handle, error);
    DigitalCurling_PluginV2StoneState stone{};
    const auto* request = input
        ? &(stone = DigitalCurling_PluginV2StoneState{{input->position.x, input->position.y}, input->angle,
                                                       {input->translational_velocity.x, input->translational_velocity.y},
                                                       input->angular_velocity})
        : nullptr;
    RemoteString remote_error(*value->backend);
    const auto status = value->backend->api->simulator->set_stone(value->remote, index, request, remote_error.out());
    return finish(status, remote_error, error);
}

DigitalCurling_ErrorCode sim_stopped(SimulatorHandle* handle, bool* out, char** error) {
    auto value = target<V2SimulatorProxy>(handle);
    if (!value || !out) return bad(handle, error);
    if (!value->backend->api->simulator->are_all_stones_stopped) return kUnsupported;
    DigitalCurling_PluginV2Bool stopped = 0;
    RemoteString remote_error(*value->backend);
    const auto status = value->backend->api->simulator->are_all_stones_stopped(
        value->remote, &stopped, remote_error.out());
    if (status == DIGITALCURLING_PLUGIN_V2_OK) *out = stopped != 0;
    return finish(status, remote_error, error);
}

DigitalCurling_ErrorCode sim_get_stones(SimulatorHandle* handle, DigitalCurling_StoneCoordinate* out,
                                        char** error) {
    auto value = target<V2SimulatorProxy>(handle);
    if (!value || !out) return bad(handle, error);
    DigitalCurling_PluginV2StoneCoordinate stones{};
    RemoteString remote_error(*value->backend);
    const auto status = value->backend->api->simulator->get_stones(value->remote, &stones, remote_error.out());
    if (status == DIGITALCURLING_PLUGIN_V2_OK) {
        for (unsigned int index = 0; index < 16; ++index) {
            const auto& source = stones.stones[index];
            out->stones[index] = {{source.position.x, source.position.y}, source.angle,
                                  {source.translational_velocity.x, source.translational_velocity.y},
                                  source.angular_velocity};
        }
    }
    return finish(status, remote_error, error);
}

DigitalCurling_ErrorCode sim_seconds(SimulatorHandle* handle, float* out, char** error) {
    auto value = target<V2SimulatorProxy>(handle);
    if (!value || !out) return bad(handle, error);
    if (!value->backend->api->simulator->get_seconds_per_frame) return kUnsupported;
    RemoteString remote_error(*value->backend);
    const auto status = value->backend->api->simulator->get_seconds_per_frame(
        value->remote, out, remote_error.out());
    return finish(status, remote_error, error);
}

DigitalCurling_ErrorCode sim_shot(SimulatorHandle* handle, const DigitalCurling_Vector2* position,
                                  float speed, float angular, DigitalCurling_Shot* out, char** error) {
    auto value = target<V2SimulatorProxy>(handle);
    if (!value || !position || !out) return bad(handle, error);
    if (!value->backend->api->simulator->calculate_shot) return kUnsupported;
    DigitalCurling_PluginV2Vector2 request{position->x, position->y};
    DigitalCurling_PluginV2Shot response{};
    RemoteString remote_error(*value->backend);
    const auto status = value->backend->api->simulator->calculate_shot(
        value->remote, &request, speed, angular, &response, remote_error.out());
    if (status == DIGITALCURLING_PLUGIN_V2_OK) {
        *out = {response.translational_velocity, response.angular_velocity, response.release_angle};
    }
    return finish(status, remote_error, error);
}

constexpr std::size_t kMaxPluginNameSize = 256;

// A valid name is 1 to 255 bytes of UTF-8 without ASCII control characters.
// The scan is bounded because the plugin is not trusted to NUL-terminate the name.
bool is_valid_plugin_name(const char* name) {
    const auto size = strnlen(name, kMaxPluginNameSize);
    if (size == 0 || size >= kMaxPluginNameSize) return false;
    for (std::size_t i = 0; i < size; ++i) {
        const auto c = static_cast<unsigned char>(name[i]);
        if (c < 0x20 || c == 0x7F) return false;
    }
    return IsValidUtf8(std::string(name, size));
}

bool validate(const DigitalCurling_PluginApiV2* api) {
    // struct_size covers the descriptor only; the sub-table sizes below make a layout change of a
    // sub-table visible as well (they are part of the descriptor, so a stale plugin fails struct_size).
    if (!api || api->abi_version != DIGITALCURLING_PLUGIN_ABI_V2 || api->struct_size != sizeof(*api) ||
        !api->plugin_name || !is_valid_plugin_name(api->plugin_name) ||
        !api->free_string || !api->create_factory || !api->create_storage || !api->destroy_factory ||
        !api->destroy_storage || !api->create_target || !api->destroy_target ||
        !api->object_creator_get_state || !api->factory_set_state || !api->storage_set_state) return false;
    if (api->plugin_kind == DIGITALCURLING_PLUGIN_V2_PLAYER) {
        return api->player_api_size == sizeof(DigitalCurling_PluginV2PlayerApi) && api->simulator_api_size == 0u &&
               api->player && !api->simulator && api->player->get_factory && api->player->save &&
               api->player->load && api->player->get_gender && api->player->play;
    }
    if (api->plugin_kind == DIGITALCURLING_PLUGIN_V2_SIMULATOR) {
        if (api->simulator_api_size != sizeof(DigitalCurling_PluginV2SimulatorApi) || api->player_api_size != 0u ||
            !api->simulator || api->player) return false;
        // The step capability is all or nothing.
        const bool has_step = api->simulator->step != nullptr;
        if (has_step != (api->simulator->are_all_stones_stopped != nullptr) ||
            has_step != (api->simulator->get_seconds_per_frame != nullptr)) return false;
        return api->simulator->get_factory && api->simulator->save &&
               api->simulator->load && api->simulator->simulate &&
               api->simulator->set_stones && api->simulator->set_stone &&
               api->simulator->get_stones;
    }
    return false;
}

} // namespace

std::shared_ptr<PluginResource> PluginResource::CreateV2(
    const DigitalCurling_PluginApiV2* descriptor, std::optional<ModulePtr> handle
) {
    if (!validate(descriptor)) {
        throw plugin_error{DIGITALCURLING_ERR_FAILED_TO_LOAD_PLUGIN, "Invalid v2 plugin descriptor."};
    }
    auto backend = std::make_shared<V2Backend>();
    backend->api = descriptor;
    backend->module = handle;

    PluginApi api{};
    api.free_string = host_free;
    api.destroy_factory = destroy_creator;
    api.destroy_storage = destroy_creator;
    api.destroy_target = destroy_target;
    api.create_target = v2_create_target;
    api.object_creator_get_state = get_state;
    api.factory_set_state = factory_set;
    api.storage_set_state = storage_set;
    if (descriptor->plugin_kind == DIGITALCURLING_PLUGIN_V2_PLAYER) {
        backend->player = {player_factory, player_save, player_load, player_gender, player_play};
        api.player = &backend->player;
    } else {
        backend->simulator = {sim_factory, sim_save, sim_load, sim_step, sim_simulate, sim_stones,
                              sim_stone, sim_stopped, sim_get_stones, sim_seconds, sim_shot};
        api.simulator = &backend->simulator;
    }

    PluginInfo info{};
    const auto name_length = strnlen(descriptor->plugin_name, sizeof(info.plugin_name) - 1);
    std::memcpy(info.plugin_name, descriptor->plugin_name, name_length);
    info.plugin_name[name_length] = '\0';
    info.plugin_type = static_cast<DigitalCurling_PluginType>(descriptor->plugin_kind);
    info.plugin_version = 2;
    auto binding = std::make_shared<CreatorBinding>();
    auto create = [backend, binding](const char* json, bool factory) -> PluginFunctionResult<digitalcurling::plugins::HandleId> {
        if (!binding->instances) {
            return plugin_error{DIGITALCURLING_ERR_OTHER_EXCEPTION, "v2 creator binding is not initialized."};
        }
        RemoteHandle remote(*backend, factory ? backend->api->destroy_factory : backend->api->destroy_storage);
        RemoteString remote_error(*backend);
        const auto status = (factory ? backend->api->create_factory : backend->api->create_storage)(
            json, remote.out(), remote_error.out());
        if (status != DIGITALCURLING_PLUGIN_V2_OK) {
            const std::string message = remote_error.get() ? std::string(remote_error.get()) : "v2 creator failed.";
            return plugin_error{static_cast<DigitalCurling_ErrorCode>(status), message};
        }
        if (!remote) return plugin_error{DIGITALCURLING_ERR_INVALID_DATA, "v2 creator returned null."};
        if (factory) {
            auto proxy_handle = std::make_shared<V2FactoryProxy>(backend, remote.get());
            remote.release();
            if (auto id = binding->instances->Add(std::move(proxy_handle))) return id.value();
        } else {
            auto proxy_handle = std::make_shared<V2StorageProxy>(backend, remote.get());
            remote.release();
            if (auto id = binding->instances->Add(std::move(proxy_handle))) return id.value();
        }
        return plugin_error{DIGITALCURLING_ERR_MEMORY_ALLOCATION, "Failed to register v2 creator proxy."};
    };

    CreatorCallback factory_callback = [create](const char* json) { return create(json, true); };
    CreatorCallback storage_callback = [create](const char* json) { return create(json, false); };
    // validate() guarantees that the step callbacks are either all present or all absent.
    const uint32_t capabilities = descriptor->plugin_kind == DIGITALCURLING_PLUGIN_V2_SIMULATOR
        ? (descriptor->simulator->step ? DIGITALCURLING_PLUGIN_CAPABILITY_STEP : 0u) |
              (descriptor->simulator->calculate_shot ? DIGITALCURLING_PLUGIN_CAPABILITY_CALCULATE_SHOT : 0u)
        : 0u;
    std::shared_ptr<PluginResource> resource;
    if (descriptor->plugin_kind == DIGITALCURLING_PLUGIN_V2_PLAYER) {
        resource = std::make_shared<PlayerPluginResource>(std::move(info), api, std::move(handle),
            std::move(factory_callback), std::move(storage_callback), capabilities);
    } else {
        resource = std::make_shared<SimulatorPluginResource>(std::move(info), api, std::move(handle),
            std::move(factory_callback), std::move(storage_callback), capabilities);
    }
    binding->instances = &resource->GetInstanceList();
    return resource;
}

} // namespace digitalcurling::plugins::detail
