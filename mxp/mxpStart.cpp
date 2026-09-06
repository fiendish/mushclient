// mxpStart.cpp - MXP start tag

// eg. <send ...>

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


#define OUTSTANDING_TAG_WARNING 100  // warn every so many tags unclosed

// make false for debugging

#define SECURE_ELEMENT_CHECK true

class CArgumentListGuard
  {
  public:
    CArgumentListGuard (CArgumentList & list) : m_list (list) { }
    ~CArgumentListGuard () { DELETE_LIST (m_list); }

  private:
    CArgumentList & m_list;
  };

static std::unique_ptr<CElement> CloneCustomElement (const CElement * pSource)
  {
  std::unique_ptr<CElement> pClone (new CElement);
  pClone->strName = pSource->strName;
  pClone->iTag = pSource->iTag;
  pClone->strFlag = pSource->strFlag;
  pClone->bOpen = pSource->bOpen;
  pClone->bCommand = pSource->bCommand;

  for (POSITION pos = pSource->AttributeList.GetHeadPosition (); pos; )
    {
    CArgument * pSourceArgument = pSource->AttributeList.GetNext (pos);
    std::unique_ptr<CArgument> pArgument
      (new CArgument (pSourceArgument->strName,
                      pSourceArgument->strValue,
                      pSourceArgument->iPosition));
    pArgument->bKeyword = pSourceArgument->bKeyword;
    pArgument->bUsed = pSourceArgument->bUsed;
    pClone->AttributeList.AddTail (pArgument.get ());
    pArgument.release ();
    }

  for (POSITION itempos = pSource->ElementItemList.GetHeadPosition (); itempos; )
    {
    CElementItem * pSourceItem = pSource->ElementItemList.GetNext (itempos);
    std::unique_ptr<CElementItem> pItem (new CElementItem);
    pItem->pAtomicElement = pSourceItem->pAtomicElement;

    for (POSITION argpos = pSourceItem->ArgumentList.GetHeadPosition (); argpos; )
      {
      CArgument * pSourceArgument = pSourceItem->ArgumentList.GetNext (argpos);
      std::unique_ptr<CArgument> pArgument
        (new CArgument (pSourceArgument->strName,
                        pSourceArgument->strValue,
                        pSourceArgument->iPosition));
      pArgument->bKeyword = pSourceArgument->bKeyword;
      pArgument->bUsed = pSourceArgument->bUsed;
      pItem->ArgumentList.AddTail (pArgument.get ());
      pArgument.release ();
      }

    pClone->ElementItemList.AddTail (pItem.get ());
    pItem.release ();
    }

  return pClone;
  }

static void SnapshotActiveTags (CActiveTagList & activeTags,
                                vector<__int64> & identities)
  {
  identities.clear ();
  for (POSITION pos = activeTags.GetHeadPosition (); pos; )
    identities.push_back (activeTags.GetNext (pos)->nCreationNumber);
  }

static bool ActiveTagsMatch (CActiveTagList & activeTags,
                             const vector<__int64> & identities)
  {
  if (activeTags.GetCount () != identities.size ())
    return false;

  vector<__int64>::const_iterator it = identities.begin ();
  for (POSITION pos = activeTags.GetHeadPosition (); pos; ++it)
    if (activeTags.GetNext (pos)->nCreationNumber != *it)
      return false;

  return true;
  }

static CString BuildAtomicCallbackArguments (CArgumentList & arguments)
  {
  CString result;
  for (POSITION pos = arguments.GetHeadPosition (); pos; )
    {
    CArgument * pArgument = arguments.GetNext (pos);
    if (pArgument->strName.IsEmpty ())
      result += CFormat ("'%s'", (LPCTSTR) pArgument->strValue);
    else
      result += CFormat ("%s='%s'",
                         (LPCTSTR) pArgument->strName,
                         (LPCTSTR) pArgument->strValue);
    if (pos)
      result += " ";
    }
  return result;
  }

static bool ExpandAtomicArgumentEntities (CMUSHclientDoc * pDoc,
                                          CArgumentList & arguments)
  {
  for (POSITION pos = arguments.GetHeadPosition (); pos; )
    {
    CArgument * pArgument = arguments.GetNext (pos);
    if (pArgument->strValue.Find ('&') == -1)
      continue;

    const char * p = pArgument->strValue;
    const char * pStart = pArgument->strValue;
    CString strFixedValue;

    for ( ; *p; p++)
      if (*p == '&')
        {
        const long length = p - pStart;
        if (length > 0)
          strFixedValue += CString (pStart, length);

        p++;
        pStart = p;
        for ( ; *p && *p != ';'; p++)
          ;
        if (*p != ';')
          {
          pDoc->MXP_error (
            DBG_ERROR,
            errMXP_NoClosingSemicolonInArgument,
            TFormat ("No closing \";\" in MXP element argument \"%s\"",
                     (LPCTSTR) pArgument->strValue));
          return false;
          }

        CString strEntity (pStart, p - pStart);
        if (strEntity == "text")
          strFixedValue += "&text;";
        else
          strFixedValue += pDoc->MXP_GetEntity (strEntity);
        pStart = p + 1;
        }

    strFixedValue += pStart;
    pArgument->strValue = strFixedValue;
    }

  return true;
  }

