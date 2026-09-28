#include "platform/windows/PrecompiledHeader.hpp"
#include "aviso/AvisoSharedConfig.hpp"
#include "config/LayeredConfig.hpp"
#include "shared/JsonDocument.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>

namespace VsmrAvisoSharedConfig
{
	namespace
	{
		constexpr std::uintmax_t MaximumBytes = 16U * 1024U * 1024U;
		constexpr size_t MaximumCachedRoots = 16;
		using Clock = std::chrono::steady_clock;

		struct Stamp
		{
			bool exists = false;
			std::uintmax_t size = 0;
			std::filesystem::file_time_type writeTime{};
			bool operator==(const Stamp& other) const
			{
				return exists == other.exists && size == other.size && writeTime == other.writeTime;
			}
		};

		struct Entry
		{
			Snapshot snapshot;
			Stamp defaults;
			Stamp user;
			Clock::time_point checked{};
			std::uint64_t used = 0;
			bool initialized = false;
		};

		std::mutex CacheMutex;
		std::map<std::wstring, Entry> Cache;
		std::uint64_t AccessSequence = 0;

		bool Stat(const std::filesystem::path& path, Stamp& result, std::string& error)
		{
			std::error_code statusError;
			result.exists = std::filesystem::exists(path, statusError);
			if (!statusError && !result.exists) return true;
			if (statusError || !std::filesystem::is_regular_file(path, statusError))
			{
				error = "Cannot inspect canonical AVISO settings file: " + path.u8string();
				return false;
			}
			result.size = std::filesystem::file_size(path, statusError);
			if (!statusError) result.writeTime = std::filesystem::last_write_time(path, statusError);
			if (statusError || result.size > MaximumBytes)
			{
				error = "Canonical AVISO settings file is unreadable or exceeds 16 MB: " + path.u8string();
				return false;
			}
			return true;
		}

		std::string Revision(const std::string& bytes)
		{
			// Concurrency token only, not a cryptographic/authentication primitive.
			std::uint64_t hash = 14695981039346656037ULL;
			for (const unsigned char byte : bytes) { hash ^= byte; hash *= 1099511628211ULL; }
			std::ostringstream result;
			result << std::hex << std::setfill('0') << std::setw(16) << hash;
			return result.str();
		}

		bool Load(const std::filesystem::path& path, bool isDefaults,
			rapidjson::Document& relevant, std::string& revision, std::string& error)
		{
			std::ifstream input(path, std::ios::binary | std::ios::ate);
			const std::streamoff length = input.is_open() ? static_cast<std::streamoff>(input.tellg()) : -1;
			if (length < 0 || static_cast<std::uintmax_t>(length) > MaximumBytes)
			{
				error = "Cannot read canonical AVISO settings (maximum 16 MB): " + path.u8string();
				return false;
			}
			std::string bytes(static_cast<size_t>(length), '\0');
			input.seekg(0);
			if (!bytes.empty()) input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
			if ((!bytes.empty() && input.gcount() != static_cast<std::streamsize>(bytes.size())) ||
				input.peek() != std::char_traits<char>::eof())
			{
				error = "Canonical AVISO settings changed while reading: " + path.u8string();
				return false;
			}
			VsmrJsonInputLimits::Limits limits;
			limits.maximumDepth = 64;
			limits.maximumValues = 500000;
			limits.maximumContainerEntries = 100000;
			limits.maximumStringBytes = 64U * 1024U;
			if (!VsmrJsonInputLimits::Validate(bytes, limits, error)) return false;
			rapidjson::Document parsed;
			if (VsmrJson::ParseDocument(parsed, bytes).HasParseError() || !parsed.IsObject())
			{
				error = "Canonical AVISO settings must be valid JSON objects: " + path.u8string();
				return false;
			}
			const auto* version = VsmrLayeredConfig::Member(parsed, "schema_version");
			if ((isDefaults && version == nullptr) ||
				(version != nullptr && (!version->IsInt() || version->GetInt() != 1)))
			{
				error = "Unsupported canonical AVISO configuration schema; user data was not changed.";
				return false;
			}
			relevant.SetObject();
			for (const char* key : { "aviso", "_migration" })
			{
				const auto* section = VsmrLayeredConfig::Member(parsed, key);
				if (section == nullptr) continue;
				if (!section->IsObject())
				{
					error = std::string("Invalid canonical AVISO configuration section: ") + key;
					return false;
				}
				VsmrLayeredConfig::Put(relevant, key, *section, relevant.GetAllocator());
			}
			revision = Revision(bytes);
			return true;
		}
	}

	Snapshot Read(const std::filesystem::path& dataRoot, bool forceRefresh)
	{
		try
		{
			if (dataRoot.empty()) return { nullptr, {}, "Canonical AVISO data directory is unavailable." };
			// Lexical normalization performs no filesystem probes on the frequent
			// render path; actual file stamps are throttled below.
			const auto directory = std::filesystem::absolute(dataRoot).lexically_normal();
			std::wstring key = directory.wstring();
			std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
			std::lock_guard<std::mutex> guard(CacheMutex);
			if (Cache.find(key) == Cache.end() && Cache.size() >= MaximumCachedRoots)
			{
				auto oldest = std::min_element(Cache.begin(), Cache.end(), [](const auto& left, const auto& right) {
					return left.second.used < right.second.used;
				});
				Cache.erase(oldest);
			}
			Entry& cached = Cache[key];
			cached.used = ++AccessSequence;
			const auto now = Clock::now();
			if (!forceRefresh && cached.initialized && now - cached.checked < std::chrono::milliseconds(500))
				return cached.snapshot;
			cached.checked = now;
			Stamp defaultsStamp;
			Stamp userStamp;
			std::string error;
			const auto defaultPath = directory / "default.json";
			const auto userPath = directory / "config.json";
			if (!Stat(defaultPath, defaultsStamp, error) || !Stat(userPath, userStamp, error))
			{
				cached.snapshot = { nullptr, {}, error };
				cached.initialized = true;
				return cached.snapshot;
			}
			if (!forceRefresh && cached.initialized && cached.snapshot.error.empty() &&
				cached.defaults == defaultsStamp && cached.user == userStamp) return cached.snapshot;
			cached.defaults = defaultsStamp;
			cached.user = userStamp;
			cached.initialized = true;
			if (userStamp.exists && !defaultsStamp.exists)
			{
				cached.snapshot = { nullptr, {}, "User AVISO configuration exists but its managed default.json is missing." };
				return cached.snapshot;
			}
			auto effective = std::make_shared<rapidjson::Document>();
			effective->SetObject();
			std::string defaultsRevision = "missing";
			std::string userRevision = "missing";
			if (defaultsStamp.exists && !Load(defaultPath, true, *effective, defaultsRevision, error))
			{
				cached.snapshot = { nullptr, {}, error };
				return cached.snapshot;
			}
			if (userStamp.exists)
			{
				rapidjson::Document user;
				if (!Load(userPath, false, user, userRevision, error))
				{
					cached.snapshot = { nullptr, {}, error };
					return cached.snapshot;
				}
				VsmrLayeredConfig::Merge(*effective, user, effective->GetAllocator());
			}
			cached.snapshot = { effective, defaultsRevision + ":" + userRevision, {} };
			return cached.snapshot;
		}
		catch (const std::exception& exception)
		{
			return { nullptr, {}, std::string("Cannot load canonical AVISO settings: ") + exception.what() };
		}
	}
}
