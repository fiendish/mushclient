//  ProgDlg.cpp : implementation file
// CG: This file was added by the Progress Dialog component

#include "stdafx.h"
#include "..\resource.h"
#include "ProgDlg.h"

#ifdef _DEBUG
#undef THIS_FILE
static char BASED_CODE THIS_FILE[] = __FILE__;
#endif

// Counts nested progress message loops on the UI thread.
static int s_iProgressMessageDepth = 0;

class CProgressMessageScope
  {
  public:
    CProgressMessageScope () { ++s_iProgressMessageDepth; }
    ~CProgressMessageScope () { --s_iProgressMessageDepth; }
  private:
    CProgressMessageScope (const CProgressMessageScope &);
    CProgressMessageScope & operator= (const CProgressMessageScope &);
  };

/////////////////////////////////////////////////////////////////////////////
// CProgressDlg dialog

CProgressDlg::CProgressDlg(UINT nCaptionID)
{
	m_nCaptionID = CG_IDS_PROGRESS_CAPTION;
	if (nCaptionID != 0)
		m_nCaptionID = nCaptionID;

    m_bCancel=FALSE;
    m_nLower=0;
    m_nUpper=100;
    m_nStep=10;
    //{{AFX_DATA_INIT(CProgressDlg)
    // NOTE: the ClassWizard will add member initialization here
    //}}AFX_DATA_INIT
    m_bParentDisabled = FALSE;
    m_bHideCancel = FALSE;
    m_iActiveOperations = 0;
    m_bDeletePending = false;
    m_hParentWindow = NULL;
    m_pParentWindowIdentity = NULL;
}

CProgressDlg::~CProgressDlg()
{
    if(m_hWnd!=NULL)
      DestroyWindow();
}

bool CProgressDlg::IsPumpingMessages ()
{
    return s_iProgressMessageDepth != 0;
}

bool CProgressDlg::IsUsable () const
{
    return !m_bDeletePending && m_hWnd != NULL && ::IsWindow (m_hWnd) &&
      CWnd::FromHandlePermanent (m_hWnd) == this;
}

void CProgressDlg::BeginOperation ()
{
    ++m_iActiveOperations;
}

void CProgressDlg::EndOperation ()
{
    ASSERT (m_iActiveOperations > 0);
    if (--m_iActiveOperations == 0 && m_bDeletePending)
      delete this;
}

void CProgressDlg::RequestDelete ()
{
    if (m_iActiveOperations != 0)
    {
      m_bDeletePending = true;
      m_bCancel = TRUE;
      return;
    }
    delete this;
}

BOOL CProgressDlg::DestroyWindow()
{
    ReEnableParent();
    return CDialog::DestroyWindow();
}

void CProgressDlg::ReEnableParent()
{
    const bool bEnable = m_bParentDisabled && ::IsWindow (m_hParentWindow) &&
      CWnd::FromHandlePermanent (m_hParentWindow) == m_pParentWindowIdentity;
    m_bParentDisabled=FALSE;
    if (bEnable)
      ::EnableWindow (m_hParentWindow, TRUE);
}

BOOL CProgressDlg::Create(CWnd *pParent)
{
    // Get the true parent of the dialog
    m_pParentWnd = CWnd::GetSafeOwner(pParent);
    m_hParentWindow = m_pParentWnd ? m_pParentWnd->GetSafeHwnd () : NULL;
    m_pParentWindowIdentity = CWnd::FromHandlePermanent (m_hParentWindow);

    // m_bParentDisabled is used to re-enable the parent window
    // when the dialog is destroyed. So we don't want to set
    // it to TRUE unless the parent was already enabled.

    if((m_pParentWnd!=NULL) && m_pParentWnd->IsWindowEnabled())
    {
      m_pParentWnd->EnableWindow(FALSE);
      m_bParentDisabled = TRUE;
    }

    if(!CDialog::Create(CProgressDlg::IDD,pParent))
    {
      ReEnableParent();
      return FALSE;
    }

    return TRUE;
}

void CProgressDlg::DoDataExchange(CDataExchange* pDX)
{
    CDialog::DoDataExchange(pDX);
    //{{AFX_DATA_MAP(CProgressDlg)
    DDX_Control(pDX, CG_IDC_PROGDLG_PROGRESS, m_Progress);
    //}}AFX_DATA_MAP
}

BEGIN_MESSAGE_MAP(CProgressDlg, CDialog)
    //{{AFX_MSG_MAP(CProgressDlg)
    //}}AFX_MSG_MAP
END_MESSAGE_MAP()

void CProgressDlg::SetStatus(LPCTSTR lpszMessage)
{
    if (!IsUsable ())
      return;
    CWnd *pWndStatus = GetDlgItem(CG_IDC_PROGDLG_STATUS);

    // Verify that the static text control exists
    ASSERT(pWndStatus!=NULL);
    pWndStatus->SetWindowText(lpszMessage);
}