static bool BuildCustomAtomicArguments (CMUSHclientDoc * pDoc,
                                        const CString & strElementName,
                                        CElement * pElement,
                                        CElementItem * pElementItem,
                                        CArgumentList & suppliedArguments,
                                        CArgumentList & builtArguments)
  {
  int iArgumentNumber = 0;
  for (POSITION atompos = pElementItem->ArgumentList.GetHeadPosition ();
       atompos; )
    {
    CArgument * pArgument = pElementItem->ArgumentList.GetNext (atompos);
    const char * p = pArgument->strValue;
    const char * pStart = pArgument->strValue;
    CString strFixedValue;

    for ( ; *p; p++)
      if (*p == '&')
        {
        const long length = p - pStart;
        if (length > 0)
          strFixedValue += CString (pStart, length);

        p++;
        pStart = p;
        for ( ; *p && *p != ';'; p++)
          ;
        if (*p != ';')
          {
          pDoc->MXP_error (
            DBG_ERROR,
            errMXP_NoClosingSemicolonInArgument,
            TFormat ("No closing \";\" in MXP element argument \"%s\"",
                     (LPCTSTR) pArgument->strValue));
          return false;
          }

        CString strEntity (pStart, p - pStart);
        CString strReplacement = "&text;";
        if (strEntity != "text")
          {
          CString strDefault;
          CArgument * pAttribute = NULL;
          int iSequence = 1;
          for (POSITION attpos = pElement->AttributeList.GetHeadPosition ();
               attpos; iSequence++)
            {
            pAttribute = pElement->AttributeList.GetNext (attpos);
            if (pAttribute->strName.IsEmpty ())
              {
              if (pAttribute->strValue.CompareNoCase (strEntity) == 0)
                break;
              }
            else if (pAttribute->strName.CompareNoCase (strEntity) == 0)
              {
              strDefault = pAttribute->strValue;
              break;
              }
            pAttribute = NULL;
            }

          if (pAttribute)
            {
            strReplacement = GetArgument (suppliedArguments,
                                          strEntity,
                                          iSequence,
                                          false);
            if (strReplacement.IsEmpty ())
              {
              strReplacement = strDefault;
              if (strReplacement.IsEmpty ())
                pDoc->MXP_error (
                  DBG_WARNING,
                  wrnMXP_ArgumentNotSupplied,
                  TFormat ("Non-default argument \"%s\" not supplied to <%s>",
                           (LPCTSTR) strEntity,
                           (LPCTSTR) strElementName));
              }
            }
          else
            strReplacement = pDoc->MXP_GetEntity (strEntity);
          }

        strFixedValue += strReplacement;
        pStart = p + 1;
        }

    strFixedValue += pStart;
    std::unique_ptr<CArgument> pNewArgument;
    if (pArgument->strName.IsEmpty ())
      pNewArgument.reset (
        new CArgument ("", strFixedValue, ++iArgumentNumber));
    else
      pNewArgument.reset (
        new CArgument (pArgument->strName, strFixedValue, 0));
    builtArguments.AddTail (pNewArgument.get ());
    pNewArgument.release ();
    }

  return true;
  }

static void ReportDeferredMXPMessages (
  CMUSHclientDoc * pDoc,
  vector<CDeferredMXPMessage> & messages)
  {
  for (vector<CDeferredMXPMessage>::const_iterator it = messages.begin ();
       it != messages.end (); ++it)
    pDoc->MXP_error (it->iLevel, it->iMessageNumber, it->strMessage);
  messages.clear ();
  }

