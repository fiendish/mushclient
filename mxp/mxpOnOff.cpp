// mxpOnOff.cpp - turning MXP on and off

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

static CStyle * FindMXPStyle (CLineList & lines,
                              const __int64 iCreationNumber)
  {
  for (POSITION linepos = lines.GetHeadPosition (); linepos; )
    {
    CLine * pLine = lines.GetNext (linepos);
    for (POSITION stylepos = pLine->styleList.GetHeadPosition (); stylepos; )
      {
      CStyle * pStyle = pLine->styleList.GetNext (stylepos);
      if (pStyle->nCreationNumber == iCreationNumber)
        return pStyle;
      }
    }
  return NULL;
  }

static void DiscardMXPTags (CMUSHclientDoc * pDoc,
                            const set<__int64> & identities)
  {
  for (POSITION pos = pDoc->m_ActiveTagList.GetHeadPosition (); pos; )
    {
    POSITION current = pos;
    CActiveTag * pTag = pDoc->m_ActiveTagList.GetNext (pos);
    if (identities.find (pTag->nCreationNumber) == identities.end ())
      continue;

    CStyle * pMarker =
      FindMXPStyle (pDoc->m_LineList, pTag->nOpeningStyleCreationNumber);
    if (pMarker && (pMarker->iFlags & START_TAG))
      {
      CAction * pReplacementAction = pTag->pOpeningAction;
      if (pReplacementAction)
        pReplacementAction->AddRef ();
      if (pMarker->pAction)
        pMarker->pAction->Release ();
      pMarker->iFlags = pTag->iOpeningFlags;
      pMarker->iForeColour = pTag->iOpeningForeColour;
      pMarker->iBackColour = pTag->iOpeningBackColour;
      pMarker->pAction = pReplacementAction;
      }

    pDoc->m_ActiveTagList.RemoveAt (current);
    delete pTag;
    }
  }

static void DiscardMXPTagsCreatedAfter (CMUSHclientDoc * pDoc,
                                        const __int64 iBoundary)
  {
  for (POSITION pos = pDoc->m_ActiveTagList.GetHeadPosition (); pos; )
    {
    POSITION current = pos;
    CActiveTag * pTag = pDoc->m_ActiveTagList.GetNext (pos);
    if (pTag->nCreationNumber <= iBoundary)
      continue;

    CStyle * pMarker =
      FindMXPStyle (pDoc->m_LineList, pTag->nOpeningStyleCreationNumber);
    if (pMarker && (pMarker->iFlags & START_TAG))
      {
      CAction * pReplacementAction = pTag->pOpeningAction;
      if (pReplacementAction)
        pReplacementAction->AddRef ();
      if (pMarker->pAction)
        pMarker->pAction->Release ();
      pMarker->iFlags = pTag->iOpeningFlags;
      pMarker->iForeColour = pTag->iOpeningForeColour;
      pMarker->iBackColour = pTag->iOpeningBackColour;
      pMarker->pAction = pReplacementAction;
      }

    pDoc->m_ActiveTagList.RemoveAt (current);
    delete pTag;
    }
  }

static void RebaseMXPResetState (CMUSHclientDoc * pDoc,
                                 const __int64 iBoundary)
  {
  for (POSITION pos = pDoc->m_ActiveTagList.GetHeadPosition (); pos; )
    {
    CActiveTag * pTag = pDoc->m_ActiveTagList.GetNext (pos);
    if (pTag->nCreationNumber <= iBoundary)
      continue;

    if (pTag->iOpeningParagraphOwner < iBoundary)
      {
      pTag->bOpeningInParagraph = false;
      pTag->iOpeningParagraphOwner = 0;
      }
    if (pTag->iOpeningPreOwner < iBoundary)
      {
      pTag->bOpeningPreMode = false;
      pTag->iOpeningPreOwner = 0;
      }
    if (pTag->iOpeningScriptOwner < iBoundary)
      {
      pTag->bOpeningMXPScript = false;
      pTag->iOpeningScriptOwner = 0;
      }
    if (pTag->iOpeningListOwner < iBoundary)
      {
      pTag->iOpeningListMode = eNoList;
      pTag->iOpeningListCount = 0;
      pTag->iOpeningListOwner = 0;
      }
    }
  }

