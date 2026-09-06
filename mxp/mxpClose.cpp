// mxpClose.cpp - MXP close tag

// called from closing tag or automatic closure

#include "stdafx.h"
#include "..\MUSHclient.h"

#include "..\doc.h"
#include "..\MUSHview.h"
#include "..\mainfrm.h"
#include "mxp.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char BASED_CODE THIS_FILE[] = __FILE__;
#endif

#define MAX_TEXT 1000     // max size for &text; variable

bool CMUSHclientDoc::MXP_PrepareCloseTag (
  CString strTag,
  const bool bOpen,
  const __int64 iExpectedOpeningStyleCreationNumber,
  const vector<int> & closeActions,
  const CActiveTag * pActiveTag,
  CPreparedMXPClose & preparedClose)
  {
  preparedClose = CPreparedMXPClose ();
  preparedClose.strTag = strTag;
  preparedClose.closeActions = closeActions;
  preparedClose.iActiveTagCreationNumber =
    pActiveTag ? pActiveTag->nCreationNumber : 0;
  if (pActiveTag)
    {
    preparedClose.bOpeningInParagraph = pActiveTag->bOpeningInParagraph;
    preparedClose.bOpeningPreMode = pActiveTag->bOpeningPreMode;
    preparedClose.bOpeningMXPScript = pActiveTag->bOpeningMXPScript;
    preparedClose.iOpeningListMode = pActiveTag->iOpeningListMode;
    preparedClose.iOpeningListCount = pActiveTag->iOpeningListCount;
    preparedClose.iOpeningParagraphOwner =
      pActiveTag->iOpeningParagraphOwner;
    preparedClose.iOpeningPreOwner = pActiveTag->iOpeningPreOwner;
    preparedClose.iOpeningScriptOwner = pActiveTag->iOpeningScriptOwner;
    preparedClose.iOpeningListOwner = pActiveTag->iOpeningListOwner;
    }

  POSITION linepos = 0, 
           stylepos, 
           oldstylepos = 0,
           oldlinepos = 0;
  CStyle * pStyle = NULL;
  CLine * pLine = NULL;
  bool bFoundit = false;
  CString strFoundVariable;

  // work backwards through the lines
  for (linepos = m_LineList.GetTailPosition (); linepos && !bFoundit; )
    {
    oldlinepos = linepos;   // for going forwards again
    pLine = m_LineList.GetPrev (linepos);

    // work backwards through the styles on the line
    for (stylepos = pLine->styleList.GetTailPosition (); stylepos && !bFoundit; )
      {
      oldstylepos = stylepos;   // where we found it
      pStyle = pLine->styleList.GetPrev (stylepos);

      if ((pStyle->iFlags & START_TAG) && pStyle->pAction &&
          (!iExpectedOpeningStyleCreationNumber ||
           pStyle->nCreationNumber == iExpectedOpeningStyleCreationNumber))
        if (pStyle->pAction->m_strAction == strTag)
          bFoundit = true;

      // this seems whacky, but it seems the variable name is on a separate
      // style to the <var> tag itself - try to remember what variable to set
      if (pStyle->pAction && !pStyle->pAction->m_strVariable.IsEmpty ())
        strFoundVariable = pStyle->pAction->m_strVariable;

      }   // end of style loop
    } // end of line loop

  bool bOpeningMarkerMissing = !bFoundit && pActiveTag;

  if (!bFoundit && !bOpeningMarkerMissing)
    {
    MXP_error (DBG_WARNING, wrnMXP_OpenTagNotInOutputBuffer,
              TFormat ("Opening MXP tag <%s> not found in output buffer", 
              (LPCTSTR) strTag)); 
    return false;
    }

  if (bOpeningMarkerMissing)
    {
    oldlinepos = NULL;
    oldstylepos = NULL;
    pLine = NULL;
    pStyle = NULL;

    // A destructive tail change publishes an exact replacement boundary.
    // Find that range first because a reused older line can contain new text.
    if (pActiveTag->nFallbackStyleRangeNumber)
      for (POSITION candidateLinePosition = m_LineList.GetHeadPosition ();
           candidateLinePosition && !oldstylepos; )
        {
        POSITION currentLinePosition = candidateLinePosition;
        CLine * pCandidateLine = m_LineList.GetNext (candidateLinePosition);
        for (POSITION candidateStylePosition =
               pCandidateLine->styleList.GetHeadPosition ();
             candidateStylePosition; )
          {
          POSITION currentStylePosition = candidateStylePosition;
          CStyle * pCandidateStyle =
            pCandidateLine->styleList.GetNext (candidateStylePosition);
          if (pCandidateLine->nCreationNumber <
                pActiveTag->nFallbackLineCreationNumber ||
              pCandidateStyle->nRangeCreationNumber !=
              pActiveTag->nFallbackStyleRangeNumber)
            continue;

          oldlinepos = currentLinePosition;
          oldstylepos = currentStylePosition;
          pLine = pCandidateLine;
          break;
          }
        }

    // Front pruning does not need a replacement boundary. Use only lines and
    // styles that were created after the original opening point.
    for (POSITION candidateLinePosition = m_LineList.GetHeadPosition ();
         candidateLinePosition && !oldstylepos; )
      {
      POSITION currentLinePosition = candidateLinePosition;
      CLine * pCandidateLine = m_LineList.GetNext (candidateLinePosition);

      if (pCandidateLine->nCreationNumber <
          pActiveTag->nOpeningLineCreationNumber)
        continue;

      POSITION candidateStylePosition =
        pCandidateLine->styleList.GetHeadPosition ();

      if (pCandidateLine->nCreationNumber ==
          pActiveTag->nOpeningLineCreationNumber)
        {
        while (candidateStylePosition)
          {
          POSITION currentStylePosition = candidateStylePosition;
          CStyle * pCandidateStyle =
            pCandidateLine->styleList.GetNext (candidateStylePosition);
          if (pCandidateStyle->nCreationNumber >
              pActiveTag->nOpeningStyleCreationNumber)
            {
            oldstylepos = currentStylePosition;
            break;
            }
          }
        }

      if (!oldstylepos &&
          pCandidateLine->nCreationNumber >
            pActiveTag->nOpeningLineCreationNumber)
        oldstylepos = pCandidateLine->styleList.GetHeadPosition ();

      if (oldstylepos)
        {
        oldlinepos = currentLinePosition;
        pLine = pCandidateLine;
        break;
        }
      }
    }

CString & strVariable = preparedClose.strVariable;
bool & bHaveVariable = preparedClose.bHaveVariable;
strVariable = "mxp_";

   if (bOpeningMarkerMissing && !pActiveTag->strVariable.IsEmpty ())
     {
     bHaveVariable = true;
     strVariable += pActiveTag->strVariable;
     }
   else if (!bOpeningMarkerMissing && pStyle->pAction &&
     !pStyle->pAction->m_strVariable.IsEmpty ())
     {
     bHaveVariable = true;
     strVariable += pStyle->pAction->m_strVariable;
     }
   else if (!bOpeningMarkerMissing &&
            ((strTag == "var") || (strTag == "v")) &&
            !strFoundVariable.IsEmpty ())
     {
     // <var>blah</var> is a bit different
     bHaveVariable = true;
     strVariable += strFoundVariable;
     }
 
  // establish text of characters between start tag and end of buffer

  CString & strText = preparedClose.strText;
  bool bStart = false;
  set<__int64> & contentStyleRangeNumbers =
    preparedClose.contentStyleRangeNumbers;
  int iBytesToGo = MAX_TEXT;
  char * p = strText.GetBuffer (MAX_TEXT);

  for (linepos = oldlinepos; linepos; )
    {
    POSITION currentLinePosition = linepos;
    CLine * pLine2 = m_LineList.GetNext (linepos);
    int iCol = 0;
    int iLen = 0;
 
    for (stylepos = pLine2->styleList.GetHeadPosition () ; stylepos; )
      {
      POSITION currentStylePosition = stylepos;
      CStyle * pStyle2 = pLine2->styleList.GetNext (stylepos);
      if ((!bOpeningMarkerMissing && pStyle2 == pStyle) ||
          (bOpeningMarkerMissing &&
           currentLinePosition == oldlinepos &&
           currentStylePosition == oldstylepos))
        bStart = true;
      if (bStart)
        {
        iLen += pStyle2->iLength; // count length
        contentStyleRangeNumbers.insert (pStyle2->nRangeCreationNumber);
        }
      else
        iCol += pStyle2->iLength; // starting column
      }   // end of each style

    if (iBytesToGo <= 2)
      continue;

    int iCopy = MIN (iLen, iBytesToGo);

    // copy line text
    
    memcpy (p, &pLine2->text [iCol], iCopy);
    p += iCopy;
    iBytesToGo -= iCopy;
    
//    strText += CString (pLine2->text, pLine2->len).Mid (iCol, iLen);

    // include newlines on intermediate lines
    if (linepos && pLine2->hard_return && iBytesToGo >= 2)
      {
      memcpy (p, ENDLINE, 2);
      p += 2;
      iBytesToGo -= 2;
      }

    }   // end of each line

  strText.ReleaseBuffer (MAX_TEXT - iBytesToGo);

  if (!bOpeningMarkerMissing)
    contentStyleRangeNumbers.erase (pStyle->nRangeCreationNumber);

  // Publish the restored style before any callback. A callback can open a new
  // tag, and that tag must inherit the style outside the tag being closed.
  CStyle * pRestoredStyle = NULL;

  if (bOpeningMarkerMissing)
    {
    pRestoredStyle = AddStyle (pActiveTag->iOpeningFlags,
                               pActiveTag->iOpeningForeColour,
                               pActiveTag->iOpeningBackColour,
                               0,
                               pActiveTag->pOpeningAction);
    }
  else
    {
    // The opening marker remembers the style that must be restored.
    CStyle * pLastStyle = m_pCurrentLine->styleList.GetTail ();
    POSITION lastStylePosition =
      m_pCurrentLine->styleList.GetTailPosition ();

    m_pCurrentLine->styleList.AddTail (pStyle);
    pLine->styleList.RemoveAt (oldstylepos);

    if (pLastStyle->iLength == 0 &&
        (pLastStyle->iFlags & START_TAG) == 0)
      {
      m_pCurrentLine->styleList.RemoveAt (lastStylePosition);
      DELETESTYLE (pLastStyle);
      }

    pStyle->iFlags &= ~START_TAG;
    CAction * pTagAction = pStyle->pAction;
    pStyle->pAction = pActiveTag ? pActiveTag->pOpeningAction : NULL;
    if (pStyle->pAction)
      pStyle->pAction->AddRef ();
    if (pTagAction)
      pTagAction->Release ();

    pRestoredStyle = pStyle;
    }

  RememberStyle (pRestoredStyle);

  return true;
  } // end of CMUSHclientDoc::MXP_PrepareCloseTag

