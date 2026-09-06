// cmdhist.cpp : implementation file
//

#include "stdafx.h"
#include "..\MUSHclient.h"
#include "..\doc.h"
#include "..\sendvw.h"
#include "cmdhist.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char BASED_CODE THIS_FILE[] = __FILE__;
#endif

/////////////////////////////////////////////////////////////////////////////
// CCmdHistory dialog


CCmdHistory::CCmdHistory(CWnd* pParent /*=NULL*/)
	: CDialog(CCmdHistory::IDD, pParent)
{
	//{{AFX_DATA_INIT(CCmdHistory)
	//}}AFX_DATA_INIT
  m_msgList = NULL;
  m_sendview = NULL;
  m_pDoc = NULL;
  m_iDocumentNumber = 0;
  m_hSendView = NULL;
}


// Copy settings without sharing a cursor, match list, or owned search resources.
static void CopyFindSettings (CFindInfo & destination, const CFindInfo & source)
  {
  destination.m_strTitle = source.m_strTitle;
  destination.m_bCanGoBackwards = source.m_bCanGoBackwards;
  destination.m_bForwards = source.m_bForwards;
  destination.m_bMatchCase = source.m_bMatchCase;
  destination.m_bRegexp = source.m_bRegexp;
  destination.m_bUTF8 = source.m_bUTF8;
  destination.m_iControlColumns = source.m_iControlColumns;
  destination.m_bRepeatOnSameLine = source.m_bRepeatOnSameLine;
  destination.m_strFindStringList.RemoveAll ();
  for (POSITION pos = source.m_strFindStringList.GetHeadPosition (); pos; )
    destination.m_strFindStringList.AddTail (source.m_strFindStringList.GetNext (pos));
  }

void CCmdHistory::SetFindInfo (const std::shared_ptr<CFindInfo> & pFindInfo)
  {
  m_pHistoryFindInfo = pFindInfo;
  CopyFindSettings (m_HistoryFindInfo, *pFindInfo);
  }

void CCmdHistory::DoDataExchange(CDataExchange* pDX)
{
	CDialog::DoDataExchange(pDX);
	//{{AFX_DATA_MAP(CCmdHistory)
	DDX_Control(pDX, IDC_HISTORY_ITEM, m_ctlHistoryItem);
	//}}AFX_DATA_MAP

if(!(pDX->m_bSaveAndValidate))
    {
CListBox* pList = (CListBox*) GetDlgItem (IDC_COMMANDS);
ASSERT (pList);
int count = 0;

   pList->SetRedraw (FALSE);
   pList->ResetContent ();
   m_msgListSnapshot.RemoveAll ();

  CString str;
  POSITION pos = m_msgList->GetHeadPosition ();

  while (pos)
    {
    int nItem;
    str = m_msgList->GetNext (pos);
    CString strDisplay = str;

    // truncate long strings or we might get a nasty crash with long strings

    if (strDisplay.GetLength () > 500)
      {
      strDisplay = strDisplay.Left (500);
      strDisplay += " ...";
      }

    nItem = pList->AddString(strDisplay);  // add to list (truncate to 500 chars)
    if (nItem != LB_ERR  && nItem != LB_ERRSPACE)
      {
      POSITION itemPos = m_msgListSnapshot.AddTail (str);
      pList->SetItemData (nItem, (DWORD) itemPos);
      count++;
      }
    }

   pList->SetCurSel(count - 1);
   pList->SetRedraw (TRUE);
   OnSelchangeCommands ();    // make sure item is displayed in history box
    }   // end of loading the dialog

}

bool CCmdHistory::IsContextLive (void) const
  {
  if (!m_pDoc || !m_sendview || !m_hSendView)
    return false;

  bool bDocumentLive = false;
  for (POSITION pos = App.m_pWorldDocTemplate->GetFirstDocPosition(); pos; )
    {
    CMUSHclientDoc * pDoc =
      (CMUSHclientDoc *) App.m_pWorldDocTemplate->GetNextDoc (pos);
    if (pDoc == m_pDoc && pDoc->m_iUniqueDocumentNumber == m_iDocumentNumber)
      {
      bDocumentLive = true;
      break;
      }
    }

  return bDocumentLive && ::IsWindow (m_hSendView) &&
         CWnd::FromHandlePermanent (m_hSendView) == m_sendview;
  }


