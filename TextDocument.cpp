// TextDocument.cpp : implementation file
//


/*
Copyright (c) 2000 Nick Gammon.

*/

#include "stdafx.h"
#include "MUSHclient.h"
#include "TextDocument.h"
#include "dialogs\ProgDlg.h"
#include "TextView.h"
#include "MainFrm.h"
#include <errno.h>
#include <process.h>    
#include "doc.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

#ifdef _DEBUG
// for documenting menus, accelerators
void ListAccelerators (CDocument * pDoc, const int iType);
#endif 

/////////////////////////////////////////////////////////////////////////////
// CTextDocument

IMPLEMENT_DYNCREATE(CTextDocument, CDocument)

CTextDocument::CTextDocument()
{
  m_bInFileChanged = false;
  m_bFileChangedPending = false;
  m_iActiveOperations = 0;
  m_bClosePending = false;
  m_bCloseQueued = false;
  m_iMonitorToken = 0;
  m_pRelatedWorld = NULL;

  m_iUniqueDocumentNumber = 0;
  m_iTextDocumentNumber = App.GetUniqueNumber ();
  m_strFontName = App.m_strDefaultInputFont;
  m_iFontSize = App.m_iDefaultInputFontHeight;
  m_iFontWeight = App.m_iDefaultInputFontWeight;
  m_iFontCharset = App.m_iDefaultInputFontCharset;
  m_bFontItalic = false;
  m_bFontUnderline = false;
  m_bFontStrikeout = false;
  m_textColour = RGB (0, 0, 0);        // window colour
  m_backColour = RGB (255, 255, 255);
  m_bMatchCase = false;
  m_bRegexp = false;         
  m_bCommands = false;
  m_bOutput = false;
  m_bNotes = false;
  m_iLines = false;
  m_bReadOnly = FALSE;

  // save method
  m_iSaveOnChange = eNotepadSaveDefault;

  // scripting support

	EnableAutomation();     // not needed?

  if (!bWine)
  	AfxOleLockApp();        // not needed?

}

BOOL CTextDocument::OnNewDocument()
{
	if (!CDocument::OnNewDocument())
		return FALSE;
  StopMonitoringThread (m_iMonitorToken);
  m_bFileChangedPending = false;
	return TRUE;
}

CTextDocument::~CTextDocument()
{
  StopMonitoringThread (m_iMonitorToken);
  m_bFileChangedPending = false;

  if (!bWine)
  	AfxOleUnlockApp();        // not needed?

}


BEGIN_MESSAGE_MAP(CTextDocument, CDocument)
	//{{AFX_MSG_MAP(CTextDocument)
	ON_COMMAND(ID_FILE_OPEN, OnFileOpen)
	ON_UPDATE_COMMAND_UI(ID_STATUSLINE_MUSHNAME, OnUpdateStatuslineMushname)
	ON_COMMAND(ID_FILE_NEW, OnFileNew)
	ON_COMMAND(ID_EDIT_CONVERTCLIPBOARDFORUMCODES, OnEditConvertclipboardforumcodes)
	//}}AFX_MSG_MAP
END_MESSAGE_MAP()

/////////////////////////////////////////////////////////////////////////////
// CTextDocument diagnostics

#ifdef _DEBUG
void CTextDocument::AssertValid() const
{
	CDocument::AssertValid();
}

void CTextDocument::Dump(CDumpContext& dc) const
{
	CDocument::Dump(dc);
}
#endif //_DEBUG

/////////////////////////////////////////////////////////////////////////////
// CTextDocument serialization

void CTextDocument::Serialize(CArchive& ar)
{
	// CEditView contains an edit control which handles all serialization
	((CTextView*)m_viewList.GetHead())->SerializeRaw(ar);

  if (ar.IsLoading ())
    CreateMonitoringThread (ar.GetFile ()->GetFilePath ());
}

/////////////////////////////////////////////////////////////////////////////
// CTextDocument commands

/*
void CTextDocument::OnUpdateStatusModified(CCmdUI* pCmdUI) 
{
  if (IsModified ())
  	pCmdUI->SetText ("Modified");
  else
  	pCmdUI->SetText ("");
	
}

  */