static void PublishMXPResetOpeningStyle (CMUSHclientDoc * pDoc,
                                         const unsigned short iFlags,
                                         const COLORREF iForeColour,
                                         const COLORREF iBackColour,
                                         CAction * pAction)
  {
  CStyle * pStyle = pDoc->m_pCurrentLine->styleList.GetTail ();
  if (pStyle->iLength == 0 && (pStyle->iFlags & START_TAG) == 0)
    {
    if (pAction)
      pAction->AddRef ();
    if (pStyle->pAction)
      pStyle->pAction->Release ();
    pStyle->iFlags = iFlags;
    pStyle->iForeColour = iForeColour;
    pStyle->iBackColour = iBackColour;
    pStyle->pAction = pAction;
    }
  else
    pStyle = pDoc->AddStyle (iFlags,
                             iForeColour,
                             iBackColour,
                             0,
                             pAction);
  pDoc->RememberStyle (pStyle);
  }


// this is for an MXP reset or MXP off
// if bCompletely is true then we are turning MXP right off, otherwise
// we are resetting to normal behaviour
void CMUSHclientDoc::MXP_Off (const bool bCompletely)
  {

  // MXP off still resets ANSI state when MXP is already disabled.
  if (!m_bMXP)
    {
    if (m_pCurrentLine)
      InterpretANSIcode (0);
    return;
    }

  // Build every allocation-backed reset snapshot before changing ANSI state,
  // generation, status, or active tags.
  const __int64 iResetBoundary = App.GetUniqueNumber ();
  set<__int64> resetTargets;
  for (POSITION pos = m_ActiveTagList.GetTailPosition (); pos; )
    {
    CActiveTag * pTag = m_ActiveTagList.GetPrev (pos);
    if (pTag->bNoReset)
      break;
    resetTargets.insert (pTag->nCreationNumber);
    }
  bool bHaveResetOpeningStyle = false;
  unsigned short iResetOpeningFlags = 0;
  COLORREF iResetOpeningForeColour = 0;
  COLORREF iResetOpeningBackColour = 0;
  CAction * pResetOpeningAction = NULL;
  for (POSITION pos = m_ActiveTagList.GetHeadPosition (); pos; )
    {
    CActiveTag * pTag = m_ActiveTagList.GetNext (pos);
    if (resetTargets.find (pTag->nCreationNumber) == resetTargets.end ())
      continue;
    bHaveResetOpeningStyle = true;
    iResetOpeningFlags = pTag->iOpeningFlags;
    iResetOpeningForeColour = pTag->iOpeningForeColour;
    iResetOpeningBackColour = pTag->iOpeningBackColour;
    pResetOpeningAction = pTag->pOpeningAction;
    if (pResetOpeningAction)
      pResetOpeningAction->AddRef ();
    break;
    }

  exception_ptr pendingException;
  if (m_pCurrentLine)          // provided we have a line yet :)
    {
    try
      {
      InterpretANSIcode (0);    // go back to white on black
      }
    catch (...)
      {
      pendingException = current_exception ();
      }
    }

  m_iMXPGeneration++;

  if (bCompletely)
    try
      {
      Frame.SetStatusMessageNow (Translate ("Closing down MXP"));
      }
    catch (...)
      {
      if (!pendingException)
        pendingException = current_exception ();
      }

  bool bCloseFailed = false;
  try
    {
    MXP_CloseAllTags ();
    }
  catch (...)
    {
    if (!pendingException)
      pendingException = current_exception ();
    bCloseFailed = true;
    }

  bool bResetTargetsRemain = false;
  if (bCloseFailed)
    for (POSITION pos = m_ActiveTagList.GetHeadPosition (); pos; )
      if (resetTargets.find (
            m_ActiveTagList.GetNext (pos)->nCreationNumber) !=
          resetTargets.end ())
        {
        bResetTargetsRemain = true;
        break;
        }

  if (bCloseFailed)
    {
    DiscardMXPTags (this, resetTargets);
    if (bResetTargetsRemain &&
        !bCompletely && bHaveResetOpeningStyle && m_pCurrentLine)
      try
        {
        PublishMXPResetOpeningStyle (this,
                                     iResetOpeningFlags,
                                     iResetOpeningForeColour,
                                     iResetOpeningBackColour,
                                     pResetOpeningAction);
        }
      catch (...)
        {
        if (!pendingException)
          pendingException = current_exception ();
        }
    }

  if (pResetOpeningAction)
    pResetOpeningAction->Release ();

  RebaseMXPResetState (this, iResetBoundary);

  if (bCompletely || m_iMXPParagraphOwner < iResetBoundary)
    {
    m_bInParagraph = false;
    m_iMXPParagraphOwner = 0;
    }
  if (bCompletely || m_iMXPScriptOwner < iResetBoundary)
    {
    m_bMXP_script = false;    // cancel scripts
    m_iMXPScriptOwner = 0;
    }
  if (bCompletely || m_iMXPPreOwner < iResetBoundary)
    {
    m_bPreMode = false;       // no more preformatted text
    m_iMXPPreOwner = 0;
    }
  if (bCompletely || m_iMXPListOwner < iResetBoundary)
    {
    m_iListMode = eNoList;    // no more ordered/unordered lists
    m_iListCount = 0;
    m_iMXPListOwner = 0;
    }

  try
    {
    MXP_error (DBG_INFO, infoMXP_ResetReceived, "MXP reset.");
    }
  catch (...)
    {
    if (!pendingException)
      pendingException = current_exception ();
    }

  if (bCompletely)
    {
    try
      {
      MXP_mode_change (eMXP_open);  // back to open mode
      }
    catch (...)
      {
      if (!pendingException)
        pendingException = current_exception ();
      m_iMXP_defaultMode = eMXP_open;
      m_iMXP_mode = eMXP_open;
      }

    // if not using MXP (any more) then turn collection phase off
    if (
       m_phase == HAVE_MXP_ELEMENT ||
       m_phase == HAVE_MXP_COMMENT ||
       m_phase == HAVE_MXP_QUOTE ||
       m_phase == HAVE_MXP_ENTITY ||
       m_phase == HAVE_MXP_ROOM_NAME ||
       m_phase == HAVE_MXP_ROOM_DESCRIPTION ||
       m_phase == HAVE_MXP_ROOM_EXITS ||
       m_phase == HAVE_MXP_WELCOME
       )
      m_phase = NONE;
    try
      {
      if (m_bPuebloActive)
        MXP_error (DBG_INFO, infoMXP_off, "Pueblo turned off.");
      else
        MXP_error (DBG_INFO, infoMXP_off, "MXP turned off.");
      }
    catch (...)
      {
      if (!pendingException)
        pendingException = current_exception ();
      }
    m_bPuebloActive = false;
    m_bMXP = false;

    DiscardMXPTagsCreatedAfter (this, iResetBoundary);
    m_bInParagraph = false;
    m_bMXP_script = false;
    m_bPreMode = false;
    m_iListMode = eNoList;
    m_iListCount = 0;
    m_iMXPParagraphOwner = 0;
    m_iMXPPreOwner = 0;
    m_iMXPScriptOwner = 0;
    m_iMXPListOwner = 0;
    if (m_pCurrentLine)
      {
      try
        {
        InterpretANSIcode (0);
        }
      catch (...)
        {
        if (!pendingException)
          pendingException = current_exception ();
        }
      }

    // execute "close" script
    if (m_dispidOnMXP_Stop != DISPID_UNKNOWN)
      {
      if (SeeIfHandlerCanExecute (m_strOnMXP_Stop))
        {
        DISPPARAMS params = { NULL, NULL, 0, 0 };
        long nInvocationCount = 0;

        try
          {
          ExecuteScript (m_dispidOnMXP_Stop,
                         m_strOnMXP_Stop,
                         eWorldAction,
                         "MXP shutdown",
                         "stopping MXP",
                         params,
                         nInvocationCount);
          }
        catch (...)
          {
          if (!pendingException)
            pendingException = current_exception ();
          }
        }
      } // end of executing close script
    try
      {
      SendToAllPluginCallbacks (ON_PLUGIN_MXP_STOP);
      }
    catch (...)
      {
      if (!pendingException)
        pendingException = current_exception ();
      }

    } // end of turn MXP off completely

  if (pendingException)
    rethrow_exception (pendingException);
  }  // end of CMUSHclientDoc::MXP_Off


