#ifndef MUSHCLIENT_LISTVIEW_SELECTION_H
#define MUSHCLIENT_LISTVIEW_SELECTION_H

// Item data must uniquely identify each row and remain unchanged during sorting.
inline BOOL SortListItemsPreservingSelectionMark (HWND list,
                                                 PFNLVCOMPARE compare,
                                                 LPARAM context)
{
  const int mark = ListView_GetSelectionMark (list);
  LVITEM item = {0};
  if (mark != -1)
    {
    item.mask = LVIF_PARAM;
    item.iItem = mark;
    if (!ListView_GetItem (list, &item))
      return FALSE;
    }

  const BOOL sorted = ListView_SortItems (list, compare, context);
  int restoredMark = -1;
  if (mark != -1)
    {
    LVFINDINFO find = {0};
    find.flags = LVFI_PARAM;
    find.lParam = item.lParam;
    restoredMark = ListView_FindItem (list, -1, &find);
    if (restoredMark == -1)
      return FALSE;
    }
  ListView_SetSelectionMark (list, restoredMark);
  return sorted && ListView_GetSelectionMark (list) == restoredMark;
}

#endif
