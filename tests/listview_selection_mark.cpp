#include <windows.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdlib.h>
#include "../listview_selection.h"

#if COMMON_CONTROLS_VERSION == 6
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='x86' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

static void require(bool condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "FAIL: %s (Win32 error %lu)\n", message, GetLastError());
        exit(1);
    }
}

static int failures = 0;
static void check(bool condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

static int CALLBACK compare(LPARAM first, LPARAM second, LPARAM direction)
{
    return ((first > second) - (first < second)) * (int)direction;
}

static LPARAM identity(HWND list, int index)
{
    LVITEMA item = {};
    item.mask = LVIF_PARAM;
    item.iItem = index;
    require(ListView_GetItem(list, &item) != FALSE, "Read row identity");
    return item.lParam;
}

int main()
{
    INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_LISTVIEW_CLASSES};
    require(InitCommonControlsEx(&controls) != FALSE, "Initialize list controls");
    HMODULE module = GetModuleHandleA("comctl32.dll");
    require(module != NULL, "Find loaded common controls");
    DLLGETVERSIONPROC getVersion = (DLLGETVERSIONPROC)GetProcAddress(module, "DllGetVersion");
    require(getVersion != NULL, "Find control version function");
    DLLVERSIONINFO version = {};
    version.cbSize = sizeof(version);
    require(SUCCEEDED(getVersion(&version)), "Read control version");
    printf("Loaded common controls %lu.%lu build %lu\n",
        version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber);
    require(version.dwMajorVersion == COMMON_CONTROLS_VERSION, "Load requested control version");
    HWND parent = CreateWindowExA(0, "STATIC", "Control test", WS_OVERLAPPED,
        0, 0, 100, 100, NULL, NULL, GetModuleHandle(NULL), NULL);
    require(parent != NULL, "Create hidden parent");

    int cases = 0;
    const LPARAM ids[] = {30, 10, 20};
    for (int mark = -1; mark < 3; ++mark)
    for (int focus = -1; focus < 3; ++focus)
    for (int direction = -1; direction <= 1; direction += 2)
    {
        HWND list = CreateWindowExA(0, WC_LISTVIEWA, "", WS_CHILD | LVS_REPORT,
            0, 0, 100, 100, parent, NULL, GetModuleHandle(NULL), NULL);
        require(list != NULL, "Create hidden list");
        for (int row = 0; row < 3; ++row)
        {
            LVITEMA item = {};
            item.mask = LVIF_PARAM | LVIF_STATE;
            item.iItem = row;
            item.lParam = ids[row];
            item.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
            item.state = (row != 1 ? LVIS_SELECTED : 0) |
                         (row == focus ? LVIS_FOCUSED : 0);
            require(ListView_InsertItem(list, &item) == row, "Insert row");
        }
        ListView_SetSelectionMark(list, mark);
        require(ListView_GetSelectionMark(list) == mark, "Set initial mark");
        require(SortListItemsPreservingSelectionMark(list, compare, direction) != FALSE, "Sort rows and restore selection mark");

        int actualMark = ListView_GetSelectionMark(list);
        printf("CASE: controls=%d mark=%d focus=%d direction=%d final_mark=%d\n",
            COMMON_CONTROLS_VERSION, mark, focus, direction, actualMark);
        fflush(stdout);
        check(mark < 0 ? actualMark == -1 :
            actualMark >= 0 && identity(list, actualMark) == ids[mark],
            "Sorting preserves marked identity");
        int actualFocus = ListView_GetNextItem(list, -1, LVNI_FOCUSED);
        check(focus < 0 ? actualFocus == -1 :
            actualFocus >= 0 && identity(list, actualFocus) == ids[focus],
            "Sorting preserves focused identity");
        for (int row = 0; row < 3; ++row)
        {
            LPARAM id = identity(list, row);
            check(id == (direction > 0 ? (row + 1) * 10 : (3 - row) * 10),
                "Rows have the requested order");
            check(!!ListView_GetItemState(list, row, LVIS_SELECTED) == (id != 10),
                "Sorting preserves selected identities");
        }
        require(DestroyWindow(list) != FALSE, "Destroy list");
        ++cases;
    }
    require(DestroyWindow(parent) != FALSE, "Destroy parent");
    printf("%d cases, %d failed assertions using the real Windows list control\n", cases, failures);
    return failures ? 1 : 0;
}