BEGIN_MESSAGE_MAP(CCmdHistory, CDialog)
	//{{AFX_MSG_MAP(CCmdHistory)
	ON_LBN_DBLCLK(IDC_COMMANDS, OnDblclkCommands)
	ON_BN_CLICKED(IDC_HELPBUTTON, OnHelpbutton)
	ON_BN_CLICKED(IDC_FIND, OnFind)
	ON_BN_CLICKED(IDC_FIND_NEXT, OnFindNext)
	ON_LBN_SELCHANGE(IDC_COMMANDS, OnSelchangeCommands)
	ON_BN_CLICKED(IDC_DO, OnDo)
	ON_BN_CLICKED(IDC_NOTEPAD, OnNotepad)
	//}}AFX_MSG_MAP
END_MESSAGE_MAP()


/////////////////////////////////////////////////////////////////////////////
// CCmdHistory message handlers

void CCmdHistory::OnOK() 
{
CListBox* pList = (CListBox*) GetDlgItem (IDC_COMMANDS);
ASSERT (pList);
int selection = pList->GetCurSel ();
CString str;

if (selection == LB_ERR)
  return;

if (!IsContextLive ())
  {
  CDialog::OnCancel ();
  return;
  }

POSITION pos = (POSITION) pList->GetItemData (selection);
str = m_msgListSnapshot.GetAt (pos);

// check they want to wipe out their typing

const bool bKeepTyping = m_sendview->CheckTyping (m_pDoc, str);
if (!IsContextLive ())
  {
  CDialog::OnCancel ();
  return;
  }
if (bKeepTyping)
  return;

m_sendview->SetCommand (str);
	
	CDialog::OnOK();
}

void CCmdHistory::OnDblclkCommands() 
{
  OnOK ();    // same as clicking on OK button
}


void CCmdHistory::OnHelpbutton() 
{
//	  OnCmdMsg(ID_HELP, CN_COMMAND, NULL, NULL);	
	App.HelpHelper(m_nIDHelp + HID_BASE_RESOURCE);
}

void CCmdHistory::OnFind() 
{
  DoFind (false);
}

void CCmdHistory::OnFindNext() 
{
  DoFind (true);
}


void CCmdHistory::DoFind (bool bAgain)
  {
CListBox* pList = (CListBox*) GetDlgItem (IDC_COMMANDS);
ASSERT (pList);

if (!IsContextLive ())
  {
  CDialog::OnCancel ();
  return;
  }

m_HistoryFindInfo.m_bAgain = bAgain;
m_HistoryFindInfo.m_nTotalLines = m_msgListSnapshot.GetCount ();
int selection = pList->GetCurSel ();
if (selection != LB_ERR)
  m_HistoryFindInfo.m_nCurrentLine = selection;

// Find Next can use saved settings before this dialog has compiled the pattern.
if (bAgain && m_HistoryFindInfo.m_bRegexp &&
    !m_HistoryFindInfo.m_regexp && !m_HistoryFindInfo.m_strFindStringList.IsEmpty ())
  m_HistoryFindInfo.m_regexp = regcomp (m_HistoryFindInfo.m_strFindStringList.GetHead (),
      (m_HistoryFindInfo.m_bMatchCase ? 0 : PCRE_CASELESS) |
      (m_HistoryFindInfo.m_bUTF8 ? PCRE_UTF8 : 0));

bool found = FindRoutine (&m_msgListSnapshot,    // passed back to callback routines
                          m_HistoryFindInfo,     // finding structure
                          InitiateSearch,        // how to re-initiate a find
                          GetNextLine);          // get the next line

// FindRoutine can process messages that close the world or destroy the send view.
if (!IsContextLive ())
  {
  CDialog::OnCancel ();
  return;
  }

m_HistoryFindInfo.m_pFindPosition = NULL;
CopyFindSettings (*m_pHistoryFindInfo, m_HistoryFindInfo);

// Get the control again after the nested message loops.
pList = (CListBox*) GetDlgItem (IDC_COMMANDS);
ASSERT (pList);
	
  if (found)
    pList->SetCurSel (m_HistoryFindInfo.m_nCurrentLine);
  else
    pList->SetCurSel (-1);

  OnSelchangeCommands ();

  } // end of CCmdHistory::DoFind



