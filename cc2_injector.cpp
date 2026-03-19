#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>
#include <filesystem>
#include <iostream>

static std::wstring Quote(const std::wstring& s) {
    return L"\"" + s + L"\"";
}

static std::filesystem::path GetModuleDir() {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::filesystem::path p = std::filesystem::absolute(exePath);
    return p.parent_path();
}

static bool InjectDll(HANDLE process, const std::wstring& dllPath) {
    const size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remoteMem = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) return false;

    if (!WriteProcessMemory(process, remoteMem, dllPath.c_str(), bytes, nullptr)) {
        VirtualFreeEx(process, remoteMem, 0, MEM_RELEASE);
        return false;
    }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (!k32) {
        VirtualFreeEx(process, remoteMem, 0, MEM_RELEASE);
        return false;
    }

    auto* loadLibraryW = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(k32, "LoadLibraryW"));
    if (!loadLibraryW) {
        VirtualFreeEx(process, remoteMem, 0, MEM_RELEASE);
        return false;
    }

    HANDLE remoteThread = CreateRemoteThread(process, nullptr, 0, loadLibraryW, remoteMem, 0, nullptr);
    if (!remoteThread) {
        VirtualFreeEx(process, remoteMem, 0, MEM_RELEASE);
        return false;
    }

    WaitForSingleObject(remoteThread, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeThread(remoteThread, &exitCode);
    CloseHandle(remoteThread);
    VirtualFreeEx(process, remoteMem, 0, MEM_RELEASE);
    return exitCode != 0;
}

int wmain(int argc, wchar_t** argv) {
    std::filesystem::path baseDir = GetModuleDir();
    std::filesystem::path gameExe;
    std::filesystem::path hookDll;

    if (argc >= 2) {
        gameExe = std::filesystem::absolute(argv[1]);
    } else {
        gameExe = baseDir / L"carrier_command_vr.exe";
    }

    if (argc >= 3) {
        hookDll = std::filesystem::absolute(argv[2]);
    } else {
        hookDll = baseDir / L"cc2_recenter_hook_dll.dll";
    }

    if (!std::filesystem::exists(gameExe)) {
        std::wcerr << L"Game EXE not found: " << gameExe.wstring() << L"\n";
        std::wcerr << L"Put this EXE next to carrier_command_vr.exe or pass the game path explicitly.\n";
        return 2;
    }
    if (!std::filesystem::exists(hookDll)) {
        std::wcerr << L"Hook DLL not found: " << hookDll.wstring() << L"\n";
        std::wcerr << L"Expected cc2_recenter_hook_dll.dll next to this EXE unless you pass a path explicitly.\n";
        return 3;
    }

    std::wstring cmdLine = Quote(gameExe.wstring());
    for (int i = 3; i < argc; ++i) {
        cmdLine += L" ";
        cmdLine += Quote(argv[i]);
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    std::wstring workingDir = gameExe.parent_path().wstring();
    std::vector<wchar_t> mutableCmd(cmdLine.begin(), cmdLine.end());
    mutableCmd.push_back(L'\0');

    BOOL ok = CreateProcessW(
        gameExe.c_str(),
        mutableCmd.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_SUSPENDED,
        nullptr,
        workingDir.c_str(),
        &si,
        &pi);

    if (!ok) {
        std::wcerr << L"CreateProcessW failed: " << GetLastError() << L"\n";
        return 4;
    }

    if (!InjectDll(pi.hProcess, hookDll.wstring())) {
        std::wcerr << L"DLL injection failed.\n";
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 5;
    }

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    std::wcout << L"Started and injected: " << gameExe.wstring() << L"\n";
    return 0;
}
