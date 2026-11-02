#!/usr/bin/env python3
"""Prepare a reviewable, minimal adaptation of the actual pinned Wine Explorer.

SPDX-License-Identifier: MIT
The generated Explorer remains LGPL-2.1-or-later Wine-derived source.
"""
from __future__ import annotations
import argparse
import difflib
import hashlib
import json
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
SOURCE_ROOT = HERE.parent / "staged/wine"
sys.path.insert(0, str(HERE.parent))
from prepare_sources import validate  # noqa: E402


def adapt(original: str) -> str:
    source = original
    def replace(old: str, new: str, count: int = 1) -> None:
        nonlocal source
        actual = source.count(old)
        if actual != count:
            raise ValueError(f"Pinned Wine source anchor changed: expected {count}, found {actual}: {old[:70]!r}")
        source = source.replace(old, new)

    replace("    IShellWindows *sw;\n    ITEMIDLIST *pidl;",
            "    IShellWindows *sw = NULL;\n    ITEMIDLIST *pidl;")
    replace("""static HRESULT WINAPI IExplorerBrowserEventsImpl_fnQueryInterface(IExplorerBrowserEvents *iface, REFIID riid, void **ppvObject)
{
    return E_NOINTERFACE;
}""", """static HRESULT WINAPI IExplorerBrowserEventsImpl_fnQueryInterface(IExplorerBrowserEvents *iface, REFIID riid, void **ppvObject)
{
    if (!ppvObject) return E_POINTER;
    *ppvObject = NULL;
    if (!IsEqualIID(riid, &IID_IUnknown) && !IsEqualIID(riid, &IID_IExplorerBrowserEvents)) return E_NOINTERFACE;
    *ppvObject = iface;
    IExplorerBrowserEvents_AddRef(iface);
    return S_OK;
}""")
    replace("        IShellWindows_Release(sw);", "        if (sw) IShellWindows_Release(sw);", 4)
    replace('        ERR( "Could not obtain an instance of IExplorerBrowser\\n" );',
            '        MihawkWineStartupFailure("CLSID_ExplorerBrowser / IID_IExplorerBrowser", hres);')
    replace('        ERR( "Could not allocate an explorer_info struct\\n" );',
            '        MihawkWineStartupFailure("Wine explorer_info allocation", E_OUTOFMEMORY);')
    replace("    ret->IExplorerBrowserEvents_iface.lpVtbl = &vt_IExplorerBrowserEvents;",
            "    if (!ret) return NULL;\n    ret->IExplorerBrowserEvents_iface.lpVtbl = &vt_IExplorerBrowserEvents;")
    replace("                        default_height,NULL,NULL,explorer_hInstance,NULL);",
            """                        default_height,NULL,NULL,explorer_hInstance,NULL);
    if (!info->main_window)
    {
        DWORD error = GetLastError();
        MihawkWineStartupFailure("Wine Explorer CreateWindowW", error ? HRESULT_FROM_WIN32(error) : E_FAIL);
        IExplorerBrowser_Release(info->browser);
        if (sw) IShellWindows_Release(sw);
        free(path);
        free(info);
        return;
    }
    MihawkWineStage("Actual Wine Explorer window created", S_OK);""")
    replace("    IExplorerBrowser_Initialize(info->browser,info->main_window,&rect,&fs);",
            """    hres = IExplorerBrowser_Initialize(info->browser,info->main_window,&rect,&fs);
    MihawkWineStage("IExplorerBrowser.Initialize", hres);
    if (FAILED(hres))
    {
        MihawkWineStartupFailure("IExplorerBrowser.Initialize", hres);
        IExplorerBrowser_Release(info->browser);
        DestroyWindow(info->main_window);
        if (sw) IShellWindows_Release(sw);
        free(path);
        free(info);
        return;
    }""")
    replace("    info->rebar_height=0;\n    info->main_window",
            '    MihawkWineStage("Actual Wine ExplorerBrowser acquired", S_OK);\n    info->rebar_height=0;\n    info->main_window')
    replace("    if(info)\n        browser = info->browser;\n    switch(uMsg)",
            "    if (!info) return DefWindowProcW(hwnd, uMsg, wParam, lParam);\n    browser = info->browser;\n    switch(uMsg)")
    replace("        IImageList_Release(info->icon_list);",
            "        if (info->icon_list) IImageList_Release(info->icon_list);")
    replace("    events = make_explorer_events(info);\n    IExplorerBrowser_Advise(info->browser,events,&info->advise_cookie);",
            """    events = make_explorer_events(info);
    if (!events)
    {
        MihawkWineStartupFailure("Wine Explorer event allocation", E_OUTOFMEMORY);
        DestroyWindow(info->main_window);
        free(path);
        return;
    }
    hres = IExplorerBrowser_Advise(info->browser,events,&info->advise_cookie);
    if (FAILED(hres))
    {
        MihawkWineStartupFailure("Wine Explorer browser Advise", hres);
        IExplorerBrowserEvents_Release(events);
        DestroyWindow(info->main_window);
        free(path);
        return;
    }""")
    replace("    SHGetDesktopFolder(&desktop);\n\n    if (!path)",
            "    if (FAILED(SHGetDesktopFolder(&desktop)) || !desktop) return NULL;\n\n    if (!path)")
    replace("    folder = get_starting_shell_folder(path);\n    IExplorerBrowser_BrowseToObject(info->browser, (IUnknown *)folder, SBSP_ABSOLUTE);\n    IShellFolder_Release(folder);",
            """    folder = get_starting_shell_folder(path);
    if (!folder)
    {
        MihawkWineStartupFailure("Wine Explorer starting shell folder", E_NOINTERFACE);
        IExplorerBrowserEvents_Release(events);
        DestroyWindow(info->main_window);
        free(path);
        return;
    }
    hres = IExplorerBrowser_BrowseToObject(info->browser, (IUnknown *)folder, SBSP_ABSOLUTE);
    IShellFolder_Release(folder);
    MihawkWineStage("Wine Explorer initial BrowseToObject", hres);
    if (FAILED(hres))
    {
        MihawkWineStartupFailure("Wine Explorer initial BrowseToObject", hres);
        IExplorerBrowserEvents_Release(events);
        DestroyWindow(info->main_window);
        free(path);
        return;
    }""")
    replace("            *((WORD*)path)=MAX_PATH;\n            SendMessageW(edit_ctrl,EM_GETLINE,0,(LPARAM)path);",
            "            if (GetWindowTextW(edit_ctrl, path, ARRAY_SIZE(path)) <= 0) return 0;")
    replace("    ptr = cds->lpData;\n    memcpy(&count, ptr, sizeof(count));",
            "    if (!cds || !MwValidateSelectionPayload(cds->lpData, cds->cbData)) return FALSE;\n    ptr = cds->lpData;\n    memcpy(&count, ptr, sizeof(count));")
    replace("    if (cds->dwData != magic)", "    if (!cds || cds->dwData != magic)")
    replace("    IExplorerBrowser_GetCurrentView(info->browser, &IID_IShellView, (void **)&sv);",
            "    if (FAILED(IExplorerBrowser_GetCurrentView(info->browser, &IID_IShellView, (void **)&sv))) return FALSE;")
    replace('        ERR( "Could not initialize COM\\n" );',
            '        MihawkWineStartupFailure("OleInitialize", hres);')
    replace('        ERR( "Could not initialize Comctl\\n" );',
            '        MihawkWineStartupFailure("InitCommonControlsEx", E_FAIL);')
    return source


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=HERE.parent / "build/native/source")
    args = parser.parse_args()
    lock = json.loads((HERE.parent / "source-lock.json").read_text(encoding="utf-8"))
    entry = next(value for value in lock["files"] if value["project"] == "wine" and value["path"] == "programs/explorer/explorer.c")
    data = (SOURCE_ROOT / entry["path"]).read_bytes()
    validate(data, entry)
    original = data.decode("utf-8")
    generated = adapt(original)
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "explorer.c").write_bytes(generated.encode("utf-8"))
    patch = "".join(difflib.unified_diff(original.splitlines(keepends=True), generated.splitlines(keepends=True),
                                      "a/programs/explorer/explorer.c", "b/programs/explorer/explorer.c"))
    (args.output / "wine-explorer-native.patch").write_bytes(patch.encode("utf-8"))
    (args.output / "provenance.json").write_text(json.dumps({
        "schema": 1,
        "repository": lock["projects"]["wine"]["repository"],
        "commit": lock["projects"]["wine"]["commit"],
        "path": entry["path"],
        "upstream_sha256": entry["sha256"],
        "adapted_sha256": hashlib.sha256(generated.encode()).hexdigest(),
        "license": "LGPL-2.1-or-later",
        "scope": "Actual Wine Explorer file-window code; no desktop/session manager or replacement shell provider",
        "execution_status": "not executed by source preparation",
    }, indent=2) + "\n", encoding="utf-8")
    print(f"Prepared actual pinned Wine Explorer and reviewable patch in {args.output}")


if __name__ == "__main__":
    main()
