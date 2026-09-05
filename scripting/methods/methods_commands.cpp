// methods_commands.cpp

// Command window  /  command handling

#include "stdafx.h"
#include "..\..\MUSHclient.h"
#include "..\..\doc.h"
#include "..\..\mainfrm.h"
#include "..\..\sendvw.h"
#include "..\..\childfrm.h"
#include "..\errors.h"

// Implements:

//    DeleteCommandHistory
//    DoCommand
//    Execute
//    GetCommand
//    GetCommandCursorPosition
//    GetCommandLineCount
//    GetCommandList
//    GetInternalCommandsList
//    GetQueue
//    PasteCommand
//    PushCommand
//    SelectCommand
//    SetCommand
//    SetCommandSelection
//    SetCommandWindowAutoResizeSuppressed
//    SetCommandWindowHeight
//    SetInputFont
//    ShiftTabCompleteItem

extern tCommandIDMapping CommandIDs [];

CSendView * CMUSHclientDoc::GetCommandView (void)
  {
  CSendView * pFirstCommandView = NULL;
  CSendView * pActiveCommandView = NULL;
  CSendView * pFocusedCommandView = NULL;
  HWND hwndFocus = ::GetFocus ();
  CMDIChildWnd * pActiveFrame = Frame.MDIGetActive ();

  for (POSITION pos = GetFirstViewPosition(); pos != NULL; )
    {
    CView * pView = GetNextView(pos);
    if (!pView->IsKindOf(RUNTIME_CLASS(CSendView)))
      continue;

    CSendView * pCommandView = (CSendView *) pView;
    if (pFirstCommandView == NULL)
      pFirstCommandView = pCommandView;
    // Activation callbacks run before MFC transfers keyboard focus.
    if (pCommandView == m_pActiveCommandView ||
        (m_pActiveOutputView && pCommandView->m_topview == m_pActiveOutputView))
      return pCommandView;
    if (pCommandView->GetEditCtrl().GetSafeHwnd() == hwndFocus)
      pFocusedCommandView = pCommandView;
    if (pCommandView->m_owner_frame == pActiveFrame)
      pActiveCommandView = pCommandView;
    }

  if (pFocusedCommandView)
    return pFocusedCommandView;
  return pActiveCommandView ? pActiveCommandView : pFirstCommandView;
  }   // end of CMUSHclientDoc::GetCommandView

// world.GetCommandList - returns a variant array which is a list of "count" recent commands

VARIANT CMUSHclientDoc::GetCommandList(long Count) 
{
  COleSafeArray sa;   // for command list

  CSendView * pmyView = GetCommandView ();
  if (pmyView)
    {

      POSITION pos;
      long iCount;
  
      // put the commands into the array
      if (!pmyView->m_msgList.IsEmpty ())    // cannot create empty dimension
        {
        if (Count <= 0)
          Count = pmyView->m_msgList.GetCount ();    // zero means all
        else
          Count = MIN (Count, pmyView->m_msgList.GetCount ());  // don't exceed what we have

        sa.CreateOneDim (VT_VARIANT, Count);

        for (iCount = 0, pos = pmyView->m_msgList.GetTailPosition(); 
            pos && iCount < Count; 
            )
          {
          CString strLine = pmyView->m_msgList.GetPrev (pos);

          // the array must be a bloody array of variants, or VBscript kicks up
          COleVariant v (strLine);
          sa.PutElement (&iCount, &v);
          iCount++;
          }      // end of looping through each command
        } // end of having at least one

    }

	return sa.Detach ();
}    // end of CMUSHclientDoc::GetCommandList

// world.PushCommand - pushes the command in the command window onto the command
//                     stack and then blanks it out
//  returns command that was pushed

BSTR CMUSHclientDoc::PushCommand()
{
	CString strCommand;

  CSendView * pmyView = GetCommandView ();
  if (pmyView)
    {

      // find what the command is
      pmyView->GetEditCtrl().GetWindowText (strCommand);

      // save the echo flag
      bool bSaveNoEcho = m_bNoEcho;
      m_bNoEcho = false;  // make sure we save it

      // remember it
      pmyView->AddToCommandHistory (strCommand);

      // restore the command
      m_bNoEcho = bSaveNoEcho;

      pmyView->GetEditCtrl().SetSel (0, -1);   // select all
      pmyView->GetEditCtrl().ReplaceSel ("", TRUE);   // blank it out
      pmyView->NotifyPluginCommandChanged ();

    }

	return strCommand.AllocSysString();
}   // end of CMUSHclientDoc::PushCommand

// world.SelectCommand - selects the command in the command window

