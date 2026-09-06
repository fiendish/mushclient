// ChooseNotepadDlg.cpp : implementation file
//

#include "stdafx.h"
#include "..\mushclient.h"
#include "..\TextDocument.h"
#include "..\doc.h"
#include "ChooseNotepadDlg.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

/////////////////////////////////////////////////////////////////////////////
// CChooseNotepadDlg dialog


CChooseNotepadDlg::CChooseNotepadDlg(CWnd* pParent /*=NULL*/)
	: CDialog(CChooseNotepadDlg::IDD, pParent)
{
	//{{AFX_DATA_INIT(CChooseNotepadDlg)
		// NOTE: the ClassWizard will add member initialization here
	//}}AFX_DATA_INIT
 m_pWorld = NULL;
 m_pTextDocument = NULL;
 m_iWorldDocumentNumber = 0;

}


void CChooseNotepadDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialog::DoDataExchange(pDX);
	//{{AFX_DATA_MAP(CChooseNotepadDlg)
	DDX_Control(pDX, IDC_NOTEPAD_LIST, m_ctlNotepadList);
	//}}AFX_DATA_MAP

  POSITION docPos;

  if (!pDX->m_bSaveAndValidate)
    {   // loading

    CMUSHclientDoc * pWorld = GetLiveWorld ();
    if (!pWorld)
      return;

    m_NotepadDocumentNumbers.RemoveAll ();

    for (docPos = App.m_pNormalDocTemplate->GetFirstDocPosition();
        docPos != NULL; )
      {
      int nItem;

      CTextDocument * pDoc = (CTextDocument *) App.m_pNormalDocTemplate->GetNextDoc(docPos);

      // ignore unrelated worlds
      if (pDoc->m_pRelatedWorld != pWorld ||
          pDoc->m_iUniqueDocumentNumber != m_iWorldDocumentNumber)
        continue;

      CString strTitle = pDoc->GetPathName ();
      if (strTitle.IsEmpty ())
         strTitle = pDoc->m_strTitle;
      if (strTitle.IsEmpty ())
        strTitle = pDoc->GetTitle ();

      nItem = m_ctlNotepadList.AddString (strTitle);

      if (nItem != LB_ERR  && nItem != LB_ERRSPACE )
        {
        int iDocument = m_NotepadDocumentNumbers.Add (pDoc->m_iTextDocumentNumber);
        m_ctlNotepadList.SetItemData (nItem, (DWORD) iDocument);
        }

      } // end of doing each document

    }   // end of loading

}

CMUSHclientDoc * CChooseNotepadDlg::GetLiveWorld (void) const
  {
  if (!m_pWorld)
    return NULL;

  for (POSITION pos = App.m_pWorldDocTemplate->GetFirstDocPosition(); pos; )
    {
    CMUSHclientDoc * pWorld =
      (CMUSHclientDoc *) App.m_pWorldDocTemplate->GetNextDoc (pos);
    if (pWorld == m_pWorld &&
        pWorld->m_iUniqueDocumentNumber == m_iWorldDocumentNumber)
      return pWorld;
    }

  return NULL;
  }

CTextDocument * CChooseNotepadDlg::GetNotepadForIndex (const int iIndex) const
  {
  CMUSHclientDoc * pWorld = GetLiveWorld ();
  if (!pWorld || iIndex < 0 || iIndex >= m_NotepadDocumentNumbers.GetSize ())
    return NULL;

  __int64 iDocumentNumber = m_NotepadDocumentNumbers [iIndex];
  for (POSITION pos = App.m_pNormalDocTemplate->GetFirstDocPosition(); pos; )
    {
    CTextDocument * pDoc =
      (CTextDocument *) App.m_pNormalDocTemplate->GetNextDoc (pos);
    if (pDoc->m_iTextDocumentNumber == iDocumentNumber &&
        pDoc->m_pRelatedWorld == pWorld &&
        pDoc->m_iUniqueDocumentNumber == m_iWorldDocumentNumber)
      return pDoc;
    }

  return NULL;
  }


BEGIN_MESSAGE_MAP(CChooseNotepadDlg, CDialog)
	//{{AFX_MSG_MAP(CChooseNotepadDlg)
	ON_BN_CLICKED(IDC_OPEN_EXISTING, OnOpenExisting)
	ON_LBN_DBLCLK(IDC_NOTEPAD_LIST, OnDblclkNotepadList)
	//}}AFX_MSG_MAP
  ON_MESSAGE(WM_KICKIDLE, OnKickIdle)
  ON_UPDATE_COMMAND_UI(IDC_OPEN_EXISTING, OnUpdateNeedSelection)
END_MESSAGE_MAP()

/////////////////////////////////////////////////////////////////////////////
// CChooseNotepadDlg message handlers

void CChooseNotepadDlg::OnOpenExisting() 
{
int nItem = m_ctlNotepadList.GetCurSel ();

  if (nItem == LB_ERR)
    return;

  m_pTextDocument = GetNotepadForIndex ((int) m_ctlNotepadList.GetItemData (nItem));
  if (!m_pTextDocument)
    {
    m_ctlNotepadList.DeleteString (nItem);
    return;
    }

  OnOK ();
	
}


LRESULT CChooseNotepadDlg::OnKickIdle(WPARAM, LPARAM)
  {
  UpdateDialogControls (AfxGetApp()->m_pMainWnd, false);
  return 0;
  } // end of CChooseNotepadDlg::OnKickIdle

void CChooseNotepadDlg::OnUpdateNeedSelection(CCmdUI* pCmdUI)
{
int nItem = m_ctlNotepadList.GetCurSel ();

	pCmdUI->Enable(nItem != LB_ERR);
}


void CChooseNotepadDlg::OnDblclkNotepadList() 
{
OnOpenExisting ();	
}
