// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#pragma once

#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <nlohmann/json.hpp>
#include "digitalcurling/plugins/i_plugin_object.hpp"
#include "digitalcurling/plugins/data_object.h"
#include "digitalcurling/plugins/plugin_api.hpp"
#include "digitalcurling/plugins/plugin_api_v2.h"
#include "digitalcurling/plugins/plugin_type.hpp"


//* NOTE: DIGITALCURLING_PLUGIN_NAME must be defined as UTF-8 string literal before including this file.
#ifndef DIGITALCURLING_PLUGIN_NAME
    #error "DIGITALCURLING_PLUGIN_NAME must be defined before including plugin_exports.hpp"
#endif

namespace digitalcurling::plugins::detail {

static_assert(sizeof(DigitalCurling_PluginV2Shot) == sizeof(DigitalCurling_Shot));
static_assert(sizeof(DigitalCurling_PluginV2StoneState) == sizeof(DigitalCurling_StoneState));
static_assert(sizeof(DigitalCurling_PluginV2StoneCoordinate) == sizeof(DigitalCurling_StoneCoordinate));
static_assert(alignof(DigitalCurling_PluginV2StoneCoordinate) == alignof(DigitalCurling_StoneCoordinate));

// --- Error/Exception handling functions ---
inline char* DuplicateString(const std::string& str)
{
    char* copy = static_cast<char*>(malloc(str.length() + 1));
    if (copy) memcpy(copy, str.c_str(), str.length() + 1);
    return copy;
}

inline DigitalCurling_ErrorCode ReturnError(const DigitalCurling_ErrorCode type, const std::string& message, char** out_error)
{
    if (!out_error) return DIGITALCURLING_ERR_BUFFER_NULLPTR;

    *out_error = DuplicateString(message);
    return *out_error ? type : DIGITALCURLING_ERR_MEMORY_ALLOCATION;
}

inline DigitalCurling_ErrorCode ReturnException(const std::exception& e, const std::string& function_name, char** out_error)
{
    DigitalCurling_ErrorCode type;
    if (dynamic_cast<const nlohmann::json::parse_error*>(&e)) {
        type = DIGITALCURLING_ERR_JSON_PARSE;
    } else if (dynamic_cast<const nlohmann::json::exception*>(&e)) {
        type = DIGITALCURLING_ERR_JSON_DATA;
    } else if (dynamic_cast<const std::bad_alloc*>(&e)) {
        type = DIGITALCURLING_ERR_MEMORY_ALLOCATION;
    } else if (dynamic_cast<const std::invalid_argument*>(&e)) {
        type = DIGITALCURLING_ERR_INVALID_ARGUMENT;
    } else if (dynamic_cast<const std::exception*>(&e)) {
        type = DIGITALCURLING_ERR_OTHER_EXCEPTION;
    } else {
        type = DIGITALCURLING_ERR_UNKNOWN;
    }
    return ReturnError(type, function_name + ": " + e.what(), out_error);
}


inline DigitalCurling_ErrorCode ReturnString(const std::string& value, const std::string& function_name, char** out_str, char** out_error) {
    if (!out_str) {
        return ReturnError(DIGITALCURLING_ERR_BUFFER_NULLPTR, function_name + ": result buffer is nullptr.", out_error);
    }
    *out_str = DuplicateString(value);
    return *out_str ? DIGITALCURLING_OK :
        ReturnError(DIGITALCURLING_ERR_MEMORY_ALLOCATION, function_name + ": Memory allocation failed.", out_error);
}


// --- C++ Implementation to C-ABI Wrapper Functions ---
template <typename T>
void DestroyObjectImpl(void* handle)
{
    delete static_cast<T*>(handle);
}

template <typename T, typename THandlePtr>
DigitalCurling_ErrorCode CreateCreatorImpl(const char* json, THandlePtr* out_handle, char** out_error);
inline DigitalCurling_ErrorCode CreateTargetImpl(CreatorHandle* creator, TargetHandle** out_target_handle, char** out_error);
inline DigitalCurling_ErrorCode CreatorGetStateImpl(CreatorHandle* creator, char** out_json, char** out_error);
template <typename T, typename THandle>
DigitalCurling_ErrorCode CreatorSetStateImpl(THandle creator, const char* json, char** out_error);

// The ABI boundary owns this box, never a C++ base-class pointer.  The concrete
// deleter is captured while still inside the plugin, so the host can only pass
// the opaque box back to the matching v2 callback.
enum class V2BoxKind : uint32_t { kFactory = 1, kStorage = 2, kPlayer = 3, kSimulator = 4 };
inline constexpr uint32_t kV2BoxMagic = 0x32564344u; // "DCV2"

struct V2OwnedBox final {
    uint32_t magic = kV2BoxMagic;
    uint32_t kind = 0;
    void* object = nullptr;
    void (*destroy)(void*) = nullptr;
    CreatorHandle* creator = nullptr;
    FactoryHandle* factory = nullptr;
    StorageHandle* storage = nullptr;
    TargetHandle* target = nullptr;
    PlayerHandle* player = nullptr;
    SimulatorHandle* simulator = nullptr;
    ~V2OwnedBox() noexcept { magic = 0; try { if (object && destroy) destroy(object); } catch (...) {} }
};

inline DigitalCurling_PluginV2Status V2ReturnError(DigitalCurling_ErrorCode code, const char* text, char** error) noexcept {
    try { return static_cast<DigitalCurling_PluginV2Status>(ReturnError(code, text, error)); }
    catch (...) { return static_cast<DigitalCurling_PluginV2Status>(DIGITALCURLING_ERR_UNKNOWN); }
}
inline DigitalCurling_PluginV2Status V2ReturnException(const std::exception& exception, const char* operation, char** error) noexcept {
    try { return static_cast<DigitalCurling_PluginV2Status>(ReturnException(exception, operation, error)); }
    catch (...) { return static_cast<DigitalCurling_PluginV2Status>(DIGITALCURLING_ERR_MEMORY_ALLOCATION); }
}
inline void DIGITALCURLING_PLUGIN_V2_CALL V2FreeStringImpl(char* text) noexcept { free(text); }
// Returns nullptr unless `box` carries the box magic.  The magic is only a guard against a
// handle of the wrong origin or an already destroyed box, not against arbitrary pointers.
inline V2OwnedBox* V2Box(void* box) {
    auto* value = static_cast<V2OwnedBox*>(box);
    return value && value->magic == kV2BoxMagic ? value : nullptr;
}
// Same as above, and the box must also be of the given kind.
inline V2OwnedBox* V2Box(void* box, V2BoxKind kind) {
    auto* value = V2Box(box);
    return value && value->kind == static_cast<uint32_t>(kind) ? value : nullptr;
}
// A creator box is a factory or a storage box.
inline V2OwnedBox* V2CreatorBox(void* box) {
    auto* value = V2Box(box);
    return value && (value->kind == static_cast<uint32_t>(V2BoxKind::kFactory) ||
                     value->kind == static_cast<uint32_t>(V2BoxKind::kStorage)) ? value : nullptr;
}
template <typename T> inline T* V2Unbox(void* box, V2BoxKind kind) {
    auto* value = V2Box(box, kind);
    return value ? static_cast<T*>(value->object) : nullptr;
}
// A box without the magic (wrong origin or already destroyed) is ignored rather than deleted.
inline void DIGITALCURLING_PLUGIN_V2_CALL V2DestroyBox(void* box) noexcept { try { delete V2Box(box); } catch (...) {} }
// Error for a handle that is null (invalid argument) or not a box of the expected kind (failed cast).
inline DigitalCurling_PluginV2Status V2BadHandle(void* handle, const char* operation, char** error) noexcept {
    try {
        return V2ReturnError(
            handle ? DIGITALCURLING_ERR_FAILED_TO_CAST : DIGITALCURLING_ERR_INVALID_ARGUMENT,
            (std::string(operation) + (handle ? ": handle is not of the expected kind." : ": handle is nullptr.")).c_str(),
            error);
    } catch (...) { return static_cast<DigitalCurling_PluginV2Status>(DIGITALCURLING_ERR_MEMORY_ALLOCATION); }
}

template <typename T>
DigitalCurling_PluginV2Status DIGITALCURLING_PLUGIN_V2_CALL V2CreateCreator(const char* json, void** out, char** error) noexcept {
    if (!out) return V2ReturnError(DIGITALCURLING_ERR_BUFFER_NULLPTR, "Create: out handle is nullptr.", error);
    *out = nullptr;
    if (error) *error = nullptr;
    try {
        T* handle = nullptr;
        const auto code = CreateCreatorImpl<T, T*>(json, &handle, error);
        if (code != DIGITALCURLING_OK) return static_cast<DigitalCurling_PluginV2Status>(code);
        std::unique_ptr<T, void(*)(T*)> owned(handle, [](T* value) { DestroyObjectImpl<T>(value); });
        auto box = std::make_unique<V2OwnedBox>();
        box->object = owned.release();
        box->destroy = &DestroyObjectImpl<T>;
        auto* creator_interface = dynamic_cast<IPluginObjectCreator*>(static_cast<T*>(box->object));
        box->creator = creator_interface ? static_cast<CreatorHandle*>(creator_interface) : nullptr;
        box->factory = dynamic_cast<FactoryHandle*>(static_cast<T*>(box->object));
        box->storage = dynamic_cast<StorageHandle*>(static_cast<T*>(box->object));
        if (!box->creator || (!box->factory && !box->storage))
            return V2ReturnError(DIGITALCURLING_ERR_FAILED_TO_CAST, "Create: object is not a factory or a storage.", error);
        box->kind = static_cast<uint32_t>(box->factory ? V2BoxKind::kFactory : V2BoxKind::kStorage);
        *out = box.release();
        return DIGITALCURLING_OK;
    } catch (const std::exception& e) { return V2ReturnException(e, "Create", error); }
      catch (...) { return V2ReturnError(DIGITALCURLING_ERR_UNKNOWN, "Create: unknown exception.", error); }
}

template <typename Target>
DigitalCurling_PluginV2Status DIGITALCURLING_PLUGIN_V2_CALL V2CreateTarget(void* creator, void** out, char** error) noexcept {
    if (!creator) return V2ReturnError(DIGITALCURLING_ERR_INVALID_ARGUMENT, "CreateTarget: creator is nullptr.", error);
    if (!out) return V2ReturnError(DIGITALCURLING_ERR_BUFFER_NULLPTR, "CreateTarget: out handle is nullptr.", error);
    *out = nullptr;
    if (error) *error = nullptr;
    try {
        TargetHandle* target = nullptr;
        auto* creator_box = V2CreatorBox(creator);
        if (!creator_box) return V2BadHandle(creator, "CreateTarget", error);
        const auto code = CreateTargetImpl(creator_box->creator, &target, error);
        if (code != DIGITALCURLING_OK) return static_cast<DigitalCurling_PluginV2Status>(code);
        auto* complete = dynamic_cast<Target*>(target);
        if (!complete) { delete target; return V2ReturnError(DIGITALCURLING_ERR_FAILED_TO_CAST, "CreateTarget: target type is invalid.", error); }
        auto box = std::make_unique<V2OwnedBox>();
        box->object = complete;
        box->destroy = &DestroyObjectImpl<Target>;
        box->target = target;
        box->player = dynamic_cast<PlayerHandle*>(target);
        box->simulator = dynamic_cast<SimulatorHandle*>(target);
        if (!box->player && !box->simulator)
            return V2ReturnError(DIGITALCURLING_ERR_FAILED_TO_CAST, "CreateTarget: target is not a player or a simulator.", error);
        box->kind = static_cast<uint32_t>(box->player ? V2BoxKind::kPlayer : V2BoxKind::kSimulator);
        *out = box.release();
        return DIGITALCURLING_OK;
    } catch (const std::exception& e) { return V2ReturnException(e, "CreateTarget", error); }
      catch (...) { return V2ReturnError(DIGITALCURLING_ERR_UNKNOWN, "CreateTarget: unknown exception.", error); }
}

inline DigitalCurling_PluginV2Status DIGITALCURLING_PLUGIN_V2_CALL V2CreatorGetState(void* creator, char** out, char** error) noexcept {
    if (out) *out = nullptr;
    if (error) *error = nullptr;
    try {
        auto* box = V2CreatorBox(creator);
        if (!box) return V2BadHandle(creator, "GetState", error);
        return static_cast<DigitalCurling_PluginV2Status>(CreatorGetStateImpl(box->creator, out, error));
    }
    catch (...) { return V2ReturnError(DIGITALCURLING_ERR_UNKNOWN, "GetState: unknown exception.", error); }
}
template <typename T>
DigitalCurling_PluginV2Status DIGITALCURLING_PLUGIN_V2_CALL V2CreatorSetState(void* creator, const char* json, char** error) noexcept {
    if (error) *error = nullptr;
    constexpr auto kind = std::is_base_of_v<FactoryHandle, T> ? V2BoxKind::kFactory : V2BoxKind::kStorage;
    try {
        auto* value = V2Unbox<T>(creator, kind);
        if (!value) return V2BadHandle(creator, "SetState", error);
        return static_cast<DigitalCurling_PluginV2Status>(CreatorSetStateImpl<T, T*>(value, json, error));
    }
    catch (...) { return V2ReturnError(DIGITALCURLING_ERR_UNKNOWN, "SetState: unknown exception.", error); }
}

template <typename T, typename THandlePtr>
DigitalCurling_ErrorCode CreateCreatorImpl(const char* json, THandlePtr* out_handle, char** out_error)
{
    if (!out_handle)
        return ReturnError(DIGITALCURLING_ERR_BUFFER_NULLPTR, "CreateCreator: out_handle is nullptr.", out_error);

    try {
        auto creator = std::make_unique<T>();
        if (json && *json != '\0') {
            nlohmann::json j = nlohmann::json::parse(json);
            from_json(j, *creator);
        }

        static_assert(std::is_convertible<T*, THandlePtr>::value, "CreateCreator: T* is not convertible to THandlePtr.");
        *out_handle = static_cast<THandlePtr>(creator.release());
        return DIGITALCURLING_OK;
    } catch (const std::exception& e) {
        return ReturnException(e, "CreateCreator", out_error);
    }
}
//template <typename T>
inline DigitalCurling_ErrorCode CreateTargetImpl(CreatorHandle* creator, TargetHandle** out_target_handle, char** out_error)
{
    if (!creator)
        return ReturnError(DIGITALCURLING_ERR_INVALID_ARGUMENT, "CreateTarget: creator handle is nullptr.", out_error);
    if (!out_target_handle)
        return ReturnError(DIGITALCURLING_ERR_BUFFER_NULLPTR, "CreateTarget: out_target_handle is nullptr.", out_error);

    try {
        auto* creator_ptr = dynamic_cast<typename digitalcurling::plugins::IPluginObjectCreator*>(creator);
        if (!creator_ptr)
            return ReturnError(DIGITALCURLING_ERR_FAILED_TO_CAST, "CreateTarget: creator handle is not of type IPluginObjectCreator.", out_error);

        *out_target_handle = creator_ptr->Create();
        return *out_target_handle ? DIGITALCURLING_OK : DIGITALCURLING_ERR_MEMORY_ALLOCATION;
    } catch (const std::exception& e) {
        return ReturnException(e, "CreateTarget", out_error);
    }
}

inline DigitalCurling_ErrorCode CreatorGetStateImpl(CreatorHandle* creator, char** out_json, char** out_error)
{
    if (!creator)
        return ReturnError(DIGITALCURLING_ERR_INVALID_ARGUMENT, "CreatorGetState: creator handle is nullptr.", out_error);

    try {
        auto* creator_ptr = dynamic_cast<typename digitalcurling::plugins::IPluginObjectCreator*>(creator);
        if (!creator_ptr)
            return ReturnError(DIGITALCURLING_ERR_FAILED_TO_CAST, "CreatorGetState: creator handle is not of type IPluginObjectCreator.", out_error);
        return ReturnString(creator_ptr->ToJson().dump(), "CreatorGetState", out_json, out_error);
    } catch (const std::exception& e) {
        return ReturnException(e, "CreatorGetState", out_error);
    }
}
template <typename T, typename THandle>
DigitalCurling_ErrorCode CreatorSetStateImpl(THandle creator, const char* json, char** out_error)
{
    if (!creator)
        return ReturnError(DIGITALCURLING_ERR_INVALID_ARGUMENT, "CreatorSetState: creator handle is nullptr.", out_error);
    if (!json)
        return ReturnError(DIGITALCURLING_ERR_INVALID_ARGUMENT, "CreatorSetState: json is nullptr.", out_error);

    try {
        auto* creator_ptr = dynamic_cast<T*>(creator);
        if (!creator_ptr)
            return ReturnError(DIGITALCURLING_ERR_FAILED_TO_CAST, "CreatorSetState: creator handle has an unexpected type.", out_error);
        auto j = creator_ptr->ToJson();
        j.merge_patch(nlohmann::json::parse(json));
        from_json(j, *creator_ptr);
        return DIGITALCURLING_OK;
    } catch (const std::exception& e) {
        return ReturnException(e, "CreatorSetState", out_error);
    }
}

template <PluginType Type, typename Factory>
DigitalCurling_ErrorCode GetFactoryImpl(typename digitalcurling::plugins::PluginTypeTraits<Type>::HandleType* handle, FactoryHandle** out_factory_handle, char** out_error)
{
    if (!handle)
        return ReturnError(DIGITALCURLING_ERR_INVALID_ARGUMENT, "GetFactory: handle is nullptr.", out_error);
    if (!out_factory_handle)
        return ReturnError(DIGITALCURLING_ERR_BUFFER_NULLPTR, "GetFactory: out_factory_handle is nullptr.", out_error);

    try {
        auto* target_ptr = dynamic_cast<typename digitalcurling::plugins::PluginTypeTraits<Type>::TargetType*>(handle);
        if (!target_ptr)
            return ReturnError(DIGITALCURLING_ERR_FAILED_TO_CAST, "GetFactory: handle has an unexpected type.", out_error);
        // GetFactory() returns a copy; check its dynamic type and hand the ownership over.
        auto factory = target_ptr->GetFactory();
        if (!factory)
            return ReturnError(DIGITALCURLING_ERR_UNKNOWN, "GetFactory: the object returned no factory.", out_error);
        auto* complete = dynamic_cast<Factory*>(factory.get());
        if (!complete)
            return ReturnError(DIGITALCURLING_ERR_FAILED_TO_CAST, "GetFactory: factory has an unexpected type.", out_error);
        factory.release();
        *out_factory_handle = static_cast<digitalcurling::plugins::FactoryHandle*>(complete);
        return DIGITALCURLING_OK;
    } catch (const std::exception& e) {
        return ReturnException(e, "GetFactory", out_error);
    }
}
template <PluginType Type, typename Storage>
DigitalCurling_ErrorCode TargetSaveImpl(typename digitalcurling::plugins::PluginTypeTraits<Type>::HandleType* handle, StorageHandle* storage_handle, char** out_error)
{
    if (!handle)
        return ReturnError(DIGITALCURLING_ERR_INVALID_ARGUMENT, "Save: handle is nullptr.", out_error);
    if (!storage_handle)
        return ReturnError(DIGITALCURLING_ERR_INVALID_ARGUMENT, "Save: storage handle is nullptr.", out_error);

    try {
        auto* target_ptr = dynamic_cast<typename digitalcurling::plugins::PluginTypeTraits<Type>::TargetType*>(handle);
        auto* storage_ptr = dynamic_cast<Storage*>(storage_handle);
        if (!target_ptr || !storage_ptr)
            return ReturnError(DIGITALCURLING_ERR_FAILED_TO_CAST, "Save: handle or storage has an unexpected type.", out_error);
        target_ptr->Save(*storage_ptr);
        return DIGITALCURLING_OK;
    } catch (const std::exception& e) {
        return ReturnException(e, "Save", out_error);
    }
}
template <PluginType Type, typename Storage>
DigitalCurling_ErrorCode TargetLoadImpl(typename digitalcurling::plugins::PluginTypeTraits<Type>::HandleType* handle, StorageHandle* storage_handle, char** out_error)
{
    if (!handle)
        return ReturnError(DIGITALCURLING_ERR_INVALID_ARGUMENT, "Load: handle is nullptr.", out_error);
    if (!storage_handle)
        return ReturnError(DIGITALCURLING_ERR_INVALID_ARGUMENT, "Load: storage handle is nullptr.", out_error);

    try {
        auto* target_ptr = dynamic_cast<typename digitalcurling::plugins::PluginTypeTraits<Type>::TargetType*>(handle);
        const auto* storage_ptr = dynamic_cast<const Storage*>(storage_handle);
        if (!target_ptr || !storage_ptr)
            return ReturnError(DIGITALCURLING_ERR_FAILED_TO_CAST, "Load: handle or storage has an unexpected type.", out_error);
        target_ptr->Load(*storage_ptr);
        return DIGITALCURLING_OK;
    } catch (const std::exception& e) {
        return ReturnException(e, "Load", out_error);
    }
}

} // namespace


