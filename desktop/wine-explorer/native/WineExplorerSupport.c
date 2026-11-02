/* SPDX-License-Identifier: MIT */
#define MIHAWK_WINE_SUPPORT_IMPLEMENTATION
#include "WineExplorerSupport.h"

typedef char WindowsWideCharactersMustBe16Bits[(sizeof(WCHAR) == sizeof(uint16_t)) ? 1 : -1];
static char native_report[8192];
static WCHAR startup_message[8192];
static DWORD report_used;
static BOOL startup_failed;

static void append(const char *text)
{
    while (*text && report_used + 1 < sizeof(native_report)) native_report[report_used++] = *text++;
    native_report[report_used] = 0;
}

static void append_hex(DWORD value)
{
    const char *digits = "0123456789abcdef";
    int shift;
    append("0x");
    for (shift = 28; shift >= 0 && report_used + 1 < sizeof(native_report); shift -= 4)
        native_report[report_used++] = digits[(value >> shift) & 15];
    native_report[report_used] = 0;
}

static void save_report(void)
{
    HANDLE file = CreateFileW(L"C:\\wine-explorer-native-report.txt", GENERIC_WRITE, 0,
        NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE)
    {
        DWORD written;
        WriteFile(file, native_report, report_used, &written, NULL);
        CloseHandle(file);
    }
}

void MihawkWineStage(const char *stage, HRESULT status)
{
    append(stage); append(": "); append_hex((DWORD)status); append("\r\n");
    save_report();
}

void MihawkWineStartupFailure(const char *stage, HRESULT status)
{
    DWORD index;
    startup_failed = TRUE;
    MihawkWineStage(stage, status);
    append("Wine Explorer startup stopped at this real failure.\r\n");
    append("This build does not supply the missing Wine shell provider or desktop session.\r\n");
    save_report();
    for (index = 0; index < report_used && index + 1 < ARRAY_SIZE(startup_message); ++index)
        startup_message[index] = (WCHAR)(unsigned char)native_report[index];
    startup_message[index] = 0;
    MessageBoxW(NULL, startup_message, L"Native Wine Explorer startup", MB_OK | MB_ICONERROR);
}

void *MihawkWineMalloc(size_t size)
{
    return HeapAlloc(GetProcessHeap(), 0, size ? size : 1);
}

void *MihawkWineCalloc(size_t count, size_t size)
{
    size_t bytes;
    if (!MwCheckedProduct(count, size, &bytes))
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes ? bytes : 1);
}

void MihawkWineFree(void *memory)
{
    if (memory) HeapFree(GetProcessHeap(), 0, memory);
}

size_t MihawkWineWideLength(const wchar_t *text)
{
    return MwWideLength((const uint16_t *)text);
}

int MihawkWineWideCompare(const wchar_t *left, const wchar_t *right, size_t count)
{
    return MwWideCompareOption((const uint16_t *)left, (const uint16_t *)right, count);
}

/* The first source unit is the file-manager window, not Wine's desktop/session
 * manager. Refuse /desktop explicitly; no replacement desktop is synthesized. */
void manage_desktop(WCHAR *argument)
{
    (void)argument;
    MihawkWineStartupFailure("Wine desktop/session manager not linked", E_NOTIMPL);
    ExitProcess(ERROR_NOT_SUPPORTED);
}

extern int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, LPWSTR arguments, int show);

void WINAPI MihawkWineExplorerEntry(void)
{
    LPWSTR command = GetCommandLineW();
    size_t arguments;
    int result;
    append("Actual Wine Explorer Windows-side source, native x64 guest build.\r\n");
    append("Wine pin: 1c78596625336a73dbfc035dbbcf0dbdd95b06eb\r\n");
    append("This file-window milestone does not include the Wine desktop/session or shell provider.\r\n\r\n");
    MihawkWineStage("Native PE64 entry reached", S_OK);
    if (!MwCommandArguments((const uint16_t *)command, 32768, MAX_PATH - 1, &arguments))
    {
        MihawkWineStartupFailure("Command line exceeds the bounded source adapter",
                                HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE));
        ExitProcess(ERROR_FILENAME_EXCED_RANGE);
    }
    result = wWinMain(GetModuleHandleW(NULL), NULL, command + arguments, SW_SHOWDEFAULT);
    if (!startup_failed) MihawkWineStage("Wine Explorer source entry returned", S_OK);
    ExitProcess(startup_failed ? 2 : (UINT)result);
}