void CProgressDlg::OnCancel()
{
    m_bCancel=TRUE;
}

void CProgressDlg::SetRange(int nLower,int nUpper)
{
    if (!IsUsable ())
      return;
    m_nLower = nLower;
    m_nUpper = nUpper;
    m_Progress.SetRange32(nLower,nUpper);
}
  
int CProgressDlg::SetPos(int nPos)
{
    PumpMessages();
    if (!IsUsable ())
      return 0;
    int iResult = m_Progress.SetPos(nPos);
    UpdatePercent(nPos);
    return iResult;
}

int CProgressDlg::SetStep(int nStep)
{
    if (!IsUsable ())
      return 0;
    m_nStep = nStep; // Store for later use in calculating percentage
    return m_Progress.SetStep(nStep);
}

int CProgressDlg::OffsetPos(int nPos)
{
    PumpMessages();
    if (!IsUsable ())
      return 0;
    int iResult = m_Progress.OffsetPos(nPos);
    UpdatePercent(iResult+nPos);
    return iResult;
}

int CProgressDlg::StepIt()
{
    PumpMessages();
    if (!IsUsable ())
      return 0;
    int iResult = m_Progress.StepIt();
    UpdatePercent(iResult+m_nStep);
    return iResult;
}

void CProgressDlg::PumpMessages()
{
    if (!IsUsable ())
    {
      m_bCancel = TRUE;
      return;
    }
    CProgressMessageScope messageScope;
    MSG msg;
    // Preserve a request to terminate the application.
    if(::PeekMessage(&msg, NULL, WM_QUIT, WM_QUIT, PM_REMOVE))
    {
      m_bCancel = TRUE;
      ::PostQuitMessage((int) msg.wParam);
      return;
    }

    // Socket notifications must keep running during long operations.
    // Callers retain their data across callbacks; document and application
    // closure is deferred until the main loop resumes.
    while(IsUsable () && !m_bCancel && ::PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
    {
      if(msg.message == WM_QUIT)
      {
        m_bCancel = TRUE;
        ::PostQuitMessage((int) msg.wParam);
        return;
      }

      // PeekMessage can dispatch sent messages that close this dialog.
      // Still dispatch any retrieved message for another window.
      if(!IsUsable () || !IsDialogMessage(&msg))
      {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
      }
    }
    if (!IsUsable ())
      m_bCancel = TRUE;
}

BOOL CProgressDlg::CheckCancelButton()
{
    // Process all pending messages
    PumpMessages();

    // Reset m_bCancel to FALSE so that
    // CheckCancelButton returns FALSE until the user
    // clicks Cancel again. This will allow you to call
    // CheckCancelButton and still continue the operation.
    // If m_bCancel stayed TRUE, then the next call to
    // CheckCancelButton would always return TRUE

    BOOL bResult = m_bCancel;
    m_bCancel = FALSE;

    return bResult;
}

void CProgressDlg::UpdatePercent(int nNewPos)
{
    if (!IsUsable ())
      return;
    CWnd *pWndPercent = GetDlgItem(CG_IDC_PROGDLG_PERCENT);
    int nPercent;
    
    int nDivisor = m_nUpper - m_nLower;
    ASSERT(nDivisor>0);  // m_nLower should be smaller than m_nUpper

    int nDividend = (nNewPos - m_nLower);
    ASSERT(nDividend>=0);   // Current position should be greater than m_nLower

    nPercent = nDividend * 100 / nDivisor;

    // Since the Progress Control wraps, we will wrap the percentage
    // along with it. However, don't reset 100% back to 0%
    if(nPercent!=100)
      nPercent %= 100;

    // Display the percentage
    CString strBuf;
    strBuf.Format(_T("%d%c"),nPercent,_T('%'));

	CString strCur; // get current percentage
    const HWND hPercent = pWndPercent->GetSafeHwnd ();
    pWndPercent->GetWindowText(strCur);

    // Reading text sends a window message and can close or replace a control.
    if (!IsUsable ())
      return;
    pWndPercent = GetDlgItem (CG_IDC_PROGDLG_PERCENT);
    if (pWndPercent && pWndPercent->GetSafeHwnd () == hPercent && strCur != strBuf)
      pWndPercent->SetWindowText(strBuf);
}
    
/////////////////////////////////////////////////////////////////////////////
// CProgressDlg message handlers

BOOL CProgressDlg::OnInitDialog() 
{
    CDialog::OnInitDialog();
    m_Progress.SetRange(m_nLower,m_nUpper);
    m_Progress.SetStep(m_nStep);
    m_Progress.SetPos(m_nLower);

    if (m_bHideCancel)
      GetDlgItem (IDCANCEL)->ShowWindow (SW_HIDE); 

    return TRUE;  
}
