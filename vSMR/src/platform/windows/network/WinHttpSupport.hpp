#pragma once

#include <windows.h>
#include <winhttp.h>

namespace VsmrHttp
{
	class InternetHandle final
	{
	public:
		explicit InternetHandle(HINTERNET value = nullptr) noexcept : value_(value) {}
		~InternetHandle() { if (value_ != nullptr) ::WinHttpCloseHandle(value_); }
		InternetHandle(const InternetHandle&) = delete;
		InternetHandle& operator=(const InternetHandle&) = delete;
		HINTERNET get() const noexcept { return value_; }
		explicit operator bool() const noexcept { return value_ != nullptr; }
	private:
		HINTERNET value_ = nullptr;
	};

	[[nodiscard]] inline bool RequireModernTls(HINTERNET session) noexcept
	{
		DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#if defined(WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3)
		protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
		if (::WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols)))
			return true;
		// Windows versions without TLS 1.3 still support the TLS 1.2 floor.
		if (::GetLastError() != ERROR_INVALID_PARAMETER)
			return false;
		protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#endif
		return ::WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS,
			&protocols, sizeof(protocols)) != FALSE;
	}
}