void CMUSHclientDoc::SelectCommand() 
{
  CSendView * pmyView = GetCommandView ();
  if (pmyView)
    pmyView->GetEditCtrl().SetSel (0, -1);   // select all

}    // end of CMUSHclientDoc::SelectCommand

// world.GetCommand - gets the command in the command window

BSTR CMUSHclientDoc::GetCommand() 
{
	CString strCommand;

  CSendView * pmyView = GetCommandView ();
  if (pmyView)
    pmyView->GetEditCtrl().GetWindowText (strCommand);


	return strCommand.AllocSysString();
}   // end of CMUSHclientDoc::GetCommand


long CMUSHclientDoc::SetCommand(LPCTSTR Message) 
{
  CSendView * pmyView = GetCommandView ();
  if (pmyView)
    {

      CString strCurrent;

      pmyView->GetEditCtrl().GetWindowText (strCurrent);
      if (strCurrent.IsEmpty ())
        {
        pmyView->GetEditCtrl().ReplaceSel (Message, TRUE);
        pmyView->NotifyPluginCommandChanged ();
        return eOK;
        }   // end of command being empty
      else
        return eCommandNotEmpty;
    }

	return eOK;
}     // end of CMUSHclientDoc::SetCommand

BSTR CMUSHclientDoc::PasteCommand(LPCTSTR Text) 
{
CString strResult;
int nStartChar,
    nEndChar;

  CSendView * pmyView = GetCommandView ();
  if (pmyView)
    {

      CString strCurrent;

      // find the selection range
      pmyView->GetEditCtrl().GetSel(nStartChar, nEndChar);
      // get window text
      pmyView->GetEditCtrl().GetWindowText (strCurrent);
      pmyView->GetEditCtrl().ReplaceSel (Text, TRUE);
      pmyView->NotifyPluginCommandChanged ();

      // get selection if any
      if (nEndChar > nStartChar)
        strResult = strCurrent.Mid (nStartChar, nEndChar - nStartChar);
    }

	return strResult.AllocSysString();
}   // end of CMUSHclientDoc::PasteCommand


void CMUSHclientDoc::DeleteCommandHistory() 
{

  for(POSITION pos = GetFirstViewPosition(); pos != NULL; )
	  {
	  CView* pView = GetNextView(pos);
	  
	  if (pView->IsKindOf(RUNTIME_CLASS(CSendView)))
  	  {
		  CSendView* pmyView = (CSendView*)pView;

      // OK, do it ...
	    pmyView->m_msgList.RemoveAll ();
      pmyView->m_HistoryPosition = NULL;
      pmyView->m_inputcount = 0;
      pmyView->m_HistoryFindInfo.m_pFindPosition = NULL;
      pmyView->m_HistoryFindInfo.m_nCurrentLine = 0;
      pmyView->m_HistoryFindInfo.m_bAgain = FALSE;  
      pmyView->m_strPartialCommand.Empty ();
      pmyView->m_last_command.Empty ();

      }	  // end of being a CSendView
    }   // end of loop through views

}   // end of CMUSHclientDoc::DeleteCommandHistory