void CTextDocument::OnCloseDocument() 
{
  if (m_iActiveOperations > 0 || CProgressDlg::IsPumpingMessages ())
    {
    if (!m_bCloseQueued)
      {
      App.DeferTextDocumentClose (m_iTextDocumentNumber);
      m_bCloseQueued = true;
      }
    m_bClosePending = true;
    return;
    }

  StopMonitoringThread (m_iMonitorToken);
  m_bFileChangedPending = false;
	CDocument::OnCloseDocument();
}

void CTextDocument::BeginOperation (void)
  {
  m_iActiveOperations++;
  }

void CTextDocument::EndOperation (void)
  {
  ASSERT (m_iActiveOperations > 0);
  m_iActiveOperations--;

  }

// ------------------- file change monitoring thread -------------------------

// Only the UI thread changes the registry. A worker owns no document data.
// Keep the registry alive until process exit: a blocked worker can outlive App.
struct CMonitorContext
  {
  CMonitorContext * next;
  char * filename;
  HWND window;
  UINT message;
  __int64 document;
  __int64 token;
  HANDLE stopEvent;
  HANDLE thread;
  HANDLE change;
  volatile LONG stopped;
  unsigned reported;
  unsigned pending;
  const char * operations[9];
  DWORD errors[9];
  // The first transient post error is published while the worker is live.
  DWORD postError;
  volatile LONG postErrorReady;
  // Terminal error fields are read only after confirmed worker exit.
  const char * workerOperation;
  DWORD workerError;
  };

static CMonitorContext * monitors = NULL;

static void RecordMonitorError (CMonitorContext * context, unsigned slot,
                                const char * operation, DWORD error)
  {
  const unsigned bit = 1U << slot;
  if (!(context->reported & bit))
    {
    context->reported |= bit;
    context->pending |= bit;
    context->operations[slot] = operation;
    context->errors[slot] = error;
    }
  }

static void RecordMonitorWorkerError (CMonitorContext * context,
                                      const char * operation, DWORD error)
  {
  context->workerOperation = operation;
  context->workerError = error;
  }

static bool MonitorStopped (CMonitorContext * context)
  {
  return InterlockedCompareExchange (&context->stopped, 0, 0) != 0;
  }

static unsigned __stdcall MonitorThread (void * parameter)
  {
  CMonitorContext * context = (CMonitorContext *) parameter;
  if (MonitorStopped (context))
    return 0;

  char * p = strrchr (context->filename, '\\');
  if (!p)
    p = strrchr (context->filename, ':');
  if (p)
    *p = 0;

  context->change = FindFirstChangeNotification
    (context->filename, TRUE, FILE_NOTIFY_CHANGE_LAST_WRITE);
  if (context->change == INVALID_HANDLE_VALUE)
    {
    RecordMonitorWorkerError (context, "FindFirstChangeNotification", GetLastError ());
    return 0;
    }

  HANDLE handles[2] = { context->stopEvent, context->change };
  while (!MonitorStopped (context))
    {
    // The finite wait also observes stopped if SetEvent fails.
    DWORD result = WaitForMultipleObjects (2, handles, FALSE, 250);
    if (MonitorStopped (context) || result == WAIT_OBJECT_0)
      break;
    if (result == WAIT_TIMEOUT)
      continue;
    if (result != WAIT_OBJECT_0 + 1)
      {
      RecordMonitorWorkerError (context, "WaitForMultipleObjects", GetLastError ());
      break;
      }

    CFileChangeNotification * notification = NULL;
    try
      {
      notification = new CFileChangeNotification;
      }
    catch (CException * e)
      {
      e->Delete ();
      RecordMonitorWorkerError (context, "notification allocation", ERROR_NOT_ENOUGH_MEMORY);
      return 0;
      }
    catch (...)
      {
      RecordMonitorWorkerError (context, "notification allocation", ERROR_NOT_ENOUGH_MEMORY);
      return 0;
      }
    notification->m_iDocumentNumber = context->document;
    notification->m_iMonitorToken = context->token;
    if (MonitorStopped (context))
      {
      delete notification;
      break;
      }
    if (!PostMessage (context->window, context->message, (WPARAM) notification, 0))
      {
      DWORD error = GetLastError ();
      if (!InterlockedCompareExchange (&context->postErrorReady, 0, 0))
        {
        context->postError = error;
        InterlockedExchange (&context->postErrorReady, 1);
        }
      delete notification;
      }
    if (MonitorStopped (context))
      break;
    if (!FindNextChangeNotification (context->change))
      {
      RecordMonitorWorkerError (context, "FindNextChangeNotification", GetLastError ());
      break;
      }
    }
  // Only the collector closes handles, after confirming thread exit.
  return 0;
  }

