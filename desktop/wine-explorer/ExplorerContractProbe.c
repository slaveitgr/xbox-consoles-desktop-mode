/* SPDX-License-Identifier: MIT
 * Self-authored PE32+ guest qualification for the COM/view contract used by
 * Wine Explorer. This is a diagnostic, not Wine Explorer or a replacement UI.
 * It enumerates at most 64 shell entries and creates no files except its report.
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <exdisp.h>

static char report[16384];
static DWORD used;
static WCHAR display[16384];
static WNDCLASSW window_class;

static void append(const char *text)
{
    while (*text && used + 1 < sizeof(report)) report[used++] = *text++;
    report[used] = 0;
}

static void hex(DWORD value)
{
    static const char digits[] = "0123456789abcdef";
    int shift;
    append("0x");
    for (shift = 28; shift >= 0 && used + 1 < sizeof(report); shift -= 4)
        report[used++] = digits[(value >> shift) & 15];
    report[used] = 0;
}

static void record(const char *stage, HRESULT result)
{
    append(stage); append(": "); hex((DWORD)result); append("\r\n");
}

static void optional_class(const char *name, REFCLSID class_id, REFIID interface_id, DWORD context)
{
    IUnknown *object = NULL;
    HRESULT status = CoCreateInstance(class_id, NULL, context, interface_id, (void **)&object);
    record(name, status);
    if (object) IUnknown_Release(object);
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcW(window, message, wparam, lparam);
}

static void pump(void)
{
    const DWORD start = GetTickCount();
    MSG message;
    while ((DWORD)(GetTickCount() - start) < 750)
    {
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT) return;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(5);
    }
}

/* Deliberate no-CRT entry point: a small x64 guest can test the shell contract
 * without adding the Wine/UCRT startup dependencies to this experiment. */
