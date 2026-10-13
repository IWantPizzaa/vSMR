#include "updater/GroundLayoutAviso.hpp"
#include "updater/UpdaterTransport.hpp"
#include "shared/JsonDocument.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>

#include <rapidjson/document.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace fs = std::filesystem;

namespace vsmr::updater::ground_layout_aviso
{
	namespace
	{
		constexpr std::uint64_t MaximumFileBytes = 32ULL * 1024 * 1024;
		constexpr std::uint64_t MaximumTotalBytes = 128ULL * 1024 * 1024;
		constexpr ULONGLONG StartupBudgetMs = 30000;
		const wchar_t* ApiRoot = L"https://api.github.com/repos/vaccfr/France-Ground-Layouts/";
		const wchar_t* RawRoot = L"https://raw.githubusercontent.com/vaccfr/France-Ground-Layouts/";

		bool Hex40(const std::string& value)
		{
			return value.size() == 40 && std::all_of(value.begin(), value.end(), [](char c) {
				return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
			});
		}

		bool AirportFile(const std::string& path)
		{
			if (path.size() != 20 || path.substr(0, 8) != "GeoJSON/" ||
				path.substr(12) != ".geojson") return false;
			return std::all_of(path.begin() + 8, path.begin() + 12, [](char c) {
				return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
			});
		}

		std::string String(const rapidjson::Value& object, const char* key)
		{
			if (!object.IsObject()) return {};
			const auto it = object.FindMember(key);
			return it != object.MemberEnd() && it->value.IsString()
				? std::string(it->value.GetString(), it->value.GetStringLength()) : std::string{};
		}

		bool Parse(const std::vector<std::uint8_t>& bytes, rapidjson::Document& document)
		{
			const std::string json(bytes.begin(), bytes.end());
			VsmrJson::ParseDocument(document, json);
			return !document.HasParseError() && document.IsObject();
		}

