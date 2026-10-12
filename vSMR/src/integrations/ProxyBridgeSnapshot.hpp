#pragma once
#include "shared/ProxySessionChannel.hpp"
#include "shared/JsonInputLimits.hpp"
#include "shared/JsonDocument.hpp"
#include "integrations/PluginBridgeReads.hpp"
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include <array>
#include <map>
#include <unordered_set>

// Read-only ESB facade over validated fields vSMR already consumes. No API
// pointers cross processes, and the real provider API is called only on the UI.
class ProxyBridgeSnapshot final
{
public:
    struct Spec { const char* provider; const char* field; ESB_Type type; bool global; };
    inline static constexpr std::array<Spec, 23> Specs{{
        {"vsid", "sid", ESB_T_STR, false}, {"vsid", "rwy", ESB_T_STR, false}, {"vsid", "cfl", ESB_T_STR, false},
        {"vsid", "automode", ESB_T_STR, true}, {"vsid", "paris", ESB_T_STR, true}, {"vsid", "lfpg_taxi", ESB_T_STR, true},
        {"vsid", "rules", ESB_T_STR, true}, {"vsid", "areas", ESB_T_STR, true}, {"vsid", "autoconfig", ESB_T_STR, true},
        {"com.viffsys.cdm", "tobt", ESB_T_I64, false}, {"com.viffsys.cdm", "tsat", ESB_T_I64, false},
        {"com.viffsys.cdm", "ttot", ESB_T_I64, false}, {"com.viffsys.cdm", "ctot", ESB_T_I64, false},
        {"com.viffsys.cdm", "tsac", ESB_T_I64, false}, {"com.viffsys.cdm", "asrt", ESB_T_I64, false},
        {"com.viffsys.cdm", "asat", ESB_T_I64, false}, {"com.viffsys.cdm", "deice", ESB_T_STR, false},
        {"com.viffsys.cdm", "tobt_set_by", ESB_T_STR, false}, {"com.viffsys.cdm", "flow_restriction", ESB_T_STR, false},
        {"com.viffsys.cdm", "ecfmp_restriction", ESB_T_STR, false}, {"com.viffsys.cdm", "manual_ctot", ESB_T_BOOL, false},
        {"rampagent", "stand", ESB_T_STR, false}, {"rampagent", "remark", ESB_T_STR, false}
    }};
    ProxySessionChannel channel;
    bool Open(const std::wstring& identity)
    {
        if (identity == identity_ && channel.Active()) return true;
        Close(); identity_ = identity; return channel.Open(identity);
    }
    void Close()
    {
        channel.Close(); identity_.clear(); signature_.clear(); published_.clear(); revision_ = 0;
        document_.SetNull(); aircraft_.clear(); if (current_ == this) current_ = nullptr;
    }
    ~ProxyBridgeSnapshot() { Close(); }
    void Withdraw()
    {
        channel.Close(); channel.Open(identity_); signature_.clear(); published_.clear();
    }
    bool Publish(const ESB_Api_v1& api, const std::unordered_set<std::string>& callsigns)
    {
        if (!channel.Active() || callsigns.size() > 4096) return false;
        std::vector<std::string> sorted(callsigns.begin(), callsigns.end());
        std::sort(sorted.begin(), sorted.end());
        std::string signature;
        for (const auto& call : sorted) signature += call + ";";
        std::map<std::string, std::array<std::uint64_t, 3>> providers;
        for (const char* id : {"vsid", "com.viffsys.cdm", "rampagent"}) {
            std::uint32_t major = 0, minor = 0;
            if (api.provider_version(id, &major, &minor) == ESB_OK && major == 1) {
                providers[id] = {major, minor, api.provider_revision(id)};
                signature += std::string(id) + ":" + std::to_string(minor) + ":" + std::to_string(providers[id][2]) + ";";
            }
        }
        if (signature == signature_ && !published_.empty()) return channel.Publish(published_);
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        bool complete = true;
        writer.StartObject(); writer.Key("callsigns"); writer.StartArray();
        for (const auto& call : sorted) writer.String(call.c_str());
        writer.EndArray(); writer.Key("providers"); writer.StartObject();
        for (const auto& provider : providers) {
            writer.Key(provider.first.c_str()); writer.StartObject();
            writer.Key("major"); writer.Uint(1); writer.Key("minor"); writer.Uint(static_cast<unsigned>(provider.second[1]));
            writer.Key("fields"); writer.StartObject();
            for (const auto& spec : Specs) {
                if (provider.first != spec.provider) continue;
                ESB_FieldId field = ESB_FIELD_NONE;
                const auto qualified = provider.first + "/" + spec.field;
                if (api.resolve(qualified.c_str(), spec.type, &field) != ESB_OK || !field) continue;
                writer.Key(spec.field); writer.StartObject();
                const auto read = [&](const std::string& call) {
                    ESB_Aircraft aircraft = ESB_AIRCRAFT_NONE;
                    if (!spec.global && VsmrPluginBridge::ResolveAircraft(api, call, aircraft) != VsmrPluginBridge::ReadStatus::Value) return;
                    VsmrPluginBridge::ReadStatus status;
                    std::string text; std::int64_t integer = 0; bool flag = false;
                    if (spec.type == ESB_T_STR) status = spec.global
                        ? VsmrPluginBridge::ReadGlobalString(api, field, 256, text)
                        : VsmrPluginBridge::ReadAircraftString(api, call, aircraft, field, 256, text);
                    else if (spec.type == ESB_T_I64) status = VsmrPluginBridge::ReadAircraftInteger(api, call, aircraft, field, integer);
                    else status = VsmrPluginBridge::ReadAircraftBoolean(api, call, aircraft, field, flag);
                    if (status == VsmrPluginBridge::ReadStatus::Failed || status == VsmrPluginBridge::ReadStatus::ProviderLost) complete = false;
                    if (status != VsmrPluginBridge::ReadStatus::Value) return;
                    writer.Key(call.c_str());
                    if (spec.type == ESB_T_STR) writer.String(text.data(), static_cast<rapidjson::SizeType>(text.size()));
                    else if (spec.type == ESB_T_I64) writer.Int64(integer);
                    else writer.Bool(flag);
                };
                if (spec.global) read(""); else for (const auto& call : sorted) read(call);
                writer.EndObject();
            }
            writer.EndObject(); writer.EndObject();
        }
        writer.EndObject(); writer.EndObject();
        published_.assign(buffer.GetString(), buffer.GetSize());
        // Don't retain failed reads behind a provider-revision gate.
        signature_ = complete ? signature : "";
        return channel.Publish(published_);
    }
    const ESB_Api_v1* RemoteApi()
    {
        if (channel.IsPublisher()) return nullptr;
        std::string payload; std::uint64_t revision = 0;
        if (!channel.Read(payload, revision)) { document_.SetNull(); revision_ = 0; return nullptr; }
        if (revision != revision_) {
            VsmrJsonInputLimits::Limits limits;
            limits.maximumDepth = 8; limits.maximumValues = 150000; limits.maximumContainerEntries = 4096;
            limits.maximumStringBytes = VsmrPluginBridge::MaximumStringBytes * 6U;
            rapidjson::Document next;
            VsmrJson::ParseDocument(next, payload, limits);
            if (!Valid(next)) { document_.SetNull(); revision_ = 0; return nullptr; }
            document_.Swap(next); revision_ = revision; ++generation_;
            aircraft_.clear();
            for (const auto& call : document_["callsigns"].GetArray()) aircraft_.emplace_back(call.GetString(), call.GetStringLength());
        }
        current_ = this;
        static const ESB_Api_v1 api = [] {
            ESB_Api_v1 result{}; result.struct_size = sizeof(result); result.abi_version = ESB_ABI_VERSION;
            result.resolve = Resolve; result.provider_version = Version; result.provider_revision = Revision;
            result.aircraft = Aircraft; result.get_global = Global; result.get_ac = Get;
            return result;
        }();
        return &api;
    }
private:
    static bool Valid(const rapidjson::Document& doc)
    {
        if (!doc.IsObject() || !doc.HasMember("providers") || !doc["providers"].IsObject() ||
            !doc.HasMember("callsigns") || !doc["callsigns"].IsArray() || doc["callsigns"].Size() > 4096) return false;
        for (const auto& call : doc["callsigns"].GetArray()) if (!call.IsString() || call.GetStringLength() > 64) return false;
        for (auto it = doc["providers"].MemberBegin(); it != doc["providers"].MemberEnd(); ++it) {
            const auto& p = it->value;
            if (!p.IsObject() || !p.HasMember("major") || !p["major"].IsUint() || p["major"].GetUint() != 1 ||
                !p.HasMember("minor") || !p["minor"].IsUint() || !p.HasMember("fields") || !p["fields"].IsObject()) return false;
            for (auto field = p["fields"].MemberBegin(); field != p["fields"].MemberEnd(); ++field) {
                const auto spec = std::find_if(Specs.begin(), Specs.end(), [&](const Spec& s) {
                    return it->name == s.provider && field->name == s.field;
                });
                if (spec == Specs.end() || !field->value.IsObject() || field->value.MemberCount() > 4096) return false;
                for (auto value = field->value.MemberBegin(); value != field->value.MemberEnd(); ++value) {
                    if (value->name.GetStringLength() > 64 || (spec->global && value->name.GetStringLength() != 0)) return false;
                    if ((spec->type == ESB_T_STR && (!value->value.IsString() || value->value.GetStringLength() > VsmrPluginBridge::MaximumStringBytes)) ||
                        (spec->type == ESB_T_I64 && !value->value.IsInt64()) || (spec->type == ESB_T_BOOL && !value->value.IsBool())) return false;
                }
            }
        }
        return true;
    }
    static const rapidjson::Value* Provider(const char* id)
    {
        if (!current_ || !id || !current_->document_.IsObject()) return nullptr;
        const auto& providers = current_->document_["providers"];
        const auto it = providers.FindMember(id); return it == providers.MemberEnd() ? nullptr : &it->value;
    }
    static ESB_Status __cdecl Version(const char* id, std::uint32_t* major, std::uint32_t* minor)
    {
        if (!major || !minor) return ESB_E_INVALID_ARG;
        const auto p = Provider(id); if (!p) return ESB_E_NO_PROVIDER;
        *major = 1; *minor = (*p)["minor"].GetUint(); return ESB_OK;
    }
    static std::uint64_t __cdecl Revision(const char* id) { return Provider(id) ? current_->generation_ : 0; }
    static ESB_Status __cdecl Resolve(const char* name, ESB_Type type, ESB_FieldId* field)
    {
        if (!name || !field) return ESB_E_INVALID_ARG;
        for (std::size_t i = 0; i < Specs.size(); ++i) {
            const auto& s = Specs[i]; if (std::string(s.provider) + "/" + s.field != name) continue;
            const auto p = Provider(s.provider); if (!p) return ESB_E_NO_PROVIDER;
            if (!(*p)["fields"].HasMember(s.field)) return ESB_E_NO_FIELD;
            if (type != s.type) return ESB_E_TYPE_MISMATCH;
            *field = static_cast<ESB_FieldId>(i + 1); return ESB_OK;
        }
        return ESB_E_NO_FIELD;
    }
    static ESB_Status __cdecl Aircraft(const char* call, ESB_Aircraft* out)
    {
        if (!current_ || !call || !out) return ESB_E_INVALID_ARG;
        const auto it = std::find(current_->aircraft_.begin(), current_->aircraft_.end(), call);
        if (it == current_->aircraft_.end()) return ESB_E_UNKNOWN_AIRCRAFT;
        *out = (static_cast<std::uint64_t>(current_->generation_) << 32) |
            static_cast<std::uint32_t>(it - current_->aircraft_.begin() + 1); return ESB_OK;
    }
    static ESB_Status Value(ESB_FieldId field, const char* call, bool global, ESB_Value* out, void* buffer, std::uint32_t* bytes)
    {
        if (!field || field > Specs.size() || !out) return ESB_E_INVALID_ARG;
        const auto& s = Specs[field - 1]; if (s.global != global) return ESB_E_INVALID_ARG;
        const auto p = Provider(s.provider); if (!p) return ESB_E_NO_PROVIDER;
        const auto& fields = (*p)["fields"]; const auto f = fields.FindMember(s.field);
        if (f == fields.MemberEnd()) return ESB_E_NO_FIELD;
        const auto v = f->value.FindMember(call); if (v == f->value.MemberEnd()) return ESB_E_UNSET;
        *out = {}; out->type = s.type;
        if (s.type == ESB_T_STR) {
            out->bytes = v->value.GetStringLength();
            if (!bytes) return ESB_E_INVALID_ARG;
            const auto capacity = *bytes; *bytes = out->bytes;
            if (capacity < out->bytes || (!buffer && out->bytes)) return ESB_E_BUFFER_TOO_SMALL;
            if (out->bytes) std::memcpy(buffer, v->value.GetString(), out->bytes);
            out->v.ptr = buffer;
        } else if (s.type == ESB_T_I64) { out->bytes = sizeof(std::int64_t); out->v.i64 = v->value.GetInt64(); }
        else { out->bytes = sizeof(std::int32_t); out->v.b = v->value.GetBool() ? 1 : 0; }
        return ESB_OK;
    }
    static ESB_Status __cdecl Global(ESB_FieldId f, ESB_Value* out, void* buf, std::uint32_t* bytes) { return Value(f, "", true, out, buf, bytes); }
    static ESB_Status __cdecl Get(ESB_Aircraft ac, ESB_FieldId f, ESB_Value* out, void* buf, std::uint32_t* bytes)
    {
        if (!current_ || static_cast<std::uint32_t>(ac >> 32) != current_->generation_) return ESB_E_STALE_AIRCRAFT;
        const auto index = static_cast<std::uint32_t>(ac);
        if (!index || index > current_->aircraft_.size()) return ESB_E_UNKNOWN_AIRCRAFT;
        return Value(f, current_->aircraft_[index - 1].c_str(), false, out, buf, bytes);
    }
    inline static ProxyBridgeSnapshot* current_ = nullptr;
    std::wstring identity_;
    rapidjson::Document document_;
    std::vector<std::string> aircraft_;
    std::uint64_t revision_ = 0;
    std::uint32_t generation_ = 0;
    std::string signature_, published_;
};
