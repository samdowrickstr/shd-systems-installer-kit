// SPDX-License-Identifier: AGPL-3.0-or-later
// SPDX-FileCopyrightText: (c) 2026 SHD Systems Ltd

// Standalone single-exe bootstrapper for the Qt installer kit.
//
// A Qt app can't be a single self-contained exe (it needs its Qt DLLs), so this
// tiny native Win32 stub embeds the whole installer bundle (Qt installer GUI +
// runtime + app payload) as a compressed resource. At runtime it unpacks the
// bundle to a temp folder, runs the real Qt installer (AppSetup.exe), waits for
// it to finish, then cleans up. The result is ONE portable .exe with no external
// dependencies.
//
// No Qt here — links only against the Win32 API, so the stub itself is tiny and
// fully self-contained. The window title used for error dialogs is baked in by
// the packer via -DSETUP_TITLE="...".

#include <windows.h>
#include <string>

// pack.ps1 writes this next to the build, baking in the product title + setup
// exe name. Falls back to generic defaults if it isn't present.
#if defined(__has_include)
#  if __has_include("bootstrap_gen.h")
#    include "bootstrap_gen.h"
#  endif
#endif

#define IDR_PAYLOAD 101

#ifndef SETUP_TITLE
#define SETUP_TITLE L"Setup"
#endif

// The name of the real Qt installer exe inside the payload.
#ifndef SETUP_EXE
#define SETUP_EXE L"AppSetup.exe"
#endif

static std::wstring makeTempDir()
{
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"AppSetup_" + std::to_wstring(GetTickCount64());
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

static bool writeResource(const std::wstring &path)
{
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_PAYLOAD), RT_RCDATA);
    if (!res) {
        return false;
    }
    HGLOBAL handle = LoadResource(nullptr, res);
    const DWORD size = SizeofResource(nullptr, res);
    void *data = LockResource(handle);
    if (!handle || !data || size == 0) {
        return false;
    }
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(file, data, size, &written, nullptr) && written == size;
    CloseHandle(file);
    return ok;
}

static DWORD runAndWait(std::wstring cmdLine, const std::wstring &cwd, DWORD flags)
{
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, &cmdLine[0], nullptr, nullptr, FALSE, flags, nullptr,
                        cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
        return static_cast<DWORD>(-1);
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return exitCode;
}

static void removeDir(const std::wstring &dir)
{
    runAndWait(L"cmd.exe /c rmdir /s /q \"" + dir + L"\"", std::wstring(), CREATE_NO_WINDOW);
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    const std::wstring dir = makeTempDir();
    if (dir.empty()) {
        MessageBoxW(nullptr, L"Could not create a temporary folder.", SETUP_TITLE, MB_ICONERROR);
        return 1;
    }

    const std::wstring zip = dir + L"\\payload.tgz";
    if (!writeResource(zip)) {
        MessageBoxW(nullptr, L"Could not unpack the installer.", SETUP_TITLE, MB_ICONERROR);
        removeDir(dir);
        return 1;
    }

    // Windows 10+ ships bsdtar as tar.exe, which extracts our tar.gz preserving
    // folders. bsdtar returns a non-zero code on the harmless "./" root entry,
    // so judge success by the result (the installer exe) rather than the code.
    const std::wstring untar = L"tar.exe -xf \"" + zip + L"\" -C \"" + dir + L"\"";
    runAndWait(untar, dir, CREATE_NO_WINDOW);

    const std::wstring setup = dir + L"\\" + std::wstring(SETUP_EXE);
    if (GetFileAttributesW(setup.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(nullptr, L"Could not extract the installer.", SETUP_TITLE, MB_ICONERROR);
        removeDir(dir);
        return 1;
    }
    DeleteFileW(zip.c_str());

    // Hand off to the real (Qt) installer and wait for the user to finish.
    runAndWait(L"\"" + setup + L"\"", dir, 0);

    removeDir(dir);
    return 0;
}