void CMUSHclientDoc::MXP_FinishCloseTag (
  const CPreparedMXPClose & preparedClose)
  {
  CPluginContextGuard pluginContextGuard (this, NULL);

  const CString & strTag = preparedClose.strTag;
  const CString & strText = preparedClose.strText;
  const CString & strVariable = preparedClose.strVariable;
  const bool bHaveVariable = preparedClose.bHaveVariable;

  exception_ptr pendingException;
  try
    {

  // call script if required
  if (m_dispidOnMXP_CloseTag != DISPID_UNKNOWN)
    {
    long nInvocationCount = 0;

    CString strType = "MXP close tag";
    CString strReason =  TFormat ("closing MXP tag %s", (LPCTSTR) strTag);

    if (GetScriptEngine () && GetScriptEngine ()->IsLua ())
      {
      list<double> nparams;
      list<string> sparams;
      sparams.push_back ((LPCTSTR) strTag);
      sparams.push_back ((LPCTSTR) strText);
      GetScriptEngine ()->ExecuteLua (m_dispidOnMXP_CloseTag, 
                                     m_strOnMXP_CloseTag, 
                                     eWorldAction,
                                     strType, 
                                     strReason, 
                                     nparams,
                                     sparams, 
                                     nInvocationCount); 
      }   // end of Lua
    else
      {
      // WARNING - arguments should appear in REVERSE order to what the sub expects them!

      enum
        {
        eText,
        eTagName,
        eArgCount,     // this MUST be last
        };    

      COleVariant args [eArgCount];
      DISPPARAMS params = { args, NULL, eArgCount, 0 };

      args [eTagName] = strTag;
      args [eText] = strText;

      ExecuteScript (m_dispidOnMXP_CloseTag,  
                     m_strOnMXP_CloseTag,
                     eWorldAction,
                     strType, 
                     strReason,
                     params, 
                     nInvocationCount); 
      } // not Lua
    }  // end of script callback wanted

  if (m_bPluginProcessesCloseTag)
    SendToAllPluginCallbacks (ON_PLUGIN_MXP_CLOSETAG, 
                              CFormat ("%s,%s",
                              (LPCTSTR) strTag,
                              (LPCTSTR) strText));

  // if this tag had a FLAG directive, set the desired variable - prefixed with mxp_
  if (bHaveVariable)
    {
    CVariable * old_variable_item = NULL;
    m_VariableMap.Lookup (strVariable, old_variable_item);

    CVariable * variable_item = new CVariable;
    try
      {
      variable_item->nUpdateNumber = App.GetUniqueNumber ();   // for concurrency checks
      variable_item->strLabel = strVariable;
      variable_item->strContents = strText;
      m_VariableMap.SetAt (strVariable, variable_item);
      }
    catch (...)
      {
      delete variable_item;
      throw;
      }

    delete old_variable_item;
    m_bVariablesChanged = true;
//    SetModifiedFlag (TRUE);

    // if auto-mapping, add to auto-map string

    if (m_bMapping && 
        strVariable == "mxp_roomname" &&
        !strText.IsEmpty ())
      {
      CString str = "{";
      CString strTemp = strText;
      strTemp.Replace ('{', '(');  // ensure no comment delimiters in room name
      strTemp.Replace ('}', ')');
      str += strTemp;
      str += "}";

      m_strMapList.AddTail (str);
	    
      // update status line
      DrawMappingStatusLine ();
      }

    // call script if required
    if (m_dispidOnMXP_SetVariable != DISPID_UNKNOWN)
      {
      long nInvocationCount = 0;

      CString strType = "MXP set variable";
      CString strReason =  TFormat ("setting MXP variable %s", (LPCTSTR) strVariable);

      if (GetScriptEngine () && GetScriptEngine ()->IsLua ())
        {
        list<double> nparams;
        list<string> sparams;
        sparams.push_back ((LPCTSTR) strVariable);
        sparams.push_back ((LPCTSTR) strText);
        GetScriptEngine ()->ExecuteLua (m_dispidOnMXP_SetVariable, 
                                       m_strOnMXP_SetVariable, 
                                       eWorldAction,
                                       strType, 
                                       strReason, 
                                       nparams,
                                       sparams, 
                                       nInvocationCount); 
        }   // end of Lua
      else
        {
        // WARNING - arguments should appear in REVERSE order to what the sub expects them!

        enum
          {
          eContents,
          eVariableName,
          eArgCount,     // this MUST be last
          };    

        COleVariant args [eArgCount];
        DISPPARAMS params = { args, NULL, eArgCount, 0 };

        args [eVariableName] = strVariable;
        args [eContents] = strText;

        ExecuteScript (m_dispidOnMXP_SetVariable,  
                       m_strOnMXP_SetVariable,
                       eWorldAction,
                       strType, 
                       strReason,
                       params, 
                       nInvocationCount); 
        } // not Lua
      }  // end of script callback wanted


    // tell each plugin what we have received
    if (m_bPluginProcessesSetVariable)
      SendToAllPluginCallbacks (ON_PLUGIN_MXP_SETVARIABLE, 
                              CFormat ("%s=%s",
                              (LPCTSTR) strVariable,
                              (LPCTSTR) strText));

    if (strTag == "var"|| strTag == "v") // add entity to map
      {
      m_CustomEntityMap.SetAt (strVariable, strText);

      // tell each plugin what we have received
      if (m_bPluginProcessesSetEntity)
        SendToAllPluginCallbacks (ON_PLUGIN_MXP_SETENTITY, 
                                CFormat ("%s=%s",
                                (LPCTSTR) strVariable,
                                (LPCTSTR) strText));

      }

    } // end of setting the variable's contents

    }
  catch (...)
    {
    pendingException = current_exception ();
    }

  // Keep the original callback-before-close-action order. Range actions use
  // the saved logical identities, so callback-created output is not included.
  for (vector<int>::const_iterator it = preparedClose.closeActions.begin ();
       it != preparedClose.closeActions.end (); ++it)
    try
      {
      MXP_CloseAtomicTag (*it, preparedClose);
      }
    catch (...)
      {
      if (!pendingException)
        pendingException = current_exception ();
      }

  if (pendingException)
    rethrow_exception (pendingException);

  } // end of CMUSHclientDoc::MXP_FinishCloseTag



