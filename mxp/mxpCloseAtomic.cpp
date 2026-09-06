// mxpCloseAtomic.cpp - close one atomic tag

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

static void RebaseClosingStateOwners (
  CActiveTagList & activeTags,
  const int iAction,
  const CPreparedMXPClose & preparedClose)
  {
  for (POSITION pos = activeTags.GetHeadPosition (); pos; )
    {
    CActiveTag * pTag = activeTags.GetNext (pos);
    switch (iAction)
      {
      case MXP_ACTION_P:
        if (pTag->iOpeningParagraphOwner ==
            preparedClose.iActiveTagCreationNumber)
          {
          pTag->bOpeningInParagraph = preparedClose.bOpeningInParagraph;
          pTag->iOpeningParagraphOwner =
            preparedClose.iOpeningParagraphOwner;
          }
        break;

      case MXP_ACTION_PRE:
        if (pTag->iOpeningPreOwner ==
            preparedClose.iActiveTagCreationNumber)
          {
          pTag->bOpeningPreMode = preparedClose.bOpeningPreMode;
          pTag->iOpeningPreOwner = preparedClose.iOpeningPreOwner;
          }
        break;

      case MXP_ACTION_SCRIPT:
        if (pTag->iOpeningScriptOwner ==
            preparedClose.iActiveTagCreationNumber)
          {
          pTag->bOpeningMXPScript = preparedClose.bOpeningMXPScript;
          pTag->iOpeningScriptOwner = preparedClose.iOpeningScriptOwner;
          }
        break;

      case MXP_ACTION_UL:
      case MXP_ACTION_OL:
        if (pTag->iOpeningListOwner ==
            preparedClose.iActiveTagCreationNumber)
          {
          pTag->iOpeningListMode = preparedClose.iOpeningListMode;
          pTag->iOpeningListCount = preparedClose.iOpeningListCount;
          pTag->iOpeningListOwner = preparedClose.iOpeningListOwner;
          }
        break;
      }
    }
  }

struct CStagedMXPActionStyle
  {
  CStagedMXPActionStyle () : pStyle (NULL), pAction (NULL),
    iFlags (0), iForeColour (0), iBackColour (0) { }
  ~CStagedMXPActionStyle ()
    {
    if (pAction)
      pAction->Release ();
    }

  CStyle * pStyle;
  CAction * pAction;
  unsigned short iFlags;
  COLORREF iForeColour;
  COLORREF iBackColour;
  };