long CMUSHclientDoc::Execute(LPCTSTR Command) 
{

// remember current plugin
CPlugin *  pCurrentPlugin = m_CurrentPlugin;

// stop an alias from recalling itself indefinitely

if (++m_iExecutionDepth > MAX_EXECUTION_DEPTH)
  {
  m_iExecutionDepth--;
  return eCommandsNestedTooDeeply;
  }

CString strFixedCommand = Command;

// huh?  ASSERT (m_CurrentPlugin == NULL); 

m_CurrentPlugin = NULL;

// look for scripting prefix
if (!m_strScriptPrefix.IsEmpty () &&    // and we *have* a script prefix
    strFixedCommand.Left (m_strScriptPrefix.GetLength ()) == 
                          m_strScriptPrefix)  // and it matches
  {
  CString strCommand = strFixedCommand.Mid (m_strScriptPrefix.GetLength ());

  // if scripting enabled, do it
  if (m_bEnableScripts)
    {
    m_bInSendToScript = false;   // they can do DeleteLines here I think

    if (m_ScriptEngine)      // scripting might be enabled, but not OK to run
      m_ScriptEngine->Parse (strCommand, "Command line");
    else
      ColourNote ("white", "red", 
          Translate ("Scripting is not active yet, or script file had a parse error."));

    m_bInSendToScript = true;
    
    m_iExecutionDepth--;
    m_CurrentPlugin = pCurrentPlugin;  // restore whatever plugin we are in
    return eOK;
    }

  // warn if they seem to want to script but it is not enabled

  strCommand.TrimLeft ();

  if (strCommand.Left (6).CompareNoCase ("world.") == 0 ||
      strCommand.Left (8).CompareNoCase ("$world->") == 0)
   ColourNote ("white", "darkblue", 
    Translate ("Warning - you appear to be doing a script command but scripting is not enabled."));

  }      // end of having a scripting prefix


// do command stacking first, so that other things (like aliases) work within command-stack

// ---------------------- COMMAND STACKING ------------------------------

if (m_enable_command_stack && 
 !m_strCommandStackCharacter.IsEmpty ())
   {
   // new in version 3.74 - command stack character at start of line disables command stacking
   if (!strFixedCommand.IsEmpty () &&
       strFixedCommand [0] == m_strCommandStackCharacter [0])
     strFixedCommand.Delete (0, 1);   // delete command stack character itself
   else
     {
     // still want command stacking
     CString strTwoStacks =  m_strCommandStackCharacter [0];
     strTwoStacks +=  m_strCommandStackCharacter [0];
     // convert two command stacks in a row to 0x01  (eg. ;;)
     strFixedCommand = ::Replace (strFixedCommand, strTwoStacks, "\x01");
     // convert any remaining command stacks to a newline
     strFixedCommand.Replace (CString (m_strCommandStackCharacter [0]), ENDLINE);
     // replace any 0x01 with one command stack character
     strFixedCommand.Replace ('\x01', m_strCommandStackCharacter [0]);
     }
   }

CStringList strList;

// change \r\n to \n
strFixedCommand.Replace (ENDLINE, "\n");

// break up command into a list, terminated by newlines
StringToList (strFixedCommand, "\n", strList);

// if list is empty, make sure we send at least one empty line
if (strList.IsEmpty ())
  strList.AddTail (""); 

for (POSITION command_pos = strList.GetHeadPosition (); command_pos; )
  {
  CString str = strList.GetNext (command_pos);


  CString strOriginalMessage = str;

  //  TRACE1 ("SendCommand = %s\n", str);

  bool bOmitFromLog = false;

  if (!m_bPluginProcessingCommand)
      {
      m_bPluginProcessingCommand = true;  // so we don't go into a loop
      if (!SendToAllPluginCallbacks (ON_PLUGIN_COMMAND, str))
        {
        m_bPluginProcessingCommand = false;
        continue;
        }

      m_bPluginProcessingCommand = false;
      }

  // empty line - just send it

    if (str.IsEmpty ())
      {
      // pressing <enter> might be trying to connect
      if (CheckConnected ())
        {
        m_iExecutionDepth--;
        m_CurrentPlugin = pCurrentPlugin;  // restore whatever plugin we are in
        return eWorldClosed;
        }

      SendMsg (ENDLINE, m_display_my_input, false, LoggingInput ()); 
      
      continue;

      }
    
  if (EvaluateCommand (str, true, bOmitFromLog))
    break;    // error (eg. connection not open, don't keep at it)
 
  }   // end of processing each line individually

// this command has completed, so we can reduce the execution depth

  m_iExecutionDepth--;
  m_CurrentPlugin = pCurrentPlugin;  // restore whatever plugin we are in
	return eOK;
}   // end of CMUSHclientDoc::Execute


long CMUSHclientDoc::DoCommand(LPCTSTR Command) 
{

int nID = StringToCommandID (Command);

  if (nID == 0)
    return eNoSuchCommand;

  Frame.PostMessage(WM_COMMAND, nID, 0);

	return eOK;
}   // end of  CMUSHclientDoc::DoCommand


VARIANT CMUSHclientDoc::GetInternalCommandsList() 
{

  COleSafeArray sa;   // for list

  long iCount;

  // count them
	for (iCount = 0; CommandIDs [iCount].iCommandID; iCount++)
    {};   // end of counting them

  sa.CreateOneDim (VT_VARIANT, iCount);

// put the addresses into the array
	for (long i = 0; CommandIDs [i].iCommandID; i++)
    {
    COleVariant v (CommandIDs [i].sCommandName);
    sa.PutElement (&i, &v);
    }      // end of looping through each command

	return sa.Detach ();
}   // end of CMUSHclientDoc::GetInternalCommandsList


BOOL CMUSHclientDoc::GetEchoInput() 
{
	return m_display_my_input != 0;
}  // end of CMUSHclientDoc::GetEchoInput

void CMUSHclientDoc::SetEchoInput(BOOL bNewValue) 
{
  m_display_my_input = bNewValue != 0;
}  // end of CMUSHclientDoc::SetEchoInput


// world.GetQueue - returns a variant array which is a list of queued commands