class CMXPStartTransaction
  {
  public:
    CMXPStartTransaction (CMUSHclientDoc * pDoc,
                          const __int64 iStateOwner) :
      m_pDoc (pDoc),
      m_iStateOwner (iStateOwner),
      m_bOpeningInParagraph (pDoc->m_bInParagraph),
      m_bOpeningPreMode (pDoc->m_bPreMode),
      m_bOpeningMXPScript (pDoc->m_bMXP_script),
      m_iOpeningListMode (pDoc->m_iListMode),
      m_iOpeningListCount (pDoc->m_iListCount),
      m_iOpeningParagraphOwner (pDoc->m_iMXPParagraphOwner),
      m_iOpeningPreOwner (pDoc->m_iMXPPreOwner),
      m_iOpeningScriptOwner (pDoc->m_iMXPScriptOwner),
      m_iOpeningListOwner (pDoc->m_iMXPListOwner),
      m_iActiveTagCreationNumber (0),
      m_iMarkerCreationNumber (0),
      m_iStyleCreationNumber (0),
      m_iPreservedStyleCreationNumber (0),
      m_iPreservedRangeCreationNumber (0),
      m_iPreservedOutputAppendCreationNumber (0),
      m_iPreservedLength (0),
      m_iPreservedFlags (0),
      m_iPreservedForeColour (0),
      m_iPreservedBackColour (0),
      m_pPreservedAction (NULL),
      m_bCommitted (false) { }

    ~CMXPStartTransaction ()
      { Rollback (); }

    void Rollback ()
      {
      if (m_bCommitted)
        return;
      m_bCommitted = true;

      if (m_pOutputTransaction)
        m_pOutputTransaction->Rollback ();

      // A callback can publish a newer tag before this opening transaction
      // fails. Replace every saved reference to this failed owner so that a
      // later close cannot restore state owned by a tag that no longer exists.
      for (POSITION pos = m_pDoc->m_ActiveTagList.GetHeadPosition (); pos; )
        {
        CActiveTag * pTag = m_pDoc->m_ActiveTagList.GetNext (pos);
        if (pTag->iOpeningParagraphOwner == m_iStateOwner)
          {
          pTag->bOpeningInParagraph = m_bOpeningInParagraph;
          pTag->iOpeningParagraphOwner = m_iOpeningParagraphOwner;
          }
        if (pTag->iOpeningPreOwner == m_iStateOwner)
          {
          pTag->bOpeningPreMode = m_bOpeningPreMode;
          pTag->iOpeningPreOwner = m_iOpeningPreOwner;
          }
        if (pTag->iOpeningScriptOwner == m_iStateOwner)
          {
          pTag->bOpeningMXPScript = m_bOpeningMXPScript;
          pTag->iOpeningScriptOwner = m_iOpeningScriptOwner;
          }
        if (pTag->iOpeningListOwner == m_iStateOwner)
          {
          pTag->iOpeningListMode = m_iOpeningListMode;
          pTag->iOpeningListCount = m_iOpeningListCount;
          pTag->iOpeningListOwner = m_iOpeningListOwner;
          }
        }

      if (m_pDoc->m_iMXPParagraphOwner == m_iStateOwner)
        {
        m_pDoc->m_bInParagraph = m_bOpeningInParagraph;
        m_pDoc->m_iMXPParagraphOwner = m_iOpeningParagraphOwner;
        }
      if (m_pDoc->m_iMXPPreOwner == m_iStateOwner)
        {
        m_pDoc->m_bPreMode = m_bOpeningPreMode;
        m_pDoc->m_iMXPPreOwner = m_iOpeningPreOwner;
        }
      if (m_pDoc->m_iMXPScriptOwner == m_iStateOwner)
        {
        m_pDoc->m_bMXP_script = m_bOpeningMXPScript;
        m_pDoc->m_iMXPScriptOwner = m_iOpeningScriptOwner;
        }
      if (m_pDoc->m_iMXPListOwner == m_iStateOwner)
        {
        m_pDoc->m_iListMode = m_iOpeningListMode;
        m_pDoc->m_iListCount = m_iOpeningListCount;
        m_pDoc->m_iMXPListOwner = m_iOpeningListOwner;
        }

      CStyle * pMarkerStyle = FindStyle (m_iMarkerCreationNumber);
      CStyle * pNewStyle = FindStyle (m_iStyleCreationNumber);
      const bool bMarkerWasRepurposed =
        pMarkerStyle &&
        ((pMarkerStyle->iFlags & START_TAG) == 0 ||
         pMarkerStyle->iLength != 0);
      const bool bOriginalStyleMissing =
        m_iPreservedStyleCreationNumber &&
        !HasStyle (m_iPreservedStyleCreationNumber);

      if (bMarkerWasRepurposed)
        {
        if (pNewStyle && pNewStyle->iLength == 0)
          RemoveStyle (m_iStyleCreationNumber);
        if (pMarkerStyle->pAction)
          pMarkerStyle->pAction->Release ();
        pMarkerStyle->iFlags = m_iPreservedFlags;
        pMarkerStyle->iForeColour = m_iPreservedForeColour;
        pMarkerStyle->iBackColour = m_iPreservedBackColour;
        pMarkerStyle->pAction = m_pPreservedAction;
        pMarkerStyle->nRangeCreationNumber =
          m_iPreservedRangeCreationNumber;
        pMarkerStyle->nOutputAppendCreationNumber =
          m_iPreservedOutputAppendCreationNumber;
        if (bOriginalStyleMissing)
          pMarkerStyle->nCreationNumber = m_iPreservedStyleCreationNumber;
        m_pPreservedAction = NULL;
        }
      else if (bOriginalStyleMissing &&
          (pMarkerStyle || (pNewStyle && pNewStyle->iLength == 0)))
        {
        CStyle * pRestoreStyle = pMarkerStyle ? pMarkerStyle : pNewStyle;
        if (pRestoreStyle != pNewStyle && pNewStyle && pNewStyle->iLength == 0)
          RemoveStyle (m_iStyleCreationNumber);
        if (pRestoreStyle != pMarkerStyle)
          RemoveStyle (m_iMarkerCreationNumber);
        if (pRestoreStyle->pAction)
          pRestoreStyle->pAction->Release ();
        pRestoreStyle->iLength = m_iPreservedLength;
        pRestoreStyle->iFlags = m_iPreservedFlags;
        pRestoreStyle->iForeColour = m_iPreservedForeColour;
        pRestoreStyle->iBackColour = m_iPreservedBackColour;
        pRestoreStyle->pAction = m_pPreservedAction;
        pRestoreStyle->nCreationNumber = m_iPreservedStyleCreationNumber;
        pRestoreStyle->nRangeCreationNumber =
          m_iPreservedRangeCreationNumber;
        pRestoreStyle->nOutputAppendCreationNumber =
          m_iPreservedOutputAppendCreationNumber;
        m_pPreservedAction = NULL;
        }
      else
        {
        if (pNewStyle && pNewStyle->iLength == 0)
          RemoveStyle (m_iStyleCreationNumber);
        if (pMarkerStyle && !bMarkerWasRepurposed)
          RemoveStyle (m_iMarkerCreationNumber);
        }

      if (m_iActiveTagCreationNumber)
        for (POSITION pos = m_pDoc->m_ActiveTagList.GetHeadPosition (); pos; )
          {
          POSITION current = pos;
          CActiveTag * pTag = m_pDoc->m_ActiveTagList.GetNext (pos);
          if (pTag->nCreationNumber == m_iActiveTagCreationNumber)
            {
            m_pDoc->m_ActiveTagList.RemoveAt (current);
            delete pTag;
            break;
            }
          }

      if (m_pPreservedAction)
        m_pPreservedAction->Release ();
      }

    void PreserveStyle (const CStyle * pStyle)
      {
      m_iPreservedStyleCreationNumber = pStyle->nCreationNumber;
      m_iPreservedRangeCreationNumber = pStyle->nRangeCreationNumber;
      m_iPreservedOutputAppendCreationNumber =
        pStyle->nOutputAppendCreationNumber;
      m_iPreservedLength = pStyle->iLength;
      m_iPreservedFlags = pStyle->iFlags;
      m_iPreservedForeColour = pStyle->iForeColour;
      m_iPreservedBackColour = pStyle->iBackColour;
      m_pPreservedAction = pStyle->pAction;
      if (m_pPreservedAction)
        m_pPreservedAction->AddRef ();
      }

    void SetActiveTag (const CActiveTag * pTag)
      { m_iActiveTagCreationNumber = pTag->nCreationNumber; }

    void SetMarker (const CStyle * pStyle)
      { m_iMarkerCreationNumber = pStyle->nCreationNumber; }

    void SetStyle (const CStyle * pStyle)
      { m_iStyleCreationNumber = pStyle->nCreationNumber; }

    COutputAppendTransaction * OutputTransaction ()
      {
      if (!m_pOutputTransaction)
        m_pOutputTransaction.reset (
          new COutputAppendTransaction (m_pDoc, 0));
      return m_pOutputTransaction.get ();
      }

    bool IsPublishedStatePresent () const
      {
      return (!m_iActiveTagCreationNumber ||
              HasActiveTag (m_iActiveTagCreationNumber)) &&
             (!m_iMarkerCreationNumber || HasStyle (m_iMarkerCreationNumber));
      }

    void Commit ()
      {
      if (m_pOutputTransaction)
        m_pOutputTransaction->Commit ();
      m_bCommitted = true;
      if (m_pPreservedAction)
        {
        m_pPreservedAction->Release ();
        m_pPreservedAction = NULL;
        }
      }

  private:
    bool HasActiveTag (const __int64 iCreationNumber) const
      {
      for (POSITION pos = m_pDoc->m_ActiveTagList.GetHeadPosition (); pos; )
        if (m_pDoc->m_ActiveTagList.GetNext (pos)->nCreationNumber == iCreationNumber)
          return true;
      return false;
      }

    bool HasStyle (const __int64 iCreationNumber) const
      { return FindStyle (iCreationNumber) != NULL; }

    CStyle * FindStyle (const __int64 iCreationNumber) const
      {
      for (POSITION linepos = m_pDoc->m_LineList.GetHeadPosition (); linepos; )
        {
        CLine * pLine = m_pDoc->m_LineList.GetNext (linepos);
        for (POSITION stylepos = pLine->styleList.GetHeadPosition (); stylepos; )
          {
          CStyle * pStyle = pLine->styleList.GetNext (stylepos);
          if (pStyle->nCreationNumber == iCreationNumber)
            return pStyle;
          }
        }
      return NULL;
      }

    void RemoveStyle (const __int64 iCreationNumber)
      {
      if (!iCreationNumber)
        return;

      for (POSITION linepos = m_pDoc->m_LineList.GetHeadPosition (); linepos; )
        {
        CLine * pLine = m_pDoc->m_LineList.GetNext (linepos);
        for (POSITION stylepos = pLine->styleList.GetHeadPosition (); stylepos; )
          {
          POSITION current = stylepos;
          CStyle * pStyle = pLine->styleList.GetNext (stylepos);
          if (pStyle->nCreationNumber == iCreationNumber)
            {
            pLine->styleList.RemoveAt (current);
            DELETESTYLE (pStyle);
            return;
            }
          }
        }
      }

    CMUSHclientDoc * m_pDoc;
    __int64 m_iStateOwner;
    bool m_bOpeningInParagraph;
    bool m_bOpeningPreMode;
    bool m_bOpeningMXPScript;
    int m_iOpeningListMode;
    int m_iOpeningListCount;
    __int64 m_iOpeningParagraphOwner;
    __int64 m_iOpeningPreOwner;
    __int64 m_iOpeningScriptOwner;
    __int64 m_iOpeningListOwner;
    __int64 m_iActiveTagCreationNumber;
    __int64 m_iMarkerCreationNumber;
    __int64 m_iStyleCreationNumber;
    __int64 m_iPreservedStyleCreationNumber;
    __int64 m_iPreservedRangeCreationNumber;
    __int64 m_iPreservedOutputAppendCreationNumber;
    unsigned short m_iPreservedLength;
    unsigned short m_iPreservedFlags;
    COLORREF m_iPreservedForeColour;
    COLORREF m_iPreservedBackColour;
    CAction * m_pPreservedAction;
    std::unique_ptr<COutputAppendTransaction> m_pOutputTransaction;
    bool m_bCommitted;
  };

