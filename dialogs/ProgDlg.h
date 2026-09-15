// ProgDlg.h : header file
// CG: This file was added by the Progress Dialog component

/////////////////////////////////////////////////////////////////////////////
// CProgressDlg dialog

#ifndef __PROGDLG_H__
#define __PROGDLG_H__

class CProgressDlg : public CDialog
{
// Construction / Destruction
public:
    CProgressDlg(UINT nCaptionID = 0);   // standard constructor
    ~CProgressDlg();

    BOOL Create(CWnd *pParent=NULL);
    static bool IsPumpingMessages ();
    bool IsUsable () const;
    void BeginOperation ();
    void EndOperation ();
    void RequestDelete ();

    // Checking for Cancel button
    BOOL CheckCancelButton();
    // Progress Dialog manipulation
    void SetStatus(LPCTSTR lpszMessage);
    void SetRange(int nLower,int nUpper);
    int  SetStep(int nStep);
    int  SetPos(int nPos);
    int  OffsetPos(int nPos);
    int  StepIt();

    bool m_bHideCancel;

// Dialog Data
    //{{AFX_DATA(CProgressDlg)
    enum { IDD = CG_IDD_PROGRESS };
    CProgressCtrl	m_Progress;
    //}}AFX_DATA

// Overrides
    // ClassWizard generated virtual function overrides
    //{{AFX_VIRTUAL(CProgressDlg)
    public:
    virtual BOOL DestroyWindow();
    protected:
    virtual void DoDataExchange(CDataExchange* pDX);    // DDX/DDV support
    //}}AFX_VIRTUAL

// Implementation
protected:
	UINT m_nCaptionID;
    int m_nLower;
    int m_nUpper;
    int m_nStep;
    
    BOOL m_bCancel;
    BOOL m_bParentDisabled;
    int m_iActiveOperations;
    bool m_bDeletePending;
    HWND m_hParentWindow;
    CWnd * m_pParentWindowIdentity;

    void ReEnableParent();

    virtual void OnCancel();
    virtual void OnOK() {}; 
    void UpdatePercent(int nCurrent);
    void PumpMessages();

    // Generated message map functions
    //{{AFX_MSG(CProgressDlg)
    virtual BOOL OnInitDialog();
    //}}AFX_MSG
    DECLARE_MESSAGE_MAP()
};

// A Lua callback can close the dialog that is currently pumping messages.
class CProgressDlgOperationGuard
  {
  public:
    explicit CProgressDlgOperationGuard (CProgressDlg * pDialog) : m_pDialog (pDialog)
      { m_pDialog->BeginOperation (); }
    ~CProgressDlgOperationGuard () { m_pDialog->EndOperation (); }
  private:
    CProgressDlg * m_pDialog;
    CProgressDlgOperationGuard (const CProgressDlgOperationGuard &);
    CProgressDlgOperationGuard & operator= (const CProgressDlgOperationGuard &);
  };

#endif // __PROGDLG_H__
