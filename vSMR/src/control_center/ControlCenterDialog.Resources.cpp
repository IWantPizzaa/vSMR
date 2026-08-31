#include "platform/windows/PrecompiledHeader.hpp"
#include "control_center/ControlCenterDialog.hpp"
#include "control_center/ControlCenterDialog.Internal.hpp"

#include "platform/windows/network/HttpHelper.hpp"
#include "shared/logging/Logger.hpp"
#include "radar/RadarScreen.hpp"
#include "control_center/ControlCenterBridge.hpp"
#include "control_center/RuntimeResourceFiles.hpp"
#include "crash/CrashRuntime.hpp"

#include <filesystem>
#include <memory>
#include <new>
#include <utility>

using namespace VsmrControlCenterDialogInternal;

struct CVsmrControlCenterDialog::GithubDownloadResult
{
	std::string resource;
	std::string source;
	std::string requestId;
	std::string body;
	bool failed = false;
};

void CVsmrControlCenterDialog::RequestGithubResource(
	const std::string& resource,
	const std::string& url,
	const std::string& requestId)
{
	if ((resource != "profiles" && resource != "aviso") ||
		!HttpHelper::IsHttpsUrlForHost(
			url,
			"raw.githubusercontent.com"))
	{
		if (Bridge)
			Bridge->PushError(
				requestId,
				"Only HTTPS raw.githubusercontent.com file URLs are allowed.");
		return;
	}
	if (GithubDownloadInProgress.exchange(true))
	{
		if (Bridge)
			Bridge->PushError(
				requestId,
				"Another GitHub resource is still loading.");
		return;
	}
	if (GithubDownloadThread.joinable())
		GithubDownloadThread.join();
	GithubDownloadCancellationRequested.store(false, std::memory_order_release);

	const HWND target = GetSafeHwnd();
	std::weak_ptr<std::atomic<bool>> weakLifetime = LifetimeToken;
	try
	{
		GithubDownloadThread = std::thread(
			[this, target, weakLifetime, resource, url, requestId]()
			{
				VsmrCrashRuntime::OwnedThreadRole crashThreadRole(
					"Control Center download worker");
				std::unique_ptr<GithubDownloadResult> result(
					new (std::nothrow) GithubDownloadResult());
				if (!result)
				{
					GithubDownloadInProgress.store(false, std::memory_order_release);
					const auto lifetime = weakLifetime.lock();
					if (lifetime && lifetime->load() && ::IsWindow(target))
						::PostMessage(target, kGithubDownloadCompleteMessage, 1, 0);
					return;
				}
				try
				{
					result->resource = resource;
					result->source = url;
					result->requestId = requestId;
					HttpHelper helper;
					result->body = helper.downloadStringFromURL(
						url,
						10000,
						&GithubDownloadCancellationRequested,
						kMaximumResourceBytes);
				}
				catch (const std::exception& ex)
				{
					result->failed = true;
					Logger::info(
						"Control Center GitHub download exception: " +
						std::string(ex.what()));
				}
				catch (...)
				{
					result->failed = true;
					Logger::info(
						"Control Center GitHub download exception: unknown");
				}

				if (GithubDownloadCancellationRequested.load(
					std::memory_order_acquire))
				{
					return;
				}
				const auto lifetime = weakLifetime.lock();
				if (!lifetime || !lifetime->load() || !::IsWindow(target))
					return;
				GithubDownloadResult* raw = result.release();
				if (!::PostMessage(
					target,
					kGithubDownloadCompleteMessage,
					0,
					reinterpret_cast<LPARAM>(raw)))
				{
					delete raw;
					GithubDownloadInProgress.store(
						false,
						std::memory_order_release);
				}
			});
	}
	catch (const std::exception& ex)
	{
		GithubDownloadInProgress.store(false);
		Logger::info(
			"Unable to start Control Center GitHub download: " +
			std::string(ex.what()));
		if (Bridge)
			Bridge->PushError(requestId, "Unable to start the GitHub download.");
	}
	catch (...)
	{
		GithubDownloadInProgress.store(false);
		Logger::info("Unable to start Control Center GitHub download: unknown");
		if (Bridge)
			Bridge->PushError(requestId, "Unable to start the GitHub download.");
	}
}