class CActionReferenceGuard
  {
  public:
    CActionReferenceGuard (CAction * pAction) : m_pAction (pAction)
      {
      if (m_pAction)
        m_pAction->AddRef ();
      }

    ~CActionReferenceGuard ()
      {
      if (m_pAction)
        m_pAction->Release ();
      }

    CAction * Get () const { return m_pAction; }

    void Reset (CAction * pAction)
      {
      if (pAction)
        pAction->AddRef ();
      if (m_pAction)
        m_pAction->Release ();
      m_pAction = pAction;
      }

  private:
    CAction * m_pAction;
  };


// here for start tag, eg. <bold> <underline> <usertag>
void CMUSHclientDoc::MXP_StartTag (CString strTag)
  {
// are we in secure mode right now?
bool bSecure = MXP_Secure ();
bool bNoReset = false;

  MXP_Restore_Mode ();  // cancel secure-once mode

CArgumentList ArgumentList;
CArgumentListGuard argumentListGuard (ArgumentList);

CString strName;

  GetWord (strName, strTag);

  // count them
  m_iMXPtags++;

  if (!IsValidName (strName))
    {
    MXP_error (DBG_ERROR, errMXP_InvalidElementName,
                TFormat ("Invalid MXP element name \"%s\" supplied.",
                strName)); 
    return;
    }

  // case insensitive?
  strName.MakeLower ();

// see if we know of this element

CAtomicElement * pAtomicElement = NULL;
CElement * pElement = NULL;
std::unique_ptr<CElement> pElementSnapshot;
bool bOpen;
bool bCommand;

// find existing styles

CStyle * pStyle = m_pCurrentLine->styleList.GetTail ();

unsigned short iFlags = pStyle->iFlags;      
COLORREF       iForeColour = pStyle->iForeColour; 
COLORREF       iBackColour = pStyle->iBackColour; 
CActionReferenceGuard openingActionGuard (pStyle->pAction);
CAction *      pAction = openingActionGuard.Get ();

CString strAction;
CString strHint;
CString strVariable;
CString strTagVariable;

  // get old action, hint etc. so that something like:
  // <send href="nick"> <b> blah </b> </send> will work
  // in this case we want the href (action) to persist through the <b>

  if (pAction)
    {
    strAction = pAction->m_strAction;
    strHint = pAction->m_strHint;
    strVariable = pAction->m_strVariable;
    } // end of having an action etc.


  if (App.m_ElementMap.Lookup (strName, pAtomicElement))
    {
    bOpen    = (pAtomicElement->iFlags & TAG_OPEN) != 0;
    bCommand = (pAtomicElement->iFlags & TAG_COMMAND) != 0;
    bNoReset = (pAtomicElement->iFlags & TAG_NO_RESET) != 0;

    // check for mixing Pueblo and MXP tags

/*

  // ALLOW BOTH for now

    if ((pAtomicElement->iFlags & TAG_PUEBLO) &&
        !m_bPuebloActive)
      {
      MXP_error (DBG_ERROR, errMXP_PuebloOnly,
                TFormat ("Using Pueblo-only element in MXP mode: <%s>" ,
                          (LPCTSTR) strName));
      return;
      }
    
    if ((pAtomicElement->iFlags & TAG_MXP) &&
        m_bPuebloActive)
      {
      MXP_error (DBG_ERROR, errMXP_MXPOnly,
                TFormat ("Using MXP-only element in Pueblo mode: <%s>" ,
                          (LPCTSTR) strName));
      return;
      }

*/

    } // end of atomic element found
  else
    {
    if (!m_CustomElementMap.Lookup (strName, pElement))
      {
      MXP_error (DBG_ERROR, errMXP_UnknownElement,
                 TFormat ("Unknown MXP element: <%s>" ,
                          (LPCTSTR) strName));
      return;
      }
    pAtomicElement = NULL;
    bOpen    = pElement->bOpen;
    bCommand = pElement->bCommand;
    if (!pElement->strFlag.IsEmpty ())
      strTagVariable = pElement->strFlag;  // variable set when this tag closes
    } // end of not atomic


  // check for secure tags
  if (!bOpen && 
      !bSecure &&
      SECURE_ELEMENT_CHECK)
    {
    MXP_error (DBG_ERROR, errMXP_ElementWhenNotSecure,
                TFormat ("Secure MXP tag ignored when not in secure mode: <%s>" ,
                        (LPCTSTR) strName));
    return;
    }


  if (BuildArgumentList (ArgumentList, strTag))
    {
    DELETE_LIST (ArgumentList);
    return;
    }

  const __int64 iOpeningMXPGeneration = m_iMXPGeneration;
  vector<__int64> activeTagsBeforePreparation;
  SnapshotActiveTags (m_ActiveTagList, activeTagsBeforePreparation);
  const bool bExpandedAtomicArguments = pAtomicElement != NULL;
  if (pAtomicElement && !ExpandAtomicArgumentEntities (this, ArgumentList))
    return;

  // Run the open callback before publishing this tag. A callback-created tag
  // then has a stable outer position and cannot be overwritten by this open.
  bool bRanOpenCallback = false;
  bool bNotWanted = false;
  if (m_dispidOnMXP_OpenTag != DISPID_UNKNOWN || m_bPluginProcessesOpenTag)
    {
    bRanOpenCallback = true;
    const CString strCallbackArguments = pAtomicElement ?
      BuildAtomicCallbackArguments (ArgumentList) : strTag;
    bNotWanted =
      MXP_StartTagScript (strName, strCallbackArguments, ArgumentList);
    }

  if (m_iMXPGeneration != iOpeningMXPGeneration || !m_bMXP)
    return;

  if (bExpandedAtomicArguments || bRanOpenCallback)
    {
    if (ActiveTagsMatch (m_ActiveTagList, activeTagsBeforePreparation))
      {
      // A note can change the current output style. Publish a new boundary
      // for the requested tag instead of rewriting the callback's style.
      pStyle = AddStyle (iFlags & STYLE_BITS,
                         iForeColour,
                         iBackColour,
                         0,
                         pAction);
      }
    else
      {
      // A callback-created tag is now outside the requested tag. Inherit its
      // current style so that the two tags remain correctly nested.
      pStyle = m_pCurrentLine->styleList.GetTail ();
      }

    iFlags = pStyle->iFlags;
    iForeColour = pStyle->iForeColour;
    iBackColour = pStyle->iBackColour;
    openingActionGuard.Reset (pStyle->pAction);
    pAction = openingActionGuard.Get ();
    strAction.Empty ();
    strHint.Empty ();
    strVariable.Empty ();
    if (pAction)
      {
      strAction = pAction->m_strAction;
      strHint = pAction->m_strHint;
      strVariable = pAction->m_strVariable;
      }

    if (bNotWanted)
      return;   // they didn't want to go ahead with this tag

    if (bRanOpenCallback && !pAtomicElement)
      {
      // The callback can redefine or delete this custom element.
      pElement = NULL;
      if (!m_CustomElementMap.Lookup (strName, pElement))
        {
        MXP_error (DBG_ERROR, errMXP_UnknownElement,
                   TFormat ("Unknown MXP element after open-tag callback: <%s>",
                            (LPCTSTR) strName));
        return;
        }
      bOpen = pElement->bOpen;
      bCommand = pElement->bCommand;
      bNoReset = false;
      strTagVariable.Empty ();
      if (!pElement->strFlag.IsEmpty ())
        strTagVariable = pElement->strFlag;

      if (!bOpen && !bSecure && SECURE_ELEMENT_CHECK)
        {
        MXP_error (DBG_ERROR, errMXP_ElementWhenNotSecure,
                   TFormat ("Secure MXP tag ignored when not in secure mode: <%s>",
                            (LPCTSTR) strName));
        return;
        }
      }
    }

  vector<std::unique_ptr<CElementItem> > expandedItems;
  vector<bool> expandedItemsWanted;
  if (!pAtomicElement)
    {
    pElementSnapshot = CloneCustomElement (pElement);
    pElement = pElementSnapshot.get ();

    vector<__int64> activeTagsBeforeExpandedCallbacks;
    SnapshotActiveTags (m_ActiveTagList, activeTagsBeforeExpandedCallbacks);
    bool bPreparedExpandedItems = false;
    for (POSITION itempos = pElement->ElementItemList.GetHeadPosition ();
         itempos; )
      {
      CElementItem * pSourceItem = pElement->ElementItemList.GetNext (itempos);
      std::unique_ptr<CElementItem> pExpandedItem (new CElementItem);
      pExpandedItem->pAtomicElement = pSourceItem->pAtomicElement;
      if (!BuildCustomAtomicArguments (this,
                                       strName,
                                       pElement,
                                       pSourceItem,
                                       ArgumentList,
                                       pExpandedItem->ArgumentList))
        return;

      bool bWanted = true;
      if (m_dispidOnMXP_OpenTag != DISPID_UNKNOWN ||
          m_bPluginProcessesOpenTag)
        {
        bPreparedExpandedItems = true;
        bWanted = !MXP_StartTagScript (
          pExpandedItem->pAtomicElement->strName,
          BuildAtomicCallbackArguments (pExpandedItem->ArgumentList),
          pExpandedItem->ArgumentList);
        }
      expandedItemsWanted.push_back (bWanted);
      expandedItems.push_back (std::move (pExpandedItem));
      }

    if (m_iMXPGeneration != iOpeningMXPGeneration || !m_bMXP)
      return;

    if (bPreparedExpandedItems)
      {
      if (ActiveTagsMatch (m_ActiveTagList,
                           activeTagsBeforeExpandedCallbacks))
        pStyle = AddStyle (iFlags & STYLE_BITS,
                           iForeColour,
                           iBackColour,
                           0,
                           pAction);
      else
        pStyle = m_pCurrentLine->styleList.GetTail ();

      iFlags = pStyle->iFlags;
      iForeColour = pStyle->iForeColour;
      iBackColour = pStyle->iBackColour;
      openingActionGuard.Reset (pStyle->pAction);
      pAction = openingActionGuard.Get ();
      strAction.Empty ();
      strHint.Empty ();
      strVariable.Empty ();
      if (pAction)
        {
        strAction = pAction->m_strAction;
        strHint = pAction->m_strHint;
        strVariable = pAction->m_strVariable;
        }
      }
    }

  vector<CDeferredMXPMessage> deferredMessages;
  const __int64 iStateOwner = App.GetUniqueNumber ();
  CMXPStartTransaction transaction (this, iStateOwner);
  transaction.PreserveStyle (pStyle);
  bool bWarnOutstandingTags = false;
  CActiveTag * pPublishedTag = NULL;

  // command tags are not popped from the stack, and thus don't need a record

  if (!bCommand)
    {
    // remember what is outstanding

    std::unique_ptr<CActiveTag> pTag (new CActiveTag);

    pTag->strName = strName;
    pTag->bSecure = bSecure;
    pTag->bNoReset = bNoReset;
    pTag->nCreationNumber = iStateOwner;
    pTag->iOpeningFlags = iFlags & STYLE_BITS;
    pTag->iOpeningForeColour = iForeColour;
    pTag->iOpeningBackColour = iBackColour;
    pTag->pOpeningAction = pAction;
    if (pTag->pOpeningAction)
      pTag->pOpeningAction->AddRef ();
    pTag->strVariable = strTagVariable;
    pTag->bOpeningInParagraph = m_bInParagraph;
    pTag->bOpeningPreMode = m_bPreMode;
    pTag->bOpeningMXPScript = m_bMXP_script;
    pTag->iOpeningListMode = m_iListMode;
    pTag->iOpeningListCount = m_iListCount;
    pTag->iOpeningParagraphOwner = m_iMXPParagraphOwner;
    pTag->iOpeningPreOwner = m_iMXPPreOwner;
    pTag->iOpeningScriptOwner = m_iMXPScriptOwner;
    pTag->iOpeningListOwner = m_iMXPListOwner;
    if (pAtomicElement)
      pTag->closeActions.push_back (pAtomicElement->iAction);
    else
      for (POSITION closepos = pElement->ElementItemList.GetHeadPosition ();
           closepos; )
        {
        CElementItem * pCloseItem = pElement->ElementItemList.GetNext (closepos);
        pTag->closeActions.push_back (pCloseItem->pAtomicElement->iAction);
        }
    m_ActiveTagList.AddTail (pTag.get ());  // add to outstanding tag list
    pPublishedTag = pTag.release ();
    transaction.SetActiveTag (pPublishedTag);

    // add a marker to the current line for the tag itself
    // this is a record of what the text style was at this point
    CStyle * pMarkerStyle = AddStyle (iFlags | START_TAG,
                                      iForeColour,
                                      iBackColour,
                                      0,
                                      strName,
                                      "",
                                      strTagVariable);
    transaction.SetMarker (pMarkerStyle);
    pPublishedTag->nOpeningStyleCreationNumber = pMarkerStyle->nCreationNumber;
    pPublishedTag->nOpeningLineCreationNumber =
      m_pCurrentLine->nCreationNumber;

    // warn if they are overdoing the outstanding tags
    if (m_ActiveTagList.GetCount () % OUTSTANDING_TAG_WARNING == 0 &&
        m_ActiveTagList.GetCount () != m_iLastOutstandingTagCount)
      bWarnOutstandingTags = true;


    } // end of not command tag

//if (strName == "ex")
  /*
  {

  iFlags &= ~COLOURTYPE;  // clear bits, eg. custom
  iFlags |= COLOUR_RGB;

  iForeColour = 255;
  iBackColour = 255 * 256;
  }
  */

// now add another style entry to the line - this will have the
// style adjusted for the new attributes (eg. bold)

CStyle * pNewStyle = AddStyle (iFlags & STYLE_BITS,
                               iForeColour, 
                               iBackColour, 
                               0,
                               strAction,
                               strHint,
                               strVariable);
transaction.SetStyle (pNewStyle);

  
  // atomic element?  (looked-up earlier)
  if (pAtomicElement)
    {
    try
      {
    if (!transaction.IsPublishedStatePresent ())
      {
      transaction.Rollback ();
      ReportDeferredMXPMessages (this, deferredMessages);
      return;
      }
    if (!pNewStyle)
      {
      transaction.Rollback ();
      ReportDeferredMXPMessages (this, deferredMessages);
      return;
      }

    CStyle * pAtomicResultStyle = pNewStyle;
    COutputAppendTransaction * pOutputTransaction =
      (pAtomicElement->iAction == MXP_ACTION_BR ||
       pAtomicElement->iAction == MXP_ACTION_HR ||
       pAtomicElement->iAction == MXP_ACTION_LI ||
       pAtomicElement->iAction == MXP_ACTION_IMG ||
       pAtomicElement->iAction == MXP_ACTION_IMAGE) ?
        transaction.OutputTransaction () : NULL;
    if (!MXP_OpenAtomicTag (strName,
                            pAtomicElement->iAction,
                            pNewStyle,
                            pAtomicResultStyle,
                            strAction,
                            strHint,
                            strVariable,
                            ArgumentList,
                            iStateOwner,
                            pOutputTransaction,
                            deferredMessages))
      {
      transaction.Rollback ();
      ReportDeferredMXPMessages (this, deferredMessages);
      return;
      }

    if (m_iMXPGeneration != iOpeningMXPGeneration || !m_bMXP)
      {
      transaction.Rollback ();
      ReportDeferredMXPMessages (this, deferredMessages);
      return;
      }

    if (!transaction.IsPublishedStatePresent ())
      {
      transaction.Rollback ();
      ReportDeferredMXPMessages (this, deferredMessages);
      return;
      }

    if (pPublishedTag && pAtomicElement->iAction == MXP_ACTION_VAR)
      pPublishedTag->strVariable = strVariable;

    // Finalize only the style that the atomic action published. A callback can
    // create a newer nested tag, whose tail style must stay unchanged.
    pNewStyle = pAtomicResultStyle;
    if (pNewStyle)
      {
      RememberStyle (pNewStyle);
      }

    if (pNewStyle)
      {
      CAction * pReplacementAction =
        GetAction (strAction, strHint, strVariable);
      CAction * pOldAction = pNewStyle->pAction;
      pNewStyle->pAction = pReplacementAction;
      if (pOldAction)
        pOldAction->Release ();
      }

    transaction.Commit ();
    ReportDeferredMXPMessages (this, deferredMessages);
    CheckArgumentsUsed (strName, ArgumentList);
    DELETE_LIST (ArgumentList);  // clean up memory
    if (bWarnOutstandingTags)
      {
      MXP_error (DBG_WARNING, wrnMXP_ManyOutstandingTags,
                 TFormat ("Now have %i outstanding MXP tags",
                          m_ActiveTagList.GetCount ()));
      m_iLastOutstandingTagCount = m_ActiveTagList.GetCount ();
      }
    return;
      }
    catch (...)
      {
      transaction.Rollback ();
      throw;
      }
    }

// --------- end of processing for ATOMIC element ------------

// must be a user-defined element

  try
    {
  for (size_t iExpandedItem = 0;
       iExpandedItem < expandedItems.size ();
       iExpandedItem++)
    {
    if (!expandedItemsWanted [iExpandedItem])
      continue;
    CElementItem * pElementItem = expandedItems [iExpandedItem].get ();
    CArgumentList & BuiltArgumentList = pElementItem->ArgumentList;

    if (!transaction.IsPublishedStatePresent ())
      {
      transaction.Rollback ();
      ReportDeferredMXPMessages (this, deferredMessages);
      return;
      }
    CStyle * pAtomicResultStyle = pNewStyle;
    COutputAppendTransaction * pOutputTransaction =
      (pElementItem->pAtomicElement->iAction == MXP_ACTION_BR ||
       pElementItem->pAtomicElement->iAction == MXP_ACTION_HR ||
       pElementItem->pAtomicElement->iAction == MXP_ACTION_LI ||
       pElementItem->pAtomicElement->iAction == MXP_ACTION_IMG ||
       pElementItem->pAtomicElement->iAction == MXP_ACTION_IMAGE) ?
        transaction.OutputTransaction () : NULL;
    if (!MXP_OpenAtomicTag (pElementItem->pAtomicElement->strName,
                            pElementItem->pAtomicElement->iAction,
                            pNewStyle,
                            pAtomicResultStyle,
                            strAction,
                            strHint,
                            strVariable,
                            BuiltArgumentList,
                            iStateOwner,
                            pOutputTransaction,
                            deferredMessages))
      {
      transaction.Rollback ();
      ReportDeferredMXPMessages (this, deferredMessages);
      return;
      }

    if (m_iMXPGeneration != iOpeningMXPGeneration || !m_bMXP)
      {
      transaction.Rollback ();
      ReportDeferredMXPMessages (this, deferredMessages);
      return;
      }

    if (!transaction.IsPublishedStatePresent ())
      {
      transaction.Rollback ();
      ReportDeferredMXPMessages (this, deferredMessages);
      return;
      }

    if (pPublishedTag &&
        pElementItem->pAtomicElement->iAction == MXP_ACTION_VAR)
      pPublishedTag->strVariable = strVariable;

    pNewStyle = pAtomicResultStyle;
    if (!pNewStyle)
      {
      COutputAppendTransaction * pBoundaryTransaction =
        transaction.OutputTransaction ();
      pBoundaryTransaction->Reserve (1);
      pBoundaryTransaction->MarkCurrentLineStyles ();
      pNewStyle = pBoundaryTransaction->PrepareAppendStyle ();
      }

    } // end of doing each atomic element

  if (pNewStyle)
    {
    RememberStyle (pNewStyle);
    }

  // make an action for the built-up action/hint/variable
  if (pNewStyle)
    {
    CAction * pReplacementAction = GetAction (strAction, strHint, strVariable);
    CAction * pOldAction = pNewStyle->pAction;
    pNewStyle->pAction = pReplacementAction;
    if (pOldAction)
      pOldAction->Release ();
    }

  transaction.Commit ();
  ReportDeferredMXPMessages (this, deferredMessages);

  for (size_t iExpandedItem = 0;
       iExpandedItem < expandedItems.size ();
       iExpandedItem++)
    if (expandedItemsWanted [iExpandedItem])
      CheckArgumentsUsed (
        expandedItems [iExpandedItem]->pAtomicElement->strName,
        expandedItems [iExpandedItem]->ArgumentList);
  CheckArgumentsUsed (strName, ArgumentList);
  DELETE_LIST (ArgumentList);  // clean up memory
  if (bWarnOutstandingTags)
    {
    MXP_error (DBG_WARNING, wrnMXP_ManyOutstandingTags,
               TFormat ("Now have %i outstanding MXP tags",
                        m_ActiveTagList.GetCount ()));
    m_iLastOutstandingTagCount = m_ActiveTagList.GetCount ();
    }
    }
  catch (...)
    {
    transaction.Rollback ();
    throw;
    }

  } // end of CMUSHclientDoc::MXP_StartTag