void WINAPI ExplorerProbeEntry(void)
{
    HRESULT ole_status, browser_status, desktop_status, view_status = E_FAIL;
    IExplorerBrowser *browser = NULL;
    IShellFolder *desktop = NULL;
    IShellFolder2 *folder2 = NULL;
    IPersistFolder2 *persist = NULL;
    IShellView *view = NULL;
    IFolderView *folder_view = NULL;
    IEnumIDList *enumerator = NULL;
    HWND parent = NULL;
    RECT bounds = {0, 0, 800, 540};
    FOLDERSETTINGS settings = {FVM_DETAILS, FWF_AUTOARRANGE | FWF_FULLROWSELECT};
    DWORD index, written;
    HANDLE file;
    BOOL initialized = FALSE;
    BOOL browsed = FALSE;
    BOOL passed = FALSE;

    append("Wine Explorer COM/view qualification v1\r\n");
    append("This probe is not Wine Explorer. HRESULTs are actual runtime results.\r\n\r\n");
    ole_status = OleInitialize(NULL);
    record("OleInitialize", ole_status);
    if (SUCCEEDED(ole_status))
    {
        browser_status = CoCreateInstance(&CLSID_ExplorerBrowser, NULL, CLSCTX_INPROC_SERVER,
                                          &IID_IExplorerBrowser, (void **)&browser);
        record("CLSID_ExplorerBrowser / IExplorerBrowser [required]", browser_status);
        optional_class("CLSID_ShellWindows / IShellWindows [optional window registry]",
                       &CLSID_ShellWindows, &IID_IShellWindows, CLSCTX_LOCAL_SERVER);
        optional_class("CLSID_NamespaceTreeControl / INameSpaceTreeControl2 [optional framed pane]",
                       &CLSID_NamespaceTreeControl, &IID_INameSpaceTreeControl2, CLSCTX_INPROC_SERVER);
        optional_class("CLSID_FileOperation / IFileOperation [future operations]",
                       &CLSID_FileOperation, &IID_IFileOperation, CLSCTX_INPROC_SERVER);

        window_class.lpfnWndProc = window_proc;
        window_class.hInstance = GetModuleHandleW(NULL);
        window_class.lpszClassName = L"MihawkExplorerContractProbe";
        if (RegisterClassW(&window_class))
            parent = CreateWindowExW(0, window_class.lpszClassName,
                L"Wine Explorer view qualification", WS_OVERLAPPEDWINDOW,
                60, 60, 820, 580, NULL, NULL, window_class.hInstance, NULL);
        if (parent) record("CreateWindowExW guest parent", S_OK);
        else
        {
            DWORD error = GetLastError();
            record("CreateWindowExW guest parent", error ? HRESULT_FROM_WIN32(error) : E_FAIL);
        }

        desktop_status = SHGetDesktopFolder(&desktop);
        record("SHGetDesktopFolder / IShellFolder [required]", desktop_status);
        if (desktop)
        {
            record("IShellFolder.QueryInterface / IShellFolder2",
                   IShellFolder_QueryInterface(desktop, &IID_IShellFolder2, (void **)&folder2));
            if (folder2) IShellFolder2_Release(folder2);
            record("IShellFolder.QueryInterface / IPersistFolder2 [Mihawk folder identity]",
                   IShellFolder_QueryInterface(desktop, &IID_IPersistFolder2, (void **)&persist));
            if (persist)
            {
                PIDLIST_ABSOLUTE folder_id = NULL;
                record("IPersistFolder2.GetCurFolder", IPersistFolder2_GetCurFolder(persist, &folder_id));
                if (folder_id) CoTaskMemFree(folder_id);
                IPersistFolder2_Release(persist); persist = NULL;
            }
            record("IShellFolder.EnumObjects",
                   IShellFolder_EnumObjects(desktop, parent, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS,
                                            &enumerator));
            if (enumerator)
            {
                LPITEMIDLIST item;
                ULONG fetched;
                HRESULT next = S_FALSE;
                for (index = 0; index < 64; ++index)
                {
                    item = NULL; fetched = 0;
                    next = IEnumIDList_Next(enumerator, 1, &item, &fetched);
                    if (item) CoTaskMemFree(item);
                    if (next != S_OK || fetched != 1) break;
                }
                record("IEnumIDList.Next final HRESULT (64-item bound)", next);
                append("IEnumIDList returned entries (hex): "); hex(index); append("\r\n");
                IEnumIDList_Release(enumerator);
            }
            record("IShellFolder.CreateViewObject / IShellView [required]",
                   IShellFolder_CreateViewObject(desktop, parent, &IID_IShellView, (void **)&view));
            if (view) { IShellView_Release(view); view = NULL; }
        }
        if (browser && parent)
        {
            IUnknown *service = NULL;
            record("IExplorerBrowser.QueryInterface / IShellBrowser [Mihawk navigation]",
                   IExplorerBrowser_QueryInterface(browser, &IID_IShellBrowser, (void **)&service));
            if (service) { IUnknown_Release(service); service = NULL; }
            record("IExplorerBrowser.QueryInterface / IObjectWithSite [Mihawk selection/filter]",
                   IExplorerBrowser_QueryInterface(browser, &IID_IObjectWithSite, (void **)&service));
            if (service) IUnknown_Release(service);
            record("IExplorerBrowser.SetOptions EBO_NOBORDER",
                   IExplorerBrowser_SetOptions(browser, EBO_NOBORDER));
            view_status = IExplorerBrowser_Initialize(browser, parent, &bounds, &settings);
            record("IExplorerBrowser.Initialize [required]", view_status);
            initialized = SUCCEEDED(view_status);
            if (initialized && desktop)
            {
                view_status = IExplorerBrowser_BrowseToObject(browser, (IUnknown *)desktop, SBSP_ABSOLUTE);
                record("IExplorerBrowser.BrowseToObject [required]", view_status);
                browsed = SUCCEEDED(view_status);
                ShowWindow(parent, SW_SHOW);
                pump();
                view_status = IExplorerBrowser_GetCurrentView(browser, &IID_IShellView, (void **)&view);
                record("IExplorerBrowser.GetCurrentView / IShellView [required]", view_status);
                if (view)
                {
                    HWND view_window = NULL;
                    HRESULT window_status = IShellView_GetWindow(view, &view_window);
                    record("IShellView.GetWindow", window_status);
                    record("IShellView.QueryInterface / IFolderView",
                           IShellView_QueryInterface(view, &IID_IFolderView, (void **)&folder_view));
                    passed = browsed && SUCCEEDED(window_status) && view_window != NULL && folder_view != NULL;
                    if (folder_view)
                    {
                        int count = 0;
                        record("IFolderView.ItemCount SVGIO_ALLVIEW", IFolderView_ItemCount(folder_view, SVGIO_ALLVIEW, &count));
                        append("View item count (hex): "); hex((DWORD)count); append("\r\n");
                        record("IFolderView.ItemCount SVGIO_SELECTION", IFolderView_ItemCount(folder_view, SVGIO_SELECTION, &count));
                        record("IFolderView.GetFolder / IPersistFolder2",
                               IFolderView_GetFolder(folder_view, &IID_IPersistFolder2, (void **)&persist));
                        if (persist) { IPersistFolder2_Release(persist); persist = NULL; }
                        IFolderView_Release(folder_view);
                    }
                    IShellView_Release(view);
                }
            }
        }
        if (initialized) IExplorerBrowser_Destroy(browser);
        if (browser) IExplorerBrowser_Release(browser);
        if (desktop) IShellFolder_Release(desktop);
        if (parent) DestroyWindow(parent);
        UnregisterClassW(window_class.lpszClassName, window_class.hInstance);
        OleUninitialize();
    }
    append(passed ? "\r\nVIEW CONTRACT PASSED. Wine Explorer itself remains untested.\r\n"
                  : "\r\nVIEW CONTRACT NOT AVAILABLE. No Wine Explorer success is claimed.\r\n");
    file = CreateFileW(L"C:\\explorer-contract-report.txt", GENERIC_WRITE, 0, NULL,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE)
    {
        BOOL saved = WriteFile(file, report, used, &written, NULL);
        CloseHandle(file);
        append(saved && written == used ? "\r\nReport saved to C:\\explorer-contract-report.txt."
                                       : "\r\nReport write failed or was incomplete.");
    }
    else append("\r\nReport could not be saved; the results remain visible here.");
    for (index = 0; index < used && index + 1 < sizeof(display) / sizeof(display[0]); ++index)
        display[index] = (WCHAR)(unsigned char)report[index];
    display[index] = 0;
    MessageBoxW(NULL, display, L"Wine Explorer compatibility results", MB_OK);
    ExitProcess(passed ? 0 : 1);
}