void CMUSHclientDoc::MXP_CloseOpenTags (void)
  {
  struct CTagToClose
    {
    __int64 iCreationNumber;
    __int64 iOpeningStyleCreationNumber;
    CString strName;
    vector<int> closeActions;
    };
  vector<CTagToClose> tagsToClose;

  for (POSITION pos = m_ActiveTagList.GetTailPosition (); pos; )
    {
    CActiveTag * pTag = m_ActiveTagList.GetPrev (pos);
    if (pTag->bSecure)
      break;

    CTagToClose tagToClose;
    tagToClose.iCreationNumber = pTag->nCreationNumber;
    tagToClose.iOpeningStyleCreationNumber =
      pTag->nOpeningStyleCreationNumber;
    tagToClose.strName = pTag->strName;
    tagToClose.closeActions = pTag->closeActions;
    tagsToClose.push_back (tagToClose);
    }

  vector<CPreparedMXPClose> preparedCloses;
  preparedCloses.reserve (tagsToClose.size ());
  exception_ptr prepareException;

  for (vector<CTagToClose>::const_iterator it = tagsToClose.begin ();
       it != tagsToClose.end (); ++it)
    {
    CActiveTag * pTag = NULL;
    POSITION tagPosition = NULL;
    for (POSITION pos = m_ActiveTagList.GetTailPosition (); pos; )
      {
      POSITION current = pos;
      CActiveTag * pCandidate = m_ActiveTagList.GetPrev (pos);
      if (pCandidate->nCreationNumber == it->iCreationNumber)
        {
        pTag = pCandidate;
        tagPosition = current;
        break;
        }
      }

    if (!pTag)
      continue;

    bool bCloseSlotAdded = false;
    bool bPrepared = false;
    try
      {
      preparedCloses.push_back (CPreparedMXPClose ());
      bCloseSlotAdded = true;
      bPrepared = MXP_PrepareCloseTag (it->strName,
                                       false,
                                       it->iOpeningStyleCreationNumber,
                                       it->closeActions,
                                       pTag,
                                       preparedCloses.back ());
      }
    catch (...)
      {
      if (bCloseSlotAdded)
        preparedCloses.pop_back ();
      prepareException = current_exception ();
      break;
      }

    if (!bPrepared)
      {
      preparedCloses.pop_back ();
      continue;
      }

    m_ActiveTagList.RemoveAt (tagPosition);
    delete pTag;
    }

  exception_ptr finishException;
  for (vector<CPreparedMXPClose>::const_iterator it = preparedCloses.begin ();
       it != preparedCloses.end (); ++it)
    {
    try
      {
      MXP_error (DBG_WARNING, wrnMXP_OpenTagClosedAtEndOfLine,
                TFormat ("End-of-line closure of open MXP tag: <%s>",
                         (LPCTSTR) it->strTag));
      }
    catch (...)
      {
      if (!finishException)
        finishException = current_exception ();
      }

    try
      {
      MXP_FinishCloseTag (*it);
      }
    catch (...)
      {
      if (!finishException)
        finishException = current_exception ();
      }
    }

  if (prepareException)
    rethrow_exception (prepareException);
  if (finishException)
    rethrow_exception (finishException);
  }  // end of CMUSHclientDoc::MXP_CloseOpenTags