void CVsmrControlCenterDialog::StopGithubDownload()
{
	GithubDownloadCancellationRequested.store(true, std::memory_order_release);
	if (GithubDownloadThread.joinable())
	{
		::CancelSynchronousIo(GithubDownloadThread.native_handle());
		GithubDownloadThread.join();
	}
	// The worker may have posted completion immediately before cancellation.
	// Drain that owned payload before another request resets the cancellation
	// flag, otherwise a stale download could activate after a reload.
	MSG queued = {};
	const HWND dialogWindow = GetSafeHwnd();
	while (::IsWindow(dialogWindow) && ::PeekMessage(
		&queued,
		dialogWindow,
		kGithubDownloadCompleteMessage,
		kGithubDownloadCompleteMessage,
		PM_REMOVE))
	{
		delete reinterpret_cast<GithubDownloadResult*>(queued.lParam);
	}
	GithubDownloadInProgress.store(false, std::memory_order_release);
}

LRESULT CVsmrControlCenterDialog::OnGithubDownloadComplete(
	WPARAM wParam,
	LPARAM lParam)
{
	std::unique_ptr<GithubDownloadResult> result(
		reinterpret_cast<GithubDownloadResult*>(lParam));
	GithubDownloadInProgress.store(false);
	if (GithubDownloadThread.joinable())
		GithubDownloadThread.join();
	if (GithubDownloadCancellationRequested.load(std::memory_order_acquire))
		return 0;
	if (!result)
	{
		if (wParam != 0 && Bridge)
			Bridge->PushError("", "GitHub download failed unexpectedly.");
		return 0;
	}
	if (result->failed)
	{
		if (Bridge)
			Bridge->PushError(
				result->requestId,
				"GitHub download failed unexpectedly.");
		return 0;
	}
	if (result->body.empty())
	{
		if (Bridge)
			Bridge->PushError(
				result->requestId,
				"GitHub download failed or returned an empty file.");
		return 0;
	}
	if (!Bridge)
		return 0;

	std::string validationError;
	if (!Bridge->ValidateLoadedResource(
		result->resource,
		result->body,
		validationError))
	{
		Bridge->PushError(
			result->requestId,
			validationError.empty()
				? "The downloaded resource is invalid."
				: validationError);
		return 0;
	}

	const std::filesystem::path dataDirectory = Owner != nullptr && !Owner->GetDataPath().empty()
		? std::filesystem::u8path(Owner->GetDataPath())
		: std::filesystem::u8path(Logger::DLL_PATH) / "vSMR_Data";
	const VsmrResourceFiles::Kind kind = result->resource == "profiles"
		? VsmrResourceFiles::Kind::Profiles
		: VsmrResourceFiles::Kind::Aviso;
	std::string storedPath;
	std::string storageError;
	if (!VsmrResourceFiles::StoreGithubDownload(
		kind,
		dataDirectory.u8string(),
		result->source,
		Owner != nullptr ? Owner->getActiveAirport() : std::string(),
		result->body,
		storedPath,
		storageError))
	{
		Bridge->PushError(
			result->requestId,
			storageError.empty()
				? "Unable to store the downloaded resource."
				: storageError);
		return 0;
	}

	if (!Bridge->HandleLoadedResource(
		result->resource,
		result->source,
		result->requestId,
		result->body,
		storedPath))
	{
		std::error_code removeError;
		std::filesystem::remove(storedPath, removeError);
		if (removeError)
		{
			Logger::info(
				"Control Center rejected resource cleanup failed path=" +
				storedPath + " error=" + removeError.message());
			Bridge->PushError(
				result->requestId,
				"The resource was rejected, but its downloaded variant could not be removed: " +
				storedPath);
		}
	}
	return 0;
}