void StopMonitoringThread (__int64 & token)
  {
  const __int64 oldToken = token;
  token = 0;
  if (!oldToken)
    return;
  for (CMonitorContext * context = monitors; context; context = context->next)
    if (context->token == oldToken)
      {
      InterlockedExchange (&context->stopped, 1);
      if (context->thread && context->stopEvent && !SetEvent (context->stopEvent))
        RecordMonitorError (context, 0, "SetEvent", GetLastError ());
      return;
      }
  }

void CollectMonitoringThreads ()
  {
  const char * reportOperation = NULL;
  DWORD reportError = 0;
  CMonitorContext ** link = &monitors;
  while (*link)
    {
    CMonitorContext * context = *link;
    bool exited = context->thread == NULL;
    if (context->thread)
      {
      DWORD result = WaitForSingleObject (context->thread, 0);
      exited = result == WAIT_OBJECT_0;
      if (!exited && result != WAIT_TIMEOUT)
        RecordMonitorError (context, 1, "WaitForSingleObject", GetLastError ());
      }
    if (InterlockedCompareExchange (&context->postErrorReady, 0, 0))
      RecordMonitorError (context, 7, "PostMessage", context->postError);
    if (exited)
      {
      if (context->workerOperation)
        RecordMonitorError (context, 8, context->workerOperation, context->workerError);
      // No thread means startup failed, or its handle was already closed.
      // Retain every failed-close handle for a later idle pass.
      if (context->change && context->change != INVALID_HANDLE_VALUE)
        {
        if (FindCloseChangeNotification (context->change))
          context->change = NULL;
        else
          RecordMonitorError (context, 2, "FindCloseChangeNotification", GetLastError ());
        }
      if (context->stopEvent)
        {
        if (CloseHandle (context->stopEvent))
          context->stopEvent = NULL;
        else
          RecordMonitorError (context, 3, "CloseHandle(stop event)", GetLastError ());
        }
      if (context->thread)
        {
        if (CloseHandle (context->thread))
          context->thread = NULL;
        else
          RecordMonitorError (context, 4, "CloseHandle(thread)", GetLastError ());
        }
      }
    if (!reportOperation && context->pending)
      for (unsigned slot = 0; slot < 9; ++slot)
        if (context->pending & (1U << slot))
          {
          reportOperation = context->operations[slot];  // Static literal.
          reportError = context->errors[slot];
          context->pending &= ~(1U << slot);
          break;
          }
    if (!exited || context->thread || context->stopEvent || context->pending ||
        (context->change && context->change != INVALID_HANDLE_VALUE))
      {
      link = &context->next;
      continue;
      }
    *link = context->next;
    free (context->filename);
    free (context);
    }
  // A modal callback can stop/start monitors or enter this collector again.
  // All registry access is complete, and these values do not refer to a context.
  if (reportOperation)
    {
    char message[192];
    snprintf (message, sizeof message, "File monitor: %s failed (error %lu).", reportOperation, reportError);
    ::MessageBoxA (NULL, message, "File monitor error", MB_OK | MB_ICONERROR | MB_TASKMODAL);
    }
  }

__int64 CreateMonitoringThread (const char * name, __int64 document, UINT message)
  {
  CMonitorContext * context = (CMonitorContext *) calloc (1, sizeof *context);
  if (!context)
    AfxThrowResourceException ();
  context->filename = (char *) malloc (strlen (name) + 1);
  if (!context->filename)
    {
    free (context);
    AfxThrowResourceException ();
    }
  strcpy (context->filename, name);
  context->document = document;
  context->token = App.GetUniqueNumber ();
  if (!context->token)
    context->token = App.GetUniqueNumber ();
  context->window = Frame.GetSafeHwnd ();
  context->message = message;
  // Register before acquiring handles so failed startup cannot lose ownership.
  context->next = monitors;
  monitors = context;
  context->stopEvent = CreateEvent (NULL, TRUE, FALSE, NULL);
  if (!context->stopEvent)
    {
    RecordMonitorError (context, 5, "CreateEvent", GetLastError ());
    AfxThrowResourceException ();
    }
  context->thread = (HANDLE) _beginthreadex (NULL, 0, MonitorThread, context, 0, NULL);
  if (!context->thread)
    {
    RecordMonitorError (context, 5, "_beginthreadex (errno)", errno);
    AfxThrowResourceException ();
    }
  if (!SetThreadPriority (context->thread, THREAD_PRIORITY_IDLE))
    RecordMonitorError (context, 6, "SetThreadPriority", GetLastError ());
  return context->token;
  }


