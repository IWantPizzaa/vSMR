#include "updater/UpdaterCore.hpp"
#include "updater/UpdaterCore.Internal.hpp"
#include "updater/UpdaterReleaseModel.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <rapidjson/document.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace vsmr::updater::internal
{
	using release_model::ParseSemVer;

	std::wstring QuoteCommandLineArgument(const std::wstring& argument)
	{
		if (argument.empty())
			return L"\"\"";
		if (argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
			return argument;
		std::wstring quoted = L"\"";
		std::size_t backslashes = 0;
		for (const wchar_t character : argument)
		{
			if (character == L'\\')
			{
				++backslashes;
				continue;
			}
			if (character == L'\"')
			{
				quoted.append(backslashes * 2 + 1, L'\\');
				quoted.push_back(L'\"');
				backslashes = 0;
				continue;
			}
			quoted.append(backslashes, L'\\');
			backslashes = 0;
			quoted.push_back(character);
		}
		quoted.append(backslashes * 2, L'\\');
		quoted.push_back(L'\"');
		return quoted;
	}

	fs::path PowerShellPath()
	{
		std::array<wchar_t, MAX_PATH + 1> windows{};
		const UINT length = ::GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size()));
		if (length == 0 || length >= windows.size())
			return {};
		const fs::path path = fs::path(std::wstring(windows.data(), length)) /
			L"System32" / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe";
		return IsRegularFile(path) ? path : fs::path{};
	}

	bool RunProcess(
		const fs::path& executable,
		const std::vector<std::wstring>& arguments,
		DWORD timeoutMs,
		DWORD& exitCode,
		bool terminateOnTimeout,
		const std::function<void()>& pulse,
		const std::vector<HANDLE>& handlesToInherit)
	{
		if (executable.empty() || !IsRegularFile(executable))
			return false;
		std::wstring commandLine = QuoteCommandLineArgument(executable.wstring());
		for (const auto& argument : arguments)
		{
			commandLine.push_back(L' ');
			commandLine += QuoteCommandLineArgument(argument);
		}
		std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
		mutableCommand.push_back(L'\0');
		STARTUPINFOEXW startup{};
		startup.StartupInfo.cb = sizeof(startup.StartupInfo);
		startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
		startup.StartupInfo.wShowWindow = SW_HIDE;
		std::vector<UniqueHandle> inheritedDuplicates;
		std::vector<HANDLE> inheritedValues;
		std::vector<BYTE> attributeStorage;
		if (!handlesToInherit.empty())
		{
			for (HANDLE handle : handlesToInherit)
			{
				if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
					return false;
				HANDLE duplicate = nullptr;
				if (!::DuplicateHandle(
					::GetCurrentProcess(), handle, ::GetCurrentProcess(), &duplicate,
					0, TRUE, DUPLICATE_SAME_ACCESS))
				{
					return false;
				}
				inheritedDuplicates.emplace_back(duplicate);
				inheritedValues.push_back(duplicate);
			}
			SIZE_T attributeBytes = 0;
			::InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
			if (attributeBytes == 0)
				return false;
			attributeStorage.resize(attributeBytes);
			startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
				attributeStorage.data());
			if (!::InitializeProcThreadAttributeList(
				startup.lpAttributeList, 1, 0, &attributeBytes))
			{
				startup.lpAttributeList = nullptr;
				return false;
			}
			if (!::UpdateProcThreadAttribute(
					startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
					inheritedValues.data(), inheritedValues.size() * sizeof(HANDLE),
					nullptr, nullptr))
			{
				if (startup.lpAttributeList != nullptr)
					::DeleteProcThreadAttributeList(startup.lpAttributeList);
				return false;
			}
			startup.StartupInfo.cb = sizeof(startup);
		}
		PROCESS_INFORMATION process{};
		const BOOL created = ::CreateProcessW(
			executable.c_str(), mutableCommand.data(), nullptr, nullptr,
			handlesToInherit.empty() ? FALSE : TRUE,
			CREATE_NO_WINDOW | (handlesToInherit.empty() ? 0 : EXTENDED_STARTUPINFO_PRESENT),
			nullptr, nullptr, &startup.StartupInfo, &process);
		if (startup.lpAttributeList != nullptr)
			::DeleteProcThreadAttributeList(startup.lpAttributeList);
		// The parent keeps only its original transaction lock. The duplicated
		// inheritable handles exist solely in the child after CreateProcess.
		inheritedDuplicates.clear();
		if (!created)
		{
			return false;
		}
		UniqueHandle processHandle(process.hProcess);
		UniqueHandle threadHandle(process.hThread);
		const ULONGLONG started = ::GetTickCount64();
		for (;;)
		{
			const DWORD wait = ::WaitForSingleObject(processHandle.get(), 250);
			if (wait == WAIT_OBJECT_0)
				break;
			if (wait != WAIT_TIMEOUT)
				return false;
			if (pulse)
				pulse();
			if (timeoutMs != INFINITE && ::GetTickCount64() - started >= timeoutMs)
			{
				if (terminateOnTimeout)
				{
					::TerminateProcess(processHandle.get(), ERROR_TIMEOUT);
					::WaitForSingleObject(processHandle.get(), 5000);
				}
				return false;
			}
		}
		return ::GetExitCodeProcess(processHandle.get(), &exitCode) && exitCode == 0;
	}

	bool IsPathBelow(const fs::path& child, const fs::path& parent)
	{
		std::error_code error;
		const std::wstring childText = ToLowerWide(fs::absolute(child, error).lexically_normal().wstring());
		if (error)
			return false;
		const std::wstring parentText = ToLowerWide(fs::absolute(parent, error).lexically_normal().wstring());
		if (error || childText.size() <= parentText.size())
			return false;
		return childText.compare(0, parentText.size(), parentText) == 0 &&
			(childText[parentText.size()] == L'\\' || childText[parentText.size()] == L'/');
	}

	const char kSafeExtractionScript[] = R"VSMRPS(
