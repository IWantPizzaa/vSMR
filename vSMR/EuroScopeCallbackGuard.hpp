#pragma once

#include "Logger.h"

#include <exception>
#include <sstream>
#include <string>
#include <utility>

namespace vsmr
{
	inline std::string FormatHResult(SCODE result)
	{
		std::ostringstream stream;
		stream << "0x" << std::hex << std::uppercase << static_cast<unsigned long>(result);
		return stream.str();
	}

	inline std::string MfcErrorTextToString(const TCHAR* text)
	{
		if (text == nullptr || text[0] == 0)
			return std::string();

#ifdef UNICODE
		std::wstring wideText(text);
		return std::string(wideText.begin(), wideText.end());
#else
		return std::string(text);
#endif
	}

	inline void LogAndDeleteMfcException(const std::string& callbackName, COleException* exception)
	{
		const SCODE result = exception != nullptr ? exception->m_sc : E_FAIL;
		Logger::info("COleException in " + callbackName + " HRESULT=" + FormatHResult(result));

		if (exception != nullptr)
			exception->Delete();
	}

	inline void LogAndDeleteMfcException(const std::string& callbackName, CException* exception)
	{
		TCHAR errorText[512] = {};
		if (exception != nullptr)
			exception->GetErrorMessage(errorText, _countof(errorText));

		const std::string message = MfcErrorTextToString(errorText);
		Logger::info("MFC exception in " + callbackName + (message.empty() ? "" : ": " + message));

		if (exception != nullptr)
			exception->Delete();
	}

	template <typename Callback>
	void RunEuroScopeCallback(const std::string& callbackName, Callback&& callback)
	{
		try
		{
			std::forward<Callback>(callback)();
		}
		catch (COleException* exception)
		{
			LogAndDeleteMfcException(callbackName, exception);
		}
		catch (CException* exception)
		{
			LogAndDeleteMfcException(callbackName, exception);
		}
		catch (const std::exception& exception)
		{
			Logger::info("std::exception in " + callbackName + ": " + exception.what());
		}
		catch (...)
		{
			Logger::info("Unknown exception in " + callbackName);
		}
	}

	template <typename Result, typename Callback>
	Result RunEuroScopeCallbackOr(const std::string& callbackName, Result fallback, Callback&& callback)
	{
		try
		{
			return std::forward<Callback>(callback)();
		}
		catch (COleException* exception)
		{
			LogAndDeleteMfcException(callbackName, exception);
		}
		catch (CException* exception)
		{
			LogAndDeleteMfcException(callbackName, exception);
		}
		catch (const std::exception& exception)
		{
			Logger::info("std::exception in " + callbackName + ": " + exception.what());
		}
		catch (...)
		{
			Logger::info("Unknown exception in " + callbackName);
		}

		return fallback;
	}
}