VARIANT CMUSHclientDoc::GetQueue() 
{
  COleSafeArray sa;   // for command list

POSITION pos;
long iCount;

  // put the queued commands into the array
  if (!m_QueuedCommandsList.IsEmpty ())    // cannot create empty dimension
    {

    sa.CreateOneDim (VT_VARIANT, m_QueuedCommandsList.GetCount ());

    for (iCount = 0, pos = m_QueuedCommandsList.GetHeadPosition(); 
        pos; 
        )
      {
      CString strLine = m_QueuedCommandsList.GetNext (pos);

      // the array must be a bloody array of variants, or VBscript kicks up
      CString s = strLine.Mid (1);
      COleVariant v (s);  // drop echo flag
      sa.PutElement (&iCount, &v);
      iCount++;
      }      // end of looping through each command
    } // end of having at least one

	return sa.Detach ();
}   // end of CMUSHclientDoc::GetQueue


// world.GetCommandCursorPosition - gets the active end of the command selection
// Returns a 1-relative position, or zero if the active end is unavailable.

long CMUSHclientDoc::GetCommandCursorPosition()
{
  HWND hwndFocus = ::GetFocus ();
  CSendView * pCommandView = GetCommandView ();
  if (pCommandView == NULL)
    return 0;

  CEdit & edit = pCommandView->GetEditCtrl();
  int nStartChar;
  int nEndChar;
  edit.GetSel(nStartChar, nEndChar);

  int nCursorChar = nEndChar;
  if (nStartChar == nEndChar)
    nCursorChar = nStartChar;
  else
    {
    if (edit.GetSafeHwnd() != hwndFocus)
      return 0;

    CPoint cursorPoint;
    if (!::GetCaretPos(&cursorPoint))
      return 0;

    CPoint start = edit.PosFromChar(nStartChar);
    if (start.x >= 0 && start.y >= 0 &&
        abs(cursorPoint.x - start.x) <= 1 && abs(cursorPoint.y - start.y) <= 1)
      nCursorChar = nStartChar;
    }

  return nCursorChar + 1;
}   // end of CMUSHclientDoc::GetCommandCursorPosition


// world.GetCommandLineCount - gets the number of visual command lines
// Returns zero if there is no command window.

long CMUSHclientDoc::GetCommandLineCount()
{
  CSendView * pCommandView = GetCommandView ();
  if (pCommandView == NULL)
    return 0;

  return pCommandView->GetEditCtrl().GetLineCount ();
}   // end of CMUSHclientDoc::GetCommandLineCount




void CMUSHclientDoc::SetInputFont(LPCTSTR FontName, short PointSize, short Weight, BOOL Italic) 
{

Italic = Italic != 0; // make boolean

  ChangeInputFont (PointSize, 
                  FontName, 
                  Weight, 
                  DEFAULT_CHARSET,
                  Italic);

}  // end of CMUSHclientDoc::SetInputFont



// set the height of the command window
long CMUSHclientDoc::SetCommandWindowHeight(short Height) 
{
	
  if (Height < 0)
    return eBadParameter;

  CSendView * pCommandView = GetCommandView ();
  CChildFrame * pFrame = pCommandView ? pCommandView->m_owner_frame : NULL;

  // fail if we can't find one
  if (pFrame == NULL)
    return eBadParameter;

  return pFrame->SetCommandWindowHeight (Height);
}   // end of CMUSHclientDoc::SetCommandWindowHeight


// suppress automatic command-window resizing without changing the world option
long CMUSHclientDoc::SetCommandWindowAutoResizeSuppressed(BOOL Suppressed)
{
  m_bSuppressCommandWindowAutoResize = Suppressed != FALSE;
  return eOK;
}   // end of CMUSHclientDoc::SetCommandWindowAutoResizeSuppressed



// add an item for shift+tab completion
long CMUSHclientDoc::ShiftTabCompleteItem(LPCTSTR Item) 
{
  if (strlen (Item) <= 0 || strlen (Item) > 30)
    return eBadParameter;  // need a string

  if (strcmp (Item, "<clear>") == 0)
    m_ExtraShiftTabCompleteItems.clear ();
  else if (strcmp (Item, "<functions>") == 0)
    m_bTabCompleteFunctions = true;
  else if (strcmp (Item, "<nofunctions>") == 0)
    m_bTabCompleteFunctions = false;
  else
    {
    if (!IsValidName (Item))    // must be alphanumeric, dot, hyphen, underscore
      return eBadParameter;  // need a string
    m_ExtraShiftTabCompleteItems.insert (Item);
    }

	return eOK;
}    // end of CMUSHclientDoc::ShiftTabCompleteItem




long CMUSHclientDoc::SetCommandSelection(long First, long Last) 
{
  CSendView * pmyView = GetCommandView ();
  if (pmyView)
    pmyView->GetEditCtrl().SetSel(First - 1, Last, FALSE);

	return eOK;

}  // end of CMUSHclientDoc::SetCommandSelection