bool CMUSHclientDoc::MXP_StartTagScript  (const CString & strName, 
                     const CString & strArguments,
                     CArgumentList & ArgumentList)
  {

  // don't make it too easy to dummy up AFK replies
  if (strName == "afk")
    return false;

  if (!SendToAllPluginCallbacks (ON_PLUGIN_MXP_OPENTAG, 
                                CFormat ("%s,%s",
                                (LPCTSTR) strName,
                                (LPCTSTR) strArguments)
                                , true))
      return true;    


  // see if main script wants to do anything
  if (m_dispidOnMXP_OpenTag == DISPID_UNKNOWN)
    return false;

  long nInvocationCount = 0;
  long iCount = ArgumentList.GetCount ();

  CString strType = "MXP open tag";
  CString strReason =  TFormat ("opening MXP tag %s", (LPCTSTR) strName);

  if (GetScriptEngine () && GetScriptEngine ()->IsLua ())
    {
    list<double> nparams;
    list<string> sparams;
    sparams.push_back ((LPCTSTR) strName);    // name of tag
    sparams.push_back ((LPCTSTR) strArguments);  // all arguments

    map <string, string> table;

    CArgument * pArgument;
    POSITION pos;

    // put the arguments into the table

    for (iCount = 0, pos = ArgumentList.GetHeadPosition (); pos; iCount++)
      {
      pArgument = ArgumentList.GetNext (pos);
      CString strName = pArgument->strName;

      // empty ones we will put there by position
      if (strName.IsEmpty ())
        strName = CFormat ("%i",
                      pArgument->iPosition);
      
      table [(LPCTSTR) strName] = pArgument->strValue;
      }      // end of looping through each argument

    bool result;
    GetScriptEngine ()->ExecuteLua (m_dispidOnMXP_OpenTag, 
                                   m_strOnMXP_OpenTag, 
                                   eWorldAction,
                                   strType, 
                                   strReason, 
                                   nparams,
                                   sparams, 
                                   nInvocationCount,
                                   NULL,
                                   &table,
                                   NULL,
                                   &result);
    return result;
    }   // end of Lua

  COleSafeArray sa;   // for wildcard list

  if (iCount) // cannot create empty array dimension
    {
    sa.CreateOneDim (VT_VARIANT, iCount);

    CArgument * pArgument;
    POSITION pos;

    // put the arguments into the array

    for (iCount = 0, pos = ArgumentList.GetHeadPosition (); pos; iCount++)
      {
      pArgument = ArgumentList.GetNext (pos);

      // the array must be a bloody array of variants, or VBscript kicks up
      COleVariant v;
      
      // empty ones we will put there by position
      if (pArgument->strName.IsEmpty ())
        v = CFormat ("%i=%s",
                      pArgument->iPosition,
                      (LPCTSTR) pArgument->strValue);
      else
        v = CFormat ("%s=%s",
                      (LPCTSTR) pArgument->strName,
                      (LPCTSTR) pArgument->strValue);
      sa.PutElement (&iCount, &v);
      }      // end of looping through each argument
    } // end of having at least one

  // WARNING - arguments should appear in REVERSE order to what the sub expects them!

  enum
    {
    eArgumentArray,
    eArguments,
    eTagName,
    eArgCount,     // this MUST be last
    };    

  COleVariant args [eArgCount];
  DISPPARAMS params = { args, NULL, eArgCount, 0 };

  args [eTagName] = strName;
  args [eArguments] = strArguments;
  args [eArgumentArray] = sa;

  COleVariant result;

  ExecuteScript (m_dispidOnMXP_OpenTag,  
                 m_strOnMXP_OpenTag,
                 eWorldAction,
                 strType, 
                 strReason,
                 params, 
                 nInvocationCount,
                 &result); 

  // if the function returns a non-zero result, don't go ahead
  if (result.vt != VT_EMPTY)
    {
    result.ChangeType (VT_I4);  // make a long
    if (result.vt == VT_I4)   // conversion successful
      if (result.lVal)        // return if non-zero
        return true;
    }

  return false;
  } // end of CMUSHclientDoc::MXP_StartTagScript 
