// cmdhist.h : header file
//

/////////////////////////////////////////////////////////////////////////////
// CCmdHistory dialog

class CCmdHistory : public CDialog
{
// Construction
public:
	CCmdHistory(CWnd* pParent = NULL);   // standard constructor

  CStringList * m_msgList;
  CStringList m_msgListSnapshot;
  CSendView * m_sendview;
  std::shared_ptr<CFindInfo> m_pHistoryFindInfo;
  CFindInfo m_HistoryFindInfo;
  CMUSHclientDoc* m_pDoc;
  __int64 m_iDocumentNumber;
  HWND m_hSendView;

  void SetFindInfo (const std::shared_ptr<CFindInfo> & pFindInfo);

// Dialog Data
	//{{AFX_DATA(CCmdHistory)
	enum { IDD = IDD_COMMAND_HISTORY };
	CEdit	m_ctlHistoryItem;
	//}}AFX_DATA


// Overrides
	// ClassWizard generated virtual function overrides
	//{{AFX_VIRTUAL(CCmdHistory)
	protected:
	virtual void DoDataExchange(CDataExchange* pDX);    // DDX/DDV support
	//}}AFX_VIRTUAL

// Implementation
protected:


// for finding

  static void InitiateSearch (const CObject * pObject,
                              CFindInfo & FindInfo);
 
  static bool GetNextLine (const CObject * pObject,
                           CFindInfo & FindInfo, 
                           CString & strLine);

  void DoFind (bool bAgain);
  bool IsContextLive (void) const;


  // Generated message map functions
	//{{AFX_MSG(CCmdHistory)
	virtual void OnOK();
	afx_msg void OnDblclkCommands();
	afx_msg void OnHelpbutton();
	afx_msg void OnFind();
	afx_msg void OnFindNext();
	afx_msg void OnSelchangeCommands();
	afx_msg void OnDo();
	afx_msg void OnNotepad();
	//}}AFX_MSG
	DECLARE_MESSAGE_MAP()
};
