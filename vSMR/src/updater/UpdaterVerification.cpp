#include "updater/UpdaterVerification.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "wintrust.lib")

#ifndef VSMR_UPDATE_SIGNER_CERT_SHA256
#define VSMR_UPDATE_SIGNER_CERT_SHA256 ""
#endif

namespace vsmr::updater::verification
{
	namespace
	{
		class FileHandle
		{
		public:
			explicit FileHandle(HANDLE value) noexcept : value_(value) {}
			~FileHandle()
			{
				if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE)
					::CloseHandle(value_);
			}
			explicit operator bool() const noexcept
			{
				return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
			}
			HANDLE get() const noexcept
			{
				return value_;
			}

		private:
			HANDLE value_ = nullptr;
		};

		class CertContextHandle
		{
		public:
			explicit CertContextHandle(PCCERT_CONTEXT value = nullptr) noexcept : value_(value) {}
			~CertContextHandle()
			{
				if (value_ != nullptr)
					::CertFreeCertificateContext(value_);
			}
			explicit operator bool() const noexcept
			{
				return value_ != nullptr;
			}
			PCCERT_CONTEXT get() const noexcept
			{
				return value_;
			}

		private:
			PCCERT_CONTEXT value_ = nullptr;
		};

		std::string ToLowerAscii(std::string value)
		{
			std::transform(value.begin(), value.end(), value.begin(),
				[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
			return value;
		}

		bool IsHex(const std::string& value, std::size_t length)
		{
			return value.size() == length && std::all_of(value.begin(), value.end(),
												 [](unsigned char character) { return std::isxdigit(character) != 0; });
		}

		std::string Hex(const BYTE* bytes, DWORD size)
		{
			std::ostringstream output;
			output << std::hex << std::setfill('0');
			for (DWORD index = 0; index < size; ++index)
				output << std::setw(2) << static_cast<unsigned int>(bytes[index]);
			return output.str();
		}

		bool Sha256Bytes(const BYTE* bytes, std::size_t size, std::string& digest)
		{
			BCRYPT_ALG_HANDLE algorithm = nullptr;
			BCRYPT_HASH_HANDLE hash = nullptr;
			std::vector<BYTE> hashObject;
			std::array<BYTE, 32> result{};
			DWORD objectLength = 0, resultLength = 0;
			bool success = false;
			if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
				goto cleanup;
			if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength),
					sizeof(objectLength), &resultLength, 0) < 0 ||
				objectLength == 0)
				goto cleanup;
			hashObject.resize(objectLength);
			if (BCryptCreateHash(algorithm, &hash, hashObject.data(), objectLength, nullptr, 0, 0) < 0)
				goto cleanup;
			while (size > 0)
			{
				const ULONG chunk = static_cast<ULONG>((std::min)(size, static_cast<std::size_t>(1024 * 1024)));
				if (BCryptHashData(hash, const_cast<PUCHAR>(bytes), chunk, 0) < 0)
					goto cleanup;
				bytes += chunk;
				size -= chunk;
			}
			if (BCryptFinishHash(hash, result.data(), static_cast<ULONG>(result.size()), 0) < 0)
				goto cleanup;
			digest = Hex(result.data(), static_cast<DWORD>(result.size()));
			success = true;
		cleanup:
			if (hash != nullptr)
				BCryptDestroyHash(hash);
			if (algorithm != nullptr)
				BCryptCloseAlgorithmProvider(algorithm, 0);
			return success;
		}

		bool VerifyAuthenticodeAndGetSignerHash(const std::filesystem::path& file, std::string& signerCertificateSha256)
		{
			WINTRUST_FILE_INFO fileInfo{};
			fileInfo.cbStruct = sizeof(fileInfo);
			fileInfo.pcwszFilePath = file.c_str();
			WINTRUST_DATA trustData{};
			trustData.cbStruct = sizeof(trustData);
			trustData.dwUIChoice = WTD_UI_NONE;
			trustData.fdwRevocationChecks = WTD_REVOKE_NONE;
			trustData.dwUnionChoice = WTD_CHOICE_FILE;
			trustData.pFile = &fileInfo;
			trustData.dwStateAction = WTD_STATEACTION_VERIFY;
			trustData.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
			GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
			const LONG trustStatus = ::WinVerifyTrust(nullptr, &policy, &trustData);
			trustData.dwStateAction = WTD_STATEACTION_CLOSE;
			::WinVerifyTrust(nullptr, &policy, &trustData);
			if (trustStatus != ERROR_SUCCESS)
				return false;
			HCERTSTORE store = nullptr;
			HCRYPTMSG message = nullptr;
			DWORD encoding = 0, contentType = 0, formatType = 0;
			if (!::CryptQueryObject(CERT_QUERY_OBJECT_FILE, file.c_str(), CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
					CERT_QUERY_FORMAT_FLAG_BINARY, 0, &encoding, &contentType, &formatType, &store, &message, nullptr))
				return false;
			DWORD signerSize = 0;
			bool success = false;
			if (::CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &signerSize) && signerSize > 0)
			{
				std::vector<BYTE> signerBuffer(signerSize);
				if (::CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, signerBuffer.data(), &signerSize))
				{
					const auto signer = reinterpret_cast<PCMSG_SIGNER_INFO>(signerBuffer.data());
					CERT_INFO certificateInfo{};
					certificateInfo.Issuer = signer->Issuer;
					certificateInfo.SerialNumber = signer->SerialNumber;
					CertContextHandle certificate(::CertFindCertificateInStore(store,
						X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SUBJECT_CERT, &certificateInfo, nullptr));
					if (certificate)
						success = Sha256Bytes(certificate.get()->pbCertEncoded, certificate.get()->cbCertEncoded,
							signerCertificateSha256);
				}
			}
			if (message != nullptr)
				::CryptMsgClose(message);
			if (store != nullptr)
				::CertCloseStore(store, 0);
			return success;
		}
	} // namespace

	bool Sha256File(const std::filesystem::path& path, std::string& digest)
	{
		FileHandle file(::CreateFileW(
			path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
		if (!file)
			return false;
		BCRYPT_ALG_HANDLE algorithm = nullptr;
		BCRYPT_HASH_HANDLE hash = nullptr;
		std::vector<BYTE> hashObject;
		std::array<BYTE, 32> result{};
		std::array<BYTE, 128 * 1024> buffer{};
		DWORD objectLength = 0, resultLength = 0;
		bool success = false;
		if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
			goto cleanup;
		if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength),
				sizeof(objectLength), &resultLength, 0) < 0 ||
			objectLength == 0)
			goto cleanup;
		hashObject.resize(objectLength);
		if (BCryptCreateHash(algorithm, &hash, hashObject.data(), objectLength, nullptr, 0, 0) < 0)
			goto cleanup;
		for (;;)
		{
			DWORD read = 0;
			if (!::ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
				goto cleanup;
			if (read == 0)
				break;
			if (BCryptHashData(hash, buffer.data(), read, 0) < 0)
				goto cleanup;
		}
		if (BCryptFinishHash(hash, result.data(), static_cast<ULONG>(result.size()), 0) < 0)
			goto cleanup;
		digest = Hex(result.data(), static_cast<DWORD>(result.size()));
		success = true;
	cleanup:
		if (hash != nullptr)
			BCryptDestroyHash(hash);
		if (algorithm != nullptr)
			BCryptCloseAlgorithmProvider(algorithm, 0);
		return success;
	}

	std::string ResolveTrustedSignerHash(const StartupOptions& options)
	{
		std::string configured = ToLowerAscii(VSMR_UPDATE_SIGNER_CERT_SHA256);
		configured.erase(std::remove_if(configured.begin(), configured.end(),
							 [](unsigned char character) { return std::isspace(character) != 0; }),
			configured.end());
		if (IsHex(configured, 64))
			return configured;
		std::string signer;
		return VerifyAuthenticodeAndGetSignerHash(options.loaderPath, signer) && IsHex(signer, 64)
				   ? ToLowerAscii(signer)
				   : std::string();
	}

	bool VerifyDetachedCms(const std::vector<std::uint8_t>& content, const std::vector<std::uint8_t>& signature,
		const std::string& expectedSignerSha256, std::string& error)
	{
		if (!IsHex(expectedSignerSha256, 64))
		{
			error = "signature_required";
			return false;
		}
		CRYPT_VERIFY_MESSAGE_PARA parameters{};
		parameters.cbSize = sizeof(parameters);
		parameters.dwMsgAndCertEncodingType = X509_ASN_ENCODING | PKCS_7_ASN_ENCODING;
		const BYTE* contents[] = {content.data()};
		DWORD contentSizes[] = {static_cast<DWORD>(content.size())};
		PCCERT_CONTEXT signerRaw = nullptr;
		if (!::CryptVerifyDetachedMessageSignature(&parameters, 0, signature.data(),
				static_cast<DWORD>(signature.size()), 1, contents, contentSizes, &signerRaw))
		{
			error = "manifest_signature_invalid";
			return false;
		}
		CertContextHandle signer(signerRaw);
		std::string actualSigner;
		if (!Sha256Bytes(signer.get()->pbCertEncoded, signer.get()->cbCertEncoded, actualSigner) ||
			ToLowerAscii(actualSigner) != ToLowerAscii(expectedSignerSha256))
		{
			error = "manifest_signer_mismatch";
			return false;
		}
		return true;
	}
} // namespace vsmr::updater::verification
