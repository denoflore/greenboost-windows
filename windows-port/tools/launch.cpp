// SPDX-License-Identifier: GPL-2.0-only
// Launch a CUDA application with the adjacent GreenBoost shim.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <detours.h>
#include <stdio.h>
#include <string>

// Windows argument escaping, including quotes and trailing backslashes.
static std::wstring quote(const wchar_t *argument)
{
    std::wstring result = L"\"";
    unsigned slashes = 0;
    for (const wchar_t *p = argument; *p; ++p) {
        if (*p == L'\\') { ++slashes; continue; }
        result.append(*p == L'\"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        result += *p;
    }
    result.append(slashes * 2, L'\\');
    return result + L'\"';
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2 || wcscmp(argv[1], L"--help") == 0) {
        fwprintf(argc < 2 ? stderr : stdout,
            L"Usage: greenboost.exe application.exe [arguments]\n"
            L"Runs the application with greenboost_cuda.dll from this folder.\n"
            L"Set GREENBOOST_DEBUG=1 for allocation diagnostics.\n");
        return argc < 2 ? 2 : 0;
    }
    char dll[MAX_PATH];
    DWORD length = GetModuleFileNameA(NULL, dll, sizeof(dll));
    if (!length || length >= sizeof(dll)) {
        fprintf(stderr, "Cannot resolve launcher path (Windows error %lu).\n", GetLastError());
        return 2;
    }
    char *slash = strrchr(dll, '\\');
    if (!slash || (size_t)(slash - dll) + sizeof("\\greenboost_cuda.dll") > sizeof(dll))
        return 2;
    strcpy_s(slash + 1, sizeof(dll) - (slash + 1 - dll), "greenboost_cuda.dll");
    if (GetFileAttributesA(dll) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "Missing adjacent shim: %s\n", dll);
        return 2;
    }
    std::wstring command;
    for (int i = 1; i < argc; ++i) {
        if (i > 1) command += L' ';
        command += quote(argv[i]);
    }
    STARTUPINFOW startup = { sizeof(startup) };
    PROCESS_INFORMATION process = {};
    if (!DetourCreateProcessWithDllExW(NULL, &command[0], NULL, NULL, TRUE,
            0, NULL, NULL, &startup, &process, dll, NULL)) {
        fprintf(stderr, "Cannot launch/inject application (Windows error %lu).\n", GetLastError());
        return 1;
    }
    CloseHandle(process.hThread);
    const char *debug = getenv("GREENBOOST_DEBUG");
    if (debug && debug[0] == '1')
        fprintf(stderr, "[GreenBoost] launched pid=%lu\n", process.dwProcessId);
    DWORD code = 1;
    if (WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0)
        GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return (int)code;
}
