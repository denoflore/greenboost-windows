/* SPDX-License-Identifier: GPL-2.0-only */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
int main(int argc, char **argv)
{
    STARTUPINFOA sa = { sizeof(sa) };
    STARTUPINFOW sw = { sizeof(sw) };
    PROCESS_INFORMATION child = { 0 };
    DWORD code = 1; BOOL ok;
    wchar_t wide[32768];
    if (argc != 3) { fprintf(stderr, "Usage: test_child.exe A|W \"child command line\"\n"); return 2; }
    if (argv[1][0] == 'A')
        ok = CreateProcessA(NULL, argv[2], NULL, NULL, TRUE, 0, NULL, NULL, &sa, &child);
    else {
        if (!MultiByteToWideChar(CP_ACP, 0, argv[2], -1, wide, 32768)) return 2;
        ok = CreateProcessW(NULL, wide, NULL, NULL, TRUE, 0, NULL, NULL, &sw, &child);
    }
    if (!ok) { fprintf(stderr, "CreateProcess: Windows %lu\n", GetLastError()); return 1; }
    printf("parent_pid=%lu child_pid=%lu api=CreateProcess%s\n", GetCurrentProcessId(), child.dwProcessId, argv[1]);
    fflush(stdout);
    CloseHandle(child.hThread);
    if (WaitForSingleObject(child.hProcess, 60000) == WAIT_OBJECT_0)
        GetExitCodeProcess(child.hProcess, &code);
    else { TerminateProcess(child.hProcess, 1); WaitForSingleObject(child.hProcess, INFINITE); }
    CloseHandle(child.hProcess);
    return (int)code;
}