void CMUSHclientDoc::MXP_CloseAllTags (void)
  {
  struct CTagToClose
    {
    __int64 iCreationNumber;
    __int64 iOpeningStyleCreationNumber;
    CString strName;
    vector<int> closeActions;
    };
  vector<CTagToClose> tagsToClose;

  for (POSITION pos = m_ActiveTagList.GetTailPosition (); pos; )
    {
    CActiveTag * pTag = m_ActiveTagList.GetPrev (pos);
    if (pTag->bNoReset)
      break;

    CTagToClose tagToClose;
    tagToClose.iCreationNumber = pTag->nCreationNumber;
    tagToClose.iOpeningStyleCreationNumber =
      pTag->nOpeningStyleCreationNumber;
    tagToClose.strName = pTag->strName;
    tagToClose.closeActions = pTag->closeActions;
    tagsToClose.push_back (tagToClose);
    }

  vector<CPreparedMXPClose> preparedCloses;
  preparedCloses.reserve (tagsToClose.size ());
  exception_ptr prepareException;

  for (vector<CTagToClose>::const_iterator it = tagsToClose.begin ();
       it != tagsToClose.end (); ++it)
    {
    CActiveTag * pTag = NULL;
    POSITION tagPosition = NULL;
    for (POSITION pos = m_ActiveTagList.GetTailPosition (); pos; )
      {
      POSITION current = pos;
      CActiveTag * pCandidate = m_ActiveTagList.GetPrev (pos);
      if (pCandidate->nCreationNumber == it->iCreationNumber)
        {
        pTag = pCandidate;
        tagPosition = current;
        break;
        }
      }

    if (!pTag)
      continue;

    bool bCloseSlotAdded = false;
    bool bPrepared = false;
    try
      {
      preparedCloses.push_back (CPreparedMXPClose ());
      bCloseSlotAdded = true;
      bPrepared = MXP_PrepareCloseTag (it->strName,
                                       false,
                                       it->iOpeningStyleCreationNumber,
                                       it->closeActions,
                                       pTag,
                                       preparedCloses.back ());
      }
    catch (...)
      {
      if (bCloseSlotAdded)
        preparedCloses.pop_back ();
      prepareException = current_exception ();
      break;
      }

    if (!bPrepared)
      {
      preparedCloses.pop_back ();
      continue;
      }

    m_ActiveTagList.RemoveAt (tagPosition);
    delete pTag;
    }

  exception_ptr finishException;
  for (vector<CPreparedMXPClose>::const_iterator it = preparedCloses.begin ();
       it != preparedCloses.end (); ++it)
    {
    try
      {
      MXP_error (DBG_WARNING, wrnMXP_TagClosedAtReset,
                TFormat ("<reset> closure of MXP tag: <%s>",
                         (LPCTSTR) it->strTag));
      }
    catch (...)
      {
      if (!finishException)
        finishException = current_exception ();
      }

    try
      {
      MXP_FinishCloseTag (*it);
      }
    catch (...)
      {
      if (!finishException)
        finishException = current_exception ();
      }
    }

  if (prepareException)
    rethrow_exception (prepareException);
  if (finishException)
    rethrow_exception (finishException);
  } // end of CMUSHclientDoc::MXP_CloseAllTags