void CMUSHclientDoc::MXP_On (const bool bPueblo, const bool bManual)
 {

  // do nothing if already on
  if (m_bMXP)
    return;

  m_iMXPGeneration++;
  const __int64 iStartingGeneration = m_iMXPGeneration;

  m_bMXP_script = false;
  m_bPreMode = false;
  m_iLastOutstandingTagCount = 0;
  m_iMXPerrors = 0;
  m_iMXPtags = 0;
  m_iMXPentities = 0;
  m_iListMode = eNoList;
  m_iListCount = 0;
  m_iMXPParagraphOwner = 0;
  m_iMXPPreOwner = 0;
  m_iMXPScriptOwner = 0;
  m_iMXPListOwner = 0;

  if (!bManual)
    {
    m_iMXP_defaultMode = m_iMXP_mode = eMXP_open;
    DELETE_MAP (m_CustomElementMap, CElement);
    DELETE_LIST (m_ActiveTagList);
    m_CustomEntityMap.RemoveAll ();
    }

  const __int64 iStartBoundary = App.GetUniqueNumber ();
  m_bMXP = true;
  m_bPuebloActive = bPueblo;

  try
    {

  if (bPueblo)
    MXP_error (DBG_INFO, infoMXP_on, "Pueblo turned on.");
  else
    MXP_error (DBG_INFO, infoMXP_on, "MXP turned on.");

  if (m_iMXPGeneration != iStartingGeneration || !m_bMXP)
    return;

  // execute "open" script
  if (m_dispidOnMXP_Start != DISPID_UNKNOWN)
    {
    if (SeeIfHandlerCanExecute (m_strOnMXP_Start))
      {

      DISPPARAMS params = { NULL, NULL, 0, 0 };
      long nInvocationCount = 0;

      ExecuteScript (m_dispidOnMXP_Start,  
                   m_strOnMXP_Start,
                   eWorldAction,
                   "MXP startup", 
                   "starting MXP",
                   params, 
                   nInvocationCount); 
        }
    } // end of executing open script

  if (m_iMXPGeneration != iStartingGeneration || !m_bMXP)
    return;

 SendToAllPluginCallbacks (ON_PLUGIN_MXP_START);

  if (m_iMXPGeneration != iStartingGeneration || !m_bMXP)
    return;
    }
  catch (...)
    {
    if (m_iMXPGeneration == iStartingGeneration)
      {
      m_bMXP = false;
      m_bPuebloActive = false;
      DiscardMXPTagsCreatedAfter (this, iStartBoundary);
      m_bInParagraph = false;
      m_bMXP_script = false;
      m_bPreMode = false;
      m_iListMode = eNoList;
      m_iListCount = 0;
      m_iMXPParagraphOwner = 0;
      m_iMXPPreOwner = 0;
      m_iMXPScriptOwner = 0;
      m_iMXPListOwner = 0;
      }
    throw;
    }

 }  // end of CMUSHclientDoc::MXP_On

