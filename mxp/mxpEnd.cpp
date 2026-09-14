// mxpEnd.cpp - MXP end tag

// eg. </send>

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



void CMUSHclientDoc::MXP_EndTag (CString strTag)
  {

bool bSecure = MXP_Secure () ;

  MXP_Restore_Mode ();  // cancel secure-once mode

CString strName;

  GetWord (strName, strTag);

  if (!IsValidName (strName))
    {
    MXP_error (DBG_ERROR, errMXP_InvalidElementName,
              TFormat ("Invalid MXP tag name: </%s>", 
              (LPCTSTR) m_strMXPstring)); 
    return;
    }

  strName.MakeLower (); // case insensitive

  const CString strClosingArguments = strTag;
  const bool bWarnClosingArguments = !strClosingArguments.IsEmpty ();


  // make sure tag is in active taglist
  // eg. </unknown> will not close any open tags
  // this test effectively checks that tag is known (otherwise it won't be in the list)

  CActiveTag * pTag = NULL;
  size_t iTagsToClose = 0;

  for (POSITION pos = m_ActiveTagList.GetTailPosition (); pos; )
    {
    pTag = m_ActiveTagList.GetPrev (pos);
    iTagsToClose++;

    if (pTag->strName == strName)
      break;
    else
      {
      // check we don't cross over some secure tags when finding it
      if (!bSecure && pTag->bSecure)
        {
        const CString strBlockingTag = pTag->strName;
        if (bWarnClosingArguments)
          MXP_error (DBG_WARNING, wrnMXP_ArgumentsToClosingTag,
                    TFormat ("Closing MXP tag </%s %s> has inappropriate arguments",
                             (LPCTSTR) strName,
                             (LPCTSTR) strClosingArguments));
        MXP_error (DBG_WARNING, wrnMXP_OpenTagBlockedBySecureTag,
                  TFormat ("Cannot close open MXP tag <%s> "
                                         "- blocked by secure tag <%s>", 
                                        (LPCTSTR) strName,
                                        (LPCTSTR) strBlockingTag));
        return;
        }
      pTag = NULL;
      }

    } // end of doing each one

  if (!pTag)
    {
    if (bWarnClosingArguments)
      MXP_error (DBG_WARNING, wrnMXP_ArgumentsToClosingTag,
                TFormat ("Closing MXP tag </%s %s> has inappropriate arguments",
                         (LPCTSTR) strName,
                         (LPCTSTR) strClosingArguments));
    MXP_error (DBG_WARNING, wrnMXP_OpenTagNotThere,
              TFormat ("Closing MXP tag </%s> does not have corresponding opening tag", 
              (LPCTSTR) strName)); 
    return;
    }

  if (!bSecure && pTag->bSecure)
    {
    if (bWarnClosingArguments)
      MXP_error (DBG_WARNING, wrnMXP_ArgumentsToClosingTag,
                TFormat ("Closing MXP tag </%s %s> has inappropriate arguments",
                         (LPCTSTR) strName,
                         (LPCTSTR) strClosingArguments));
    MXP_error (DBG_WARNING, wrnMXP_TagOpenedInSecureMode,
              TFormat ("Cannot close open MXP tag <%s> "
                       "- it was opened in secure mode.", 
                      (LPCTSTR) strName)); 
    return;
    }

  // we know it is in the list - close all tags until we reach this one
  // eg.  <b> <i> </b> </i>  
  //  in the above example the </b> will also close the <i>

  struct CTagToClose
    {
    __int64 iCreationNumber;
    __int64 iOpeningStyleCreationNumber;
    CString strName;
    vector<int> closeActions;
    };
  vector<CTagToClose> tagsToClose;
  tagsToClose.reserve (iTagsToClose);
  POSITION snapshotPosition = m_ActiveTagList.GetTailPosition ();
  for (size_t i = 0; i < iTagsToClose; i++)
    {
    CActiveTag * pSnapshotTag = m_ActiveTagList.GetPrev (snapshotPosition);
    CTagToClose tagToClose;
    tagToClose.iCreationNumber = pSnapshotTag->nCreationNumber;
    tagToClose.iOpeningStyleCreationNumber =
      pSnapshotTag->nOpeningStyleCreationNumber;
    tagToClose.strName = pSnapshotTag->strName;
      tagToClose.closeActions = pSnapshotTag->closeActions;
      tagsToClose.push_back (tagToClose);
    }

  struct CPreparedTagClose
    {
    CPreparedMXPClose close;
    bool bOutOfSequence;
    };
  vector<CPreparedTagClose> preparedCloses;
  preparedCloses.reserve (tagsToClose.size ());
  exception_ptr prepareException;

  for (vector<CTagToClose>::const_iterator it = tagsToClose.begin ();
       it != tagsToClose.end ();
       ++it)
    {
    CActiveTag * pDetachedTag = NULL;
    POSITION tagPosition = NULL;
    for (POSITION pos = m_ActiveTagList.GetTailPosition (); pos; )
      {
      POSITION current = pos;
      CActiveTag * pCandidate = m_ActiveTagList.GetPrev (pos);
      if (pCandidate->nCreationNumber == it->iCreationNumber)
        {
        pDetachedTag = pCandidate;
        tagPosition = current;
        break;
        }
      }

    if (!pDetachedTag)
      continue;

    bool bCloseSlotAdded = false;
    bool bPrepared = false;
    try
      {
      CPreparedTagClose preparedTagClose;
      preparedTagClose.bOutOfSequence = false;
      preparedCloses.push_back (preparedTagClose);
      bCloseSlotAdded = true;
      CPreparedTagClose & publishedClose = preparedCloses.back ();
      publishedClose.bOutOfSequence = it->strName != strName;
      bPrepared = MXP_PrepareCloseTag (it->strName,
                                       false,
                                       it->iOpeningStyleCreationNumber,
                                       it->closeActions,
                                       pDetachedTag,
                                       publishedClose.close);
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
    delete pDetachedTag;
    }

  // All target styles are now restored. Callbacks cannot open a tag between
  // two restores and make it cross an outer close.
  exception_ptr finishException;
  if (bWarnClosingArguments)
    try
      {
      MXP_error (DBG_WARNING, wrnMXP_ArgumentsToClosingTag,
                TFormat ("Closing MXP tag </%s %s> has inappropriate arguments",
                         (LPCTSTR) strName,
                         (LPCTSTR) strClosingArguments));
      }
    catch (...)
      {
      finishException = current_exception ();
      }

  for (vector<CPreparedTagClose>::const_iterator it = preparedCloses.begin ();
       it != preparedCloses.end (); ++it)
    {
    if (it->bOutOfSequence)
      try
        {
        MXP_error (DBG_WARNING, wrnMXP_ClosingOutOfSequenceTag,
                  TFormat ("Closing out-of-sequence MXP tag: <%s>",
                           (LPCTSTR) it->close.strTag));
        }
      catch (...)
        {
        if (!finishException)
          finishException = current_exception ();
        }

    try
      {
      MXP_FinishCloseTag (it->close);
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

  } // end of CMUSHclientDoc::MXP_EndTag
