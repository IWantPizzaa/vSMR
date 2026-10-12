#pragma once
#include "shared/ProxySessionChannel.hpp"
#include "shared/JsonInputLimits.hpp"
#include "shared/JsonDocument.hpp"
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include <map>

// Display-only CPDLC state. No password, message body, connection or send RPC.
class ProxyDatalinkView final
{
public:
    bool connected = false, connecting = false, polling = false, remote = false;
    std::string logon;
    std::map<std::string, unsigned> flags;
    bool Open(const std::wstring& identity)
    {
        if (identity == identity_ && channel_.Active()) return true;
        channel_.Close(); Clear(); identity_ = identity;
        return channel_.Open(identity, L"DatalinkView");
    }
    void Publish(bool online, bool starting, bool busy, const std::string& callsign, const std::map<std::string, unsigned>& aircraft)
    {
        if (aircraft.size() > 4096 || callsign.size() > 64) return;
        rapidjson::StringBuffer buffer; rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        writer.StartObject(); writer.Key("connected"); writer.Bool(online);
        writer.Key("connecting"); writer.Bool(starting); writer.Key("polling"); writer.Bool(busy);
        writer.Key("logon"); writer.String(callsign.c_str()); writer.Key("flags"); writer.StartObject();
        for (const auto& item : aircraft) { writer.Key(item.first.c_str()); writer.Uint(item.second & 31U); }
        writer.EndObject(); writer.EndObject();
        channel_.Publish(std::string(buffer.GetString(), buffer.GetSize())); Clear();
    }
    void Poll()
    {
        if (channel_.IsPublisher()) { channel_.Close(); channel_.Open(identity_, L"DatalinkView"); }
        std::string payload; std::uint64_t revision = 0;
        if (!channel_.Read(payload, revision)) { Clear(); return; }
        if (remote && revision == revision_) return;
        VsmrJsonInputLimits::Limits limits; limits.maximumDepth = 3; limits.maximumValues = 5000;
        limits.maximumContainerEntries = 4096; limits.maximumStringBytes = 64;
        rapidjson::Document doc; VsmrJson::ParseDocument(doc, payload, limits);
        if (!doc.IsObject() || !doc.HasMember("connected") || !doc["connected"].IsBool() ||
            !doc.HasMember("connecting") || !doc["connecting"].IsBool() ||
            !doc.HasMember("polling") || !doc["polling"].IsBool() ||
            !doc.HasMember("logon") || !doc["logon"].IsString() ||
            !doc.HasMember("flags") || !doc["flags"].IsObject()) { Clear(); return; }
        std::map<std::string, unsigned> next;
        for (auto it = doc["flags"].MemberBegin(); it != doc["flags"].MemberEnd(); ++it) {
            if (!it->value.IsUint() || it->value.GetUint() > 31 || it->name.GetStringLength() > 64) { Clear(); return; }
            next.emplace(std::string(it->name.GetString(), it->name.GetStringLength()), it->value.GetUint());
        }
        connected = doc["connected"].GetBool(); connecting = doc["connecting"].GetBool(); polling = doc["polling"].GetBool();
        logon.assign(doc["logon"].GetString(), doc["logon"].GetStringLength()); flags = std::move(next); remote = true; revision_ = revision;
    }
    unsigned Flags(const std::string& call) const { const auto it = flags.find(call); return it != flags.end() ? it->second : 0; }
private:
    void Clear() { connected = connecting = polling = remote = false; logon.clear(); flags.clear(); revision_ = 0; }
    std::uint64_t revision_ = 0;
    std::wstring identity_;
    ProxySessionChannel channel_;
};