namespace dcpd = digitalcurling::plugins::detail;

// --- Internal Macro for Exporting the Plugin API Function ---
#ifdef DIGITALCURLING_BUNDLE_PLUGINS
    #define DIGITALCURLING_EXPORT_PLUGIN_FUNCTIONS(ApiV2Instance) \
        namespace { \
            struct StaticPluginRegistrarV2 { StaticPluginRegistrarV2() { digitalcurling::plugins::RegisterStaticPluginV2(&(ApiV2Instance)); } }; \
            StaticPluginRegistrarV2 g_static_registrar; \
        }
#else
    #define DIGITALCURLING_EXPORT_PLUGIN_FUNCTIONS(ApiV2Instance) \
        DIGITALCURLING_PLUGIN_V2_EXPORT const DigitalCurling_PluginApiV2* DIGITALCURLING_PLUGIN_V2_CALL dc_get_plugin_api_v2() noexcept \
        { return &(ApiV2Instance); }
#endif // DIGITALCURLING_BUNDLE_PLUGINS

#define DIGITALCURLING_EXPORT_PLUGIN_INNER(PluginType, FactoryClass, StorageClass, TargetClass, PlayerApiV2Instance, PlayerApiV2Size, SimulatorApiV2Instance, SimulatorApiV2Size) \
    static const DigitalCurling_PluginApiV2 g_plugin_api_v2 = { \
        DIGITALCURLING_PLUGIN_ABI_V2, sizeof(DigitalCurling_PluginApiV2), static_cast<uint32_t>(PluginType), \
        PlayerApiV2Size, SimulatorApiV2Size, DIGITALCURLING_PLUGIN_NAME, \
        &dcpd::V2FreeStringImpl, &dcpd::V2CreateCreator<FactoryClass>, &dcpd::V2CreateCreator<StorageClass>, \
        &dcpd::V2DestroyBox, &dcpd::V2DestroyBox, &dcpd::V2CreateTarget<TargetClass>, &dcpd::V2DestroyBox, \
        &dcpd::V2CreatorGetState, &dcpd::V2CreatorSetState<FactoryClass>, &dcpd::V2CreatorSetState<StorageClass>, \
        PlayerApiV2Instance, SimulatorApiV2Instance \
    }; \
    DIGITALCURLING_EXPORT_PLUGIN_FUNCTIONS(g_plugin_api_v2)