// ------------------- handle change to file -------------------------

void CTextDocument::OnFileChanged(void)
{
  // exit if we are already asking them the question

	if (m_bInFileChanged)
		return;

	CTextDocumentOperationGuard operationGuard (this);
	CBoolStateGuard fileChangedGuard (m_bInFileChanged, true);

	// Check if this file has changed
	CFileStatus	status;
	CFile::GetStatus(GetPathName (), status);
	if (m_timeFileMod != status.m_mtime)
	  {

    // remember time for next time

    m_timeFileMod = status.m_mtime;

		CString	strText;
    strText = TFormat ("The file \"%s\" has been modified. Do you wish to reload it?",
      (LPCTSTR) GetPathName ());
    int iAnswer = ::TMessageBox (strText, MB_YESNO | MB_ICONQUESTION);

    if (m_bClosePending)
      return;

    if (iAnswer == IDYES)
      {
		  CWaitCursor	wait;

	    try
	      {

        // Reload existing file
		    CFile	f(GetPathName (), CFile::modeRead|CFile::shareDenyWrite);
        CArchive ar(&f, CArchive::load);
	      Serialize (ar);
        ar.Close();
        SetModifiedFlag (false);
	      }
	    catch(CException* e)
  	    {
        ::TMessageBox ("Unable to read file", MB_ICONEXCLAMATION);
		    e->Delete();
	      }
      } // end of approving modification or wanting it anyway
    } // end of time changing

}


// kill the monitoring thread during the save
BOOL CTextDocument::DoSave(LPCTSTR lpszPathName, BOOL bReplace)
  {
  CTextDocumentOperationGuard operationGuard (this);
  StopMonitoringThread (m_iMonitorToken);
  m_bFileChangedPending = false;

  BOOL bResult = CDocument::DoSave (lpszPathName, bReplace);

  if (m_bClosePending)
    return bResult;

  // monitor this file again
  if (!GetPathName ().IsEmpty ())
    CreateMonitoringThread (GetPathName ());
  return bResult;
  }



void CTextDocument::CreateMonitoringThread(const char * sName)
{
  // kill any old thread
  StopMonitoringThread (m_iMonitorToken);
  m_bFileChangedPending = false;

  // find when the file was last modified

	CFileStatus	status;
	CFile::GetStatus(sName, status);
  m_timeFileMod = status.m_mtime;

  // create the thread
  m_iMonitorToken = ::CreateMonitoringThread
    (sName, m_iTextDocumentNumber, WM_USER_FILE_CONTENTS_CHANGED);

  UpdateAllViews  (NULL);     // force window title to be redrawn
}

void CTextDocument::OnFileOpen() 
{
	CTextDocumentOperationGuard operationGuard (this);

	CString title;
	VERIFY(title.LoadString(AFX_IDS_OPENFILE));

  CString fileName;

  CFileDialog dlgFile (TRUE,   // loading the file
                 "txt",        // default extension
                 "",           // suggested name
                 OFN_HIDEREADONLY | OFN_FILEMUSTEXIST,
                 "Text files (*.txt)|*.txt|"
                 "Plugins (*.xml)|*.xml|"
                 "Lua scripts (*.lua)|*.lua|"
                 "All files (*.*)|*.*||",    // filter 
                 NULL);        // parent window

	dlgFile.m_ofn.lpstrTitle = title;
	SetFileDialogFileName (dlgFile, fileName, "");
  ChangeToFileBrowsingDirectory ();
	int nResult = dlgFile.DoModal();
  ChangeToStartupDirectory ();
	fileName.ReleaseBuffer();

  if (nResult != IDOK)
    return;

  CTextDocument * pNewDoc = (CTextDocument *)
     App.m_pNormalDocTemplate->OpenDocumentFile (dlgFile.m_ofn.lpstrFile);	

  if (!pNewDoc)
    return;

  SetTheFont ();

}

