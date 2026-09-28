#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include "updater/FileUpdater.hpp"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int count = 0;
    auto arguments = ::CommandLineToArgvW(::GetCommandLineW(), &count);
    if (arguments == nullptr || count != 3)
    {
        if (arguments != nullptr) ::LocalFree(arguments);
        return 1;
    }
    const std::filesystem::path root(arguments[1]);
    wchar_t* end = nullptr;
    const unsigned long pid = ::wcstoul(arguments[2], &end, 10);
    const bool valid = pid != 0 && end != arguments[2] && *end == L'\0';
    ::LocalFree(arguments);
    if (!valid) return 1;
    try { return vsmr::updater::files::ApplyPending(root, static_cast<std::uint32_t>(pid)); }
    catch (...) { return 10; } // Durable journal is recovered on the next launch.
}