void CCmdHistory::InitiateSearch (const CObject * pObject,
                                  CFindInfo & FindInfo)
  {
CStringList* pMsgList = (CStringList*) pObject;

  if (FindInfo.m_bAgain)
    FindInfo.m_pFindPosition = pMsgList->FindIndex (FindInfo.m_nCurrentLine);
  else
    if (FindInfo.m_bForwards)
      FindInfo.m_pFindPosition = pMsgList->GetHeadPosition ();
    else
      FindInfo.m_pFindPosition = pMsgList->GetTailPosition ();

  } // end of CSendView::InitiateSearch

bool CCmdHistory::GetNextLine (const CObject * pObject,
                             CFindInfo & FindInfo, 
                             CString & strLine)
  {
CStringList* pMsgList = (CStringList*) pObject;

  if (FindInfo.m_pFindPosition == NULL)
    return true;

  if (FindInfo.m_bForwards)
    strLine = pMsgList->GetNext (FindInfo.m_pFindPosition);
  else
    strLine = pMsgList->GetPrev (FindInfo.m_pFindPosition);

  return false;
  } // end of CSendView::GetNextLine

void CCmdHistory::OnSelchangeCommands() 
{
CListBox* pList = (CListBox*) GetDlgItem (IDC_COMMANDS);
ASSERT (pList);
int selection = pList->GetCurSel ();
CString str;

if (selection == LB_ERR)
  {
  m_ctlHistoryItem.SetWindowText ("");
  return;
  }

pList->GetText (selection, str);

m_ctlHistoryItem.SetWindowText (str);
	
}


void CCmdHistory::OnDo() 
{

CListBox* pList = (CListBox*) GetDlgItem (IDC_COMMANDS);
ASSERT (pList);
int selection = pList->GetCurSel ();
CString str;

if (selection == LB_ERR)
  return;

if (!IsContextLive ())
  {
  CDialog::OnCancel ();
  return;
  }

POSITION pos = (POSITION) pList->GetItemData (selection);
str = m_msgListSnapshot.GetAt (pos);

m_sendview->SendCommand (str, TRUE);
	
}

void CCmdHistory::OnNotepad() 
{
CListBox* pList = (CListBox*) GetDlgItem (IDC_COMMANDS);
ASSERT (pList);
int selection = pList->GetCurSel ();
CString str;

  if (selection == LB_ERR)
    return;

  if (!IsContextLive ())
    {
    CDialog::OnCancel ();
    return;
    }

  POSITION pos = (POSITION) pList->GetItemData (selection);
  str = m_msgListSnapshot.GetAt (pos);

  // edit current input window
  CreateTextWindow ((LPCTSTR) str,     // command
                    (LPCTSTR) TFormat ("Notepad: %s", (LPCTSTR) m_pDoc->m_mush_name),     // title
                    m_pDoc,   // document
                    m_pDoc->m_iUniqueDocumentNumber,      // document number
                    m_pDoc->m_input_font_name,
                    m_pDoc->m_input_font_height,
                    m_pDoc->m_input_font_weight,
                    m_pDoc->m_input_font_charset,
                    m_pDoc->m_input_text_colour,
                    m_pDoc->m_input_background_colour,
                    "",       // search string
                    "",       // line preamble
                    false,
                    false,
                    false,
                    false,  
                    false,
                    false,
                    eNotepadNormal
                    );

  // bye-bye dialog box
	CDialog::OnOK();

}