void CTextDocument::SetTheFont (void)
  {
  
  // find first view
  POSITION pos = GetFirstViewPosition();

  if (!pos)
    return;

  // find view
  CTextView * pView = (CTextView *) GetNextView(pos);

  // ensure font displayed correctly
  pView->SetTheFont ();  

  }


// returns a pointer to the related world, or NULL if none

CMUSHclientDoc * CTextDocument::FindWorld (void)
  {

  if (!m_pRelatedWorld)
    return NULL;

  // make sure world still in world list
  for (POSITION docPos = App.m_pWorldDocTemplate->GetFirstDocPosition();
      docPos != NULL; )
    {

    CMUSHclientDoc * pDoc = (CMUSHclientDoc *) App.m_pWorldDocTemplate->GetNextDoc(docPos);

    // it might be the same pointer, but is it the same *world*?
    if (pDoc == m_pRelatedWorld)
      if (pDoc->m_iUniqueDocumentNumber == m_iUniqueDocumentNumber)
        return pDoc;
      else
        return NULL;    // no, unique ID number has changed
    } // end of doing each document

  return NULL;    // no longer in world list
  }

void CTextDocument::OnUpdateStatuslineMushname(CCmdUI* pCmdUI) 
{
 
  if (FindWorld ())
    {
    pCmdUI->Enable(TRUE);
    pCmdUI->SetText(m_pRelatedWorld->m_mush_name);	
	  }
  else
    {
    pCmdUI->Enable(TRUE);
    pCmdUI->SetText("(no related world)");	
    }
}


void CTextDocument::OnFileNew() 
{
CString strTitle = m_strTitle;

CMUSHclientDoc * pDoc = FindWorld ();

  // don't make another recall window
  if (!m_strSearchString.IsEmpty ())
    {
    if (pDoc)
      strTitle = TFormat ("Notepad: %s", (LPCTSTR) pDoc->m_mush_name);
    else
      strTitle = Translate ("Untitled");
    }

  CreateTextWindow ("",     // contents
                    (LPCTSTR) strTitle,     // title
                    pDoc,   // document
                    m_iUniqueDocumentNumber,      // document number
                    m_strFontName,
                    m_iFontSize,
                    m_iFontWeight,
                    m_iFontCharset,
                    m_textColour,
                    m_backColour,
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

#ifdef _DEBUG
//  ListAccelerators (this, 2);     // for documenting menus, accelerators
#endif 

}

BOOL CTextDocument::SaveModified() 
{
	CTextDocumentOperationGuard operationGuard (this);

// don't bother asking if they want to save an empty document
CTextView* pView = (CTextView*) m_viewList.GetHead();
  
	if (GetText (pView->GetEditCtrl ()).IsEmpty ())
    return TRUE;

  switch (m_iSaveOnChange)
    {
    case eNotepadSaveAlways: break;  // fall through to save if changed
    case eNotepadSaveNever: return TRUE;    // don't save
    case eNotepadSaveDefault:

      // default processing
      switch (m_iNotepadType)
        {
        case eNotepadMXPdebug:
          if (!App.m_bConfirmBeforeClosingMXPdebug)
            return TRUE;
          break;

        // these ones close without asking
        case eNotepadPacketDebug:
        case eNotepadLineInfo:
        case eNotepadWorldLoadError: 
        case eNotepadXMLcomments:
        case eNotepadPluginInfo:
        case eNotepadRecall:
          return TRUE;

        default:
          break;  // normal check
        } // end of switch on notepad type
        break;

    } // end of switch on m_iSaveOnChange


	return CDocument::SaveModified();
} // end of CTextDocument::SaveModified


void CTextDocument::OnEditConvertclipboardforumcodes() 
{
CString strContents; 

  if (!GetClipboardContents (strContents, false, false))
    return;

 putontoclipboard  (QuoteForumCodes (strContents));
	
} // end of CTextDocument::OnEditConvertclipboardforumcodes


void CTextDocument::SetReadOnly (BOOL bReadOnly)
  {
CTextView* pView = (CTextView*) m_viewList.GetHead();
  
  if (pView)
    pView->GetEditCtrl ().SetReadOnly (bReadOnly);

  m_bReadOnly = bReadOnly;
  } // end of CTextDocument::SetReadOnly