param(
    [Parameter(Mandatory=$true)][string]$Archive,
    [Parameter(Mandatory=$true)][string]$Destination
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archivePath = [IO.Path]::GetFullPath($Archive)
$destinationPath = [IO.Path]::GetFullPath($Destination).TrimEnd('\','/')
if (-not [IO.File]::Exists($archivePath)) { throw 'Archive missing.' }
if ([IO.Directory]::Exists($destinationPath)) { [IO.Directory]::Delete($destinationPath, $true) }
[IO.Directory]::CreateDirectory($destinationPath) | Out-Null
$rootPrefix = $destinationPath + [IO.Path]::DirectorySeparatorChar
$seen = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
$zip = [IO.Compression.ZipFile]::OpenRead($archivePath)
try {
    if ($zip.Entries.Count -gt 20000) { throw 'Archive has too many entries.' }
    [UInt64]$total = 0
    foreach ($entry in $zip.Entries) {
        $relative = $entry.FullName.Replace('/', '\')
        if ([string]::IsNullOrWhiteSpace($relative) -or [IO.Path]::IsPathRooted($relative) -or
            $relative.Contains(':') -or $relative.StartsWith('\') -or
            @($relative.Split('\') | Where-Object { $_ -eq '..' }).Count -gt 0) {
            throw "Unsafe archive entry: $relative"
        }
        $target = [IO.Path]::GetFullPath([IO.Path]::Combine($destinationPath, $relative))
        if (-not $target.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Archive entry escapes destination: $relative"
        }
        $key = $target.TrimEnd('\','/')
        if (-not $seen.Add($key)) { throw "Duplicate archive entry: $relative" }
        if ([UInt64]$entry.Length -gt 268435456) { throw "Archive entry too large: $relative" }
        $total += [UInt64]$entry.Length
        if ($total -gt 805306368) { throw 'Archive expanded size is too large.' }
    }
    foreach ($entry in $zip.Entries) {
        $relative = $entry.FullName.Replace('/', '\')
        $target = [IO.Path]::GetFullPath([IO.Path]::Combine($destinationPath, $relative))
        if ($relative.EndsWith('\')) {
            [IO.Directory]::CreateDirectory($target) | Out-Null
            continue
        }
        $parent = [IO.Path]::GetDirectoryName($target)
        if (-not [string]::IsNullOrWhiteSpace($parent)) { [IO.Directory]::CreateDirectory($parent) | Out-Null }
        $source = $entry.Open()
        try {
            $destinationFile = New-Object IO.FileStream($target, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
            try { $source.CopyTo($destinationFile) } finally { $destinationFile.Dispose() }
        }
        finally { $source.Dispose() }
    }
}
finally { $zip.Dispose() }
)VSMRPS";

	bool SafelyExtractArchive(
		Context& context,
		const fs::path& archive,
		const fs::path& destination,
		std::string& error)
	{
		if (!IsPathBelow(destination, context.storageRoot))
		{
			error = "unsafe_extraction_destination";
			return false;
		}
		const fs::path script = context.storageRoot / L"safe_extract.ps1";
		if (!AtomicWriteText(script, kSafeExtractionScript))
		{
			error = "extractor_write_failed";
			return false;
		}
		DWORD exitCode = 0;
		const DWORD timeout = RemainingMs(context, 30000);
		if (timeout < 1000)
		{
			error = "deadline";
			return false;
		}
		if (!RunProcess(
			PowerShellPath(),
			{ L"-NoLogo", L"-NoProfile", L"-NonInteractive", L"-ExecutionPolicy", L"Bypass",
			  L"-File", script.wstring(), L"-Archive", archive.wstring(),
			  L"-Destination", destination.wstring() },
			timeout, exitCode, true))
		{
			error = exitCode == ERROR_TIMEOUT ? "extraction_timeout" : "extraction_failed";
			return false;
		}
		return true;
	}

	OwnedMutex AcquireUpdaterMutex(const fs::path& installRoot)
	{
		(void)installRoot;
		const std::wstring name = L"Local\\vSMR.Updater.Global";
		UniqueHandle mutex(::CreateMutexW(nullptr, FALSE, name.c_str()));
		if (!mutex)
			return {};
		const DWORD wait = ::WaitForSingleObject(mutex.get(), 0);
		if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
			return {};
		return OwnedMutex(std::move(mutex));
	}

	OwnedMutex AcquireHealthMarkerMutex(const fs::path& markerPath)
	{
		std::error_code error;
		const std::wstring normalized = ToLowerWide(
			fs::absolute(markerPath, error).lexically_normal().wstring());
		if (error || normalized.empty())
			return {};
		std::wostringstream name;
		name << L"Local\\vSMR.Updater.Health." << std::hex << std::setfill(L'0')
			<< std::setw(16) << Fnv1a64(normalized);
		UniqueHandle mutex(::CreateMutexW(nullptr, FALSE, name.str().c_str()));
		if (!mutex)
			return {};
		const DWORD wait = ::WaitForSingleObject(mutex.get(), 5000);
		if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
			return {};
		return OwnedMutex(std::move(mutex));
	}

	UniqueHandle AcquireExclusiveSessionLock(
		const fs::path& storageRoot,
		const fs::path& installRoot)
	{
		if (storageRoot.empty() || installRoot.empty())
			return {};
		const fs::path path = SessionLockPath(storageRoot, installRoot);
		std::error_code error;
		fs::create_directories(path.parent_path(), error);
		if (error)
			return {};
		return UniqueHandle(::CreateFileW(
			path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
			OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr));
	}

	bool CopyFileAtomically(const fs::path& source, const fs::path& destination)
	{
		std::error_code error;
		fs::create_directories(destination.parent_path(), error);
		if (error)
			return false;
		const fs::path temporary = destination.wstring() + L".tmp." + std::to_wstring(::GetTickCount64());
		if (!::CopyFileW(source.c_str(), temporary.c_str(), TRUE))
			return false;
		if (!::MoveFileExW(
			temporary.c_str(), destination.c_str(),
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			::DeleteFileW(temporary.c_str());
			return false;
		}
		return true;
	}

	bool ReadInstallationMetadata(
		const fs::path& dataRoot,
		std::string& installedVersion,
		fs::path& rollbackBackup)
	{
		std::string json;
		if (!ReadText(dataRoot / L"INSTALLATION.json", json, 128 * 1024))
			return false;
		rapidjson::Document document;
		document.Parse<0>(json.c_str());
		if (document.HasParseError() || !document.IsObject())
			return false;
		installedVersion = JsonString(document, "installed_version");
		rollbackBackup = fs::path(Utf8ToWide(JsonString(document, "rollback_backup")));
		return ParseSemVer(installedVersion).valid && !rollbackBackup.empty();
	}

	fs::path FindNewestRollbackBackup(
		const fs::path& storageRoot,
		const fs::path& installRoot,
		const std::string& installingVersion)
	{
		const fs::path backupRoot = storageRoot / L"backups" / HashName(installRoot);
		std::error_code error;
		fs::path selected;
		fs::file_time_type selectedTime{};
		for (fs::directory_iterator iterator(backupRoot, error), end;
			!error && iterator != end; iterator.increment(error))
		{
			if (!iterator->is_directory(error) || error)
				continue;
			std::string json;
			if (!ReadText(iterator->path() / L"BACKUP-METADATA.json", json, 128 * 1024))
				continue;
			rapidjson::Document document;
			document.Parse<0>(json.c_str());
			if (document.HasParseError() || !document.IsObject() ||
				JsonString(document, "kind") != "vSMR complete pre-install backup" ||
				JsonString(document, "installing_version") != installingVersion)
			{
				continue;
			}
			std::error_code timeError;
			const auto time = fs::last_write_time(iterator->path(), timeError);
			if (!timeError && (selected.empty() || time > selectedTime))
			{
				selected = iterator->path();
				selectedTime = time;
			}
		}
		return selected;
	}

	bool ReadReleaseVersion(const fs::path& dataRoot, std::string& version)
	{
		std::string json;
		if (!ReadText(dataRoot / L"RELEASE-METADATA.json", json, 128 * 1024))
			return false;
		rapidjson::Document document;
		document.Parse<0>(json.c_str());
		if (document.HasParseError() || !document.IsObject())
			return false;
		version = JsonString(document, "version");
		return ParseSemVer(version).valid;
	}

	fs::path RecoveryScriptPath(
		const fs::path& storageRoot,
		const fs::path& installRoot,
		const std::string& version)
	{
		return storageRoot / L"recovery" / HashName(installRoot) /
			Utf8ToWide(version) / L"restore_vsmr_backup.ps1";
	}

	bool StageRecoveryScript(
		Context& context,
		const fs::path& packageRoot,
		const Manifest& manifest,
		std::string& error)
	{
		const fs::path source = packageRoot / L"vSMR_Data" / L"Tools" / L"restore_vsmr_backup.ps1";
		const fs::path destination = RecoveryScriptPath(
			context.storageRoot, context.options.installRoot, manifest.version.normalized);
		if (!IsRegularFile(source) || !CopyFileAtomically(source, destination))
		{
			error = "recovery_helper_staging_failed";
			return false;
		}
		return true;
	}

	bool RunInstaller(
		Context& context,
		const fs::path& packageRoot,
		bool preserveLoader,
		bool reloadAviso,
		bool replaceModifiedAviso,
		HANDLE installationSessionLock,
		std::string& error)
	{
		const fs::path installer = packageRoot / L"vSMR_Data" / L"Tools" / L"install_vsmr.ps1";
		if (!IsRegularFile(installer))
		{
			error = "package_installer_missing";
			return false;
		}
		const fs::path backupRoot = context.storageRoot / L"backups" / HashName(context.options.installRoot);
		std::vector<std::wstring> arguments = {
			L"-NoLogo", L"-NoProfile", L"-NonInteractive", L"-ExecutionPolicy", L"Bypass",
			L"-File", installer.wstring(),
			L"-DestinationDirectory", context.options.installRoot.wstring(),
			L"-BackupRoot", backupRoot.wstring()
		};
		if (preserveLoader)
			arguments.push_back(L"-PreserveLoader");
		if (reloadAviso)
			arguments.push_back(L"-ReloadAviso");
		if (replaceModifiedAviso)
			arguments.push_back(L"-ReplaceModifiedAviso");
		DWORD exitCode = 0;
		ULONGLONG lastPulse = 0;
		if (!RunProcess(
			PowerShellPath(), arguments, INFINITE, exitCode, false,
			[&]() {
				const ULONGLONG now = ::GetTickCount64();
				if (now - lastPulse >= 1000)
				{
					lastPulse = now;
					Report(
						context, ProgressStage::Installing, -1,
						reloadAviso ? L"Reloading AVISO data..." : L"Installing vSMR update...");
				}
			},
			{ installationSessionLock }))
		{
			error = exitCode == ERROR_TIMEOUT ? "installer_timeout" : "installer_failed";
			return false;
		}
		return true;
	}

}