// do the action required to close a single atomic tag (iAction)
void CMUSHclientDoc::MXP_CloseAtomicTag (
  const int iAction,
  const CPreparedMXPClose & preparedClose)
  {
  const CString & strText = preparedClose.strText;
  const set<__int64> & contentStyleRangeNumbers =
    preparedClose.contentStyleRangeNumbers;
  POSITION linepos, 
           stylepos;

  RebaseClosingStateOwners (m_ActiveTagList, iAction, preparedClose);

  // now take any closing action 
  switch (iAction)
    {
    case MXP_ACTION_SEND: 
    case MXP_ACTION_HYPERLINK:

      {
      vector<std::unique_ptr<CStagedMXPActionStyle> > stagedStyles;

      for (linepos = m_LineList.GetHeadPosition (); linepos; )
        {
        CLine * pLine2 = m_LineList.GetNext (linepos);
        stylepos = pLine2->styleList.GetHeadPosition ();

        for ( ; stylepos; )
          {
          CStyle * pStyle2 = pLine2->styleList.GetNext (stylepos);
          if (contentStyleRangeNumbers.find (
                pStyle2->nRangeCreationNumber) ==
              contentStyleRangeNumbers.end ())
            continue;
          if ((pStyle2->iFlags & ACTIONTYPE) &&
              (pStyle2->iFlags & START_TAG) == 0)
            {
            CString strAction;
            CString strHint;
            CString strVariable;
            if (pStyle2->pAction)
              {
              strAction = pStyle2->pAction->m_strAction;
              strHint = pStyle2->pAction->m_strHint;
              strVariable = pStyle2->pAction->m_strVariable;
              }

            std::unique_ptr<CStagedMXPActionStyle> pStaged
              (new CStagedMXPActionStyle);
            pStaged->pStyle = pStyle2;
            pStaged->iFlags = pStyle2->iFlags;
            pStaged->iForeColour = pStyle2->iForeColour;
            pStaged->iBackColour = pStyle2->iBackColour;

            if (m_bUseCustomLinkColour && !m_bMudCanChangeLinkColour)
              {
              COLORREF colour1,
                       colour2;

              // find current foreground and background RGB values
              GetStyleRGB (pStyle2, colour1, colour2);

              pStaged->iForeColour = m_iHyperlinkColour;
              pStaged->iBackColour = colour2;
              pStaged->iFlags &= ~COLOURTYPE;
              pStaged->iFlags |= COLOUR_RGB;
              } // end of changing colour back to wanted link colour

            if (m_bUnderlineHyperlinks && !m_bMudCanRemoveUnderline)
              pStaged->iFlags |= UNDERLINE;

            if (strAction.IsEmpty ())
              { // no action defined - use &text; as the action
              pStaged->pAction = GetAction (strText, strHint, strVariable);
              }
            else
              {   // we have an strAction already
              // replace the &text; sequence
              strAction.Replace ("&text;", strText);
              strHint.Replace ("&text;", strText);
              pStaged->pAction = GetAction (strAction, strHint, strVariable);
              } // end of having text to send

            stagedStyles.push_back (std::move (pStaged));
            }  // end of being an ordinary "send" style
          } // end of doing each style on this line
        } // end of doing each line

      for (vector<std::unique_ptr<CStagedMXPActionStyle> >::iterator it =
             stagedStyles.begin (); it != stagedStyles.end (); ++it)
        {
        CStagedMXPActionStyle * pStaged = it->get ();
        CAction * pOldAction = pStaged->pStyle->pAction;
        pStaged->pStyle->iFlags = pStaged->iFlags;
        pStaged->pStyle->iForeColour = pStaged->iForeColour;
        pStaged->pStyle->iBackColour = pStaged->iBackColour;
        pStaged->pStyle->pAction = pStaged->pAction;
        pStaged->pAction = NULL;
        if (pOldAction)
          pOldAction->Release ();
        }
      }
      break;  // end of MXP_ACTION_SEND or MXP_ACTION_HYPERLINK

      // end script
    case MXP_ACTION_SCRIPT:
      if (m_iMXPScriptOwner == preparedClose.iActiveTagCreationNumber)
        {
        m_bMXP_script = preparedClose.bOpeningMXPScript;
        m_iMXPScriptOwner = preparedClose.iOpeningScriptOwner;
        }
      MXP_error (DBG_INFO, infoMXP_ScriptCollectionCompleted,
                "Script collection mode completed.");
      break;  // end of MXP_ACTION_SCRIPT

      // end new para
    case MXP_ACTION_P:
       if (m_iMXPParagraphOwner == preparedClose.iActiveTagCreationNumber)
         {
         m_bInParagraph = preparedClose.bOpeningInParagraph;
         m_iMXPParagraphOwner = preparedClose.iOpeningParagraphOwner;
         }
       if (!StartNewLine (true, 0))
         return;
       break;  // end of MXP_ACTION_P

    case MXP_ACTION_PRE: 
      if (m_iMXPPreOwner == preparedClose.iActiveTagCreationNumber)
        {
        m_bPreMode = preparedClose.bOpeningPreMode;
        m_iMXPPreOwner = preparedClose.iOpeningPreOwner;
        }
      break;  // end of MXP_ACTION_PRE

     case MXP_ACTION_UL:   
       if (m_iMXPListOwner == preparedClose.iActiveTagCreationNumber)
         {
         m_iListMode = preparedClose.iOpeningListMode;
         m_iListCount = preparedClose.iOpeningListCount;
         m_iMXPListOwner = preparedClose.iOpeningListOwner;
         }
       // wrap up previous line if necessary
       if (m_pCurrentLine->len > 0)
          if (!StartNewLine (true, 0))
            return;
       break;  // end of MXP_ACTION_UL
     
     case MXP_ACTION_OL:   
       if (m_iMXPListOwner == preparedClose.iActiveTagCreationNumber)
         {
         m_iListMode = preparedClose.iOpeningListMode;
         m_iListCount = preparedClose.iOpeningListCount;
         m_iMXPListOwner = preparedClose.iOpeningListOwner;
         }
       // wrap up previous line if necessary
       if (m_pCurrentLine->len > 0)
          if (!StartNewLine (true, 0))
            return;

       break;  // end of MXP_ACTION_OL

      case MXP_ACTION_VAR: 
        {
        }
        break;
    } // end of switch on action type

  } // end of CMUSHclientDoc::MXP_CloseAtomicTag