		bool BlobHash(const std::vector<std::uint8_t>& bytes, std::string& result)
		{
			result.clear();
			BCRYPT_HASH_HANDLE hash = nullptr;
			if (BCryptCreateHash(BCRYPT_SHA1_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0) < 0) return false;
			const std::string header = "blob " + std::to_string(bytes.size()) + std::string(1, '\0');
			std::array<UCHAR, 20> digest{};
			const bool good = BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(header.data())),
				static_cast<ULONG>(header.size()), 0) >= 0 &&
				(bytes.empty() || BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()),
					static_cast<ULONG>(bytes.size()), 0) >= 0) &&
				BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
			BCryptDestroyHash(hash);
			if (!good) return false;
			static constexpr char digits[] = "0123456789abcdef";
			for (const UCHAR byte : digest) { result += digits[byte >> 4]; result += digits[byte & 15]; }
			return true;
		}

		bool ReadFile(const fs::path& path, std::uint64_t limit, std::vector<std::uint8_t>& bytes)
		{
			std::error_code error;
			const auto size = fs::file_size(path, error);
			if (error || size > limit) return false;
			std::ifstream input(path, std::ios::binary);
			if (!input) return false;
			bytes.resize(static_cast<std::size_t>(size));
			return bytes.empty() || static_cast<bool>(input.read(reinterpret_cast<char*>(bytes.data()),
				static_cast<std::streamsize>(bytes.size())));
		}

		bool WriteAtomic(const fs::path& path, const std::vector<std::uint8_t>& bytes)
		{
			std::error_code error;
			fs::create_directories(path.parent_path(), error);
			if (error) return false;
			const fs::path temporary = path.wstring() + L".tmp." + std::to_wstring(GetCurrentProcessId());
			{
				std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
				if (!output || (!bytes.empty() && !output.write(reinterpret_cast<const char*>(bytes.data()),
					static_cast<std::streamsize>(bytes.size()))) || !output.flush()) return false;
			}
			if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			{
				fs::remove(temporary, error);
				return false;
			}
			return true;
		}

		transport::Response Get(const std::wstring& url, ULONGLONG started, std::uint64_t limit)
		{
			const ULONGLONG elapsed = GetTickCount64() - started;
			transport::Request request;
			request.initialUrl = url;
			request.timeoutMs = static_cast<std::uint32_t>((std::min)(
				StartupBudgetMs > elapsed ? StartupBudgetMs - elapsed : static_cast<ULONGLONG>(0),
				static_cast<ULONGLONG>(5000)));
			request.maximumBytes = limit;
			return transport::HttpGet(request, [] { return false; }, [](int) { return true; });
		}

		bool ValidGeoJson(const std::vector<std::uint8_t>& bytes)
		{
			rapidjson::Document document;
			return Parse(bytes, document) && String(document, "type") == "FeatureCollection" &&
				document.HasMember("features") && document["features"].IsArray();
		}
	}

	fs::path ActiveDirectory(const fs::path& dataRoot)
	{
		const fs::path root = dataRoot / L"UserData" / L"France-Ground-Layouts";
		std::ifstream input(root / L"current.txt", std::ios::binary);
		std::string commit;
		if (!input || !std::getline(input, commit) || !Hex40(commit)) return {};
		const fs::path directory = root / fs::u8path(commit) / L"GeoJSON";
		std::error_code error;
		return fs::is_directory(directory, error) && !error ? directory : fs::path{};
	}

	bool Sync(const fs::path& dataRoot, std::wstring& message) noexcept
	{
		try
		{
			const ULONGLONG started = GetTickCount64();
			const fs::path root = dataRoot / L"UserData" / L"France-Ground-Layouts";
			std::error_code filesystemError;
			fs::create_directories(root, filesystemError);
			if (filesystemError) { message = L"Ground-layout cache is not writable."; return false; }
			struct Lock
			{
				HANDLE handle = INVALID_HANDLE_VALUE;
				~Lock() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
			} lock;
			while (GetTickCount64() - started < StartupBudgetMs)
			{
				lock.handle = CreateFileW((root / L"sync.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
					0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
				if (lock.handle != INVALID_HANDLE_VALUE) break;
				if (GetLastError() != ERROR_SHARING_VIOLATION) break;
				Sleep(100);
			}
			if (lock.handle == INVALID_HANDLE_VALUE)
			{
				message = L"Ground-layout update is busy; retaining cached AVISO.";
				return false;
			}
			const auto commitResponse = Get(std::wstring(ApiRoot) + L"commits/master", started, 2 * 1024 * 1024);
			rapidjson::Document commitJson;
			if (commitResponse.statusCode != 200 || !commitResponse.error.empty() || !Parse(commitResponse.body, commitJson))
			{
				message = L"France-Ground-Layouts check unavailable; retaining cached AVISO.";
				return false;
			}
			const std::string commit = String(commitJson, "sha");
			std::string treeSha;
			const auto commitMember = commitJson.FindMember("commit");
			if (commitMember != commitJson.MemberEnd() && commitMember->value.IsObject())
			{
				const auto treeMember = commitMember->value.FindMember("tree");
				if (treeMember != commitMember->value.MemberEnd()) treeSha = String(treeMember->value, "sha");
			}
			if (!Hex40(commit) || !Hex40(treeSha)) { message = L"Invalid ground-layout commit."; return false; }
			const auto treeResponse = Get(std::wstring(ApiRoot) + L"git/trees/" +
				std::wstring(treeSha.begin(), treeSha.end()) + L"?recursive=1", started, 4 * 1024 * 1024);
			rapidjson::Document treeJson;
			if (treeResponse.statusCode != 200 || !treeResponse.error.empty() || !Parse(treeResponse.body, treeJson) ||
				String(treeJson, "sha") != treeSha || !treeJson.HasMember("truncated") ||
				!treeJson["truncated"].IsBool() || treeJson["truncated"].GetBool() ||
				!treeJson.HasMember("tree") || !treeJson["tree"].IsArray())
			{
				message = L"Ground-layout file list unavailable or incomplete; retaining cached AVISO.";
				return false;
			}
			std::map<std::string, std::pair<std::string, std::uint64_t>> files;
			std::uint64_t total = 0;
			for (const auto& entry : treeJson["tree"].GetArray())
			{
				if (!entry.IsObject() || !AirportFile(String(entry, "path"))) continue;
				const std::string name = String(entry, "path").substr(8);
				const std::string sha = String(entry, "sha");
				const auto size = entry.FindMember("size");
				if (String(entry, "type") != "blob" || !Hex40(sha) || size == entry.MemberEnd() ||
					!size->value.IsUint64() || size->value.GetUint64() == 0 ||
					size->value.GetUint64() > MaximumFileBytes || total > MaximumTotalBytes - size->value.GetUint64() ||
					!files.emplace(name, std::make_pair(sha, size->value.GetUint64())).second)
				{
					message = L"Invalid ground-layout file list; retaining cached AVISO.";
					return false;
				}
				total += size->value.GetUint64();
			}
			if (files.empty() || files.size() > 512) { message = L"Invalid ground-layout file count."; return false; }
			const fs::path destination = root / fs::u8path(commit) / L"GeoJSON";
			const fs::path previous = ActiveDirectory(dataRoot);
			for (const auto& [name, identity] : files)
			{
				const fs::path target = destination / fs::u8path(name);
				std::vector<std::uint8_t> bytes;
				std::string hash;
				if (ReadFile(target, identity.second, bytes) && bytes.size() == identity.second &&
					BlobHash(bytes, hash) && hash == identity.first && ValidGeoJson(bytes)) continue;
				const fs::path candidates[] = { previous / fs::u8path(name), dataRoot / L"AVISO" / fs::u8path(name) };
				bool found = false;
				for (const fs::path& candidate : candidates)
				{
					if (ReadFile(candidate, identity.second, bytes) && bytes.size() == identity.second &&
						BlobHash(bytes, hash) && hash == identity.first && ValidGeoJson(bytes))
						{ found = true; break; }
				}
				if (!found)
				{
					const std::wstring relative(name.begin(), name.end());
					const auto response = Get(std::wstring(RawRoot) + std::wstring(commit.begin(), commit.end()) +
						L"/GeoJSON/" + relative, started, identity.second);
					if (response.statusCode != 200 || !response.error.empty() || response.body.size() != identity.second ||
						!BlobHash(response.body, hash) || hash != identity.first || !ValidGeoJson(response.body))
					{
						message = L"Ground-layout download incomplete; retaining cached AVISO.";
						return false;
					}
					bytes = response.body;
				}
				if (!WriteAtomic(target, bytes)) { message = L"Could not save ground-layout AVISO cache."; return false; }
			}
			const std::vector<std::uint8_t> pointer(commit.begin(), commit.end());
			if (!WriteAtomic(root / L"current.txt", pointer)) { message = L"Could not activate ground-layout AVISO cache."; return false; }
			message = L"France-Ground-Layouts AVISO ready at commit " + std::wstring(commit.begin(), commit.end()) + L".";
			return true;
		}
		catch (...) { message = L"Ground-layout AVISO update failed; retaining cached AVISO."; return false; }
	}
}
