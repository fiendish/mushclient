#ifndef MUSHCLIENT_CONTROL_REDRAW_H
#define MUSHCLIENT_CONTROL_REDRAW_H

// All redraw batches for a control must share its bRedrawDisabled flag.
// The control must outlive the batch. Nested batches leave redraw disabled.
class CControlRedrawGuard
  {
  HWND m_hWnd;
  bool & m_bRedrawDisabled;
  bool m_bOwnsDisable;
  bool m_bWasVisible;

  CControlRedrawGuard (const CControlRedrawGuard &);
  CControlRedrawGuard & operator= (const CControlRedrawGuard &);

public:
  CControlRedrawGuard (HWND hWnd, bool & bRedrawDisabled)
    : m_hWnd (hWnd), m_bRedrawDisabled (bRedrawDisabled),
      m_bOwnsDisable (!bRedrawDisabled && !::GetPropA (hWnd, "SysSetRedraw")),
      m_bWasVisible ((::GetWindowLong (hWnd, GWL_STYLE) & WS_VISIBLE) != 0)
    {
    if (m_bOwnsDisable)
      {
      m_bRedrawDisabled = true;
      ::SendMessage (m_hWnd, WM_SETREDRAW, FALSE, 0);
      }
    }

  void Restore ()
    {
    if (!m_bOwnsDisable)
      return;
    m_bOwnsDisable = false;
    ::SendMessage (m_hWnd, WM_SETREDRAW, TRUE, 0);
    // DefWindowProc can add WS_VISIBLE when redraw is enabled.
    if (!m_bWasVisible)
      ::ShowWindow (m_hWnd, SW_HIDE);
    m_bRedrawDisabled = false;
    ::RedrawWindow (m_hWnd, NULL, NULL,
                    RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
    }

  ~CControlRedrawGuard () { Restore (); }
  };

#endif // MUSHCLIENT_CONTROL_REDRAW_H
