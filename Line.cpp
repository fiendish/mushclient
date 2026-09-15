#include "stdafx.h"
#include "MUSHclient.h"

#include "doc.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char BASED_CODE THIS_FILE[] = __FILE__;
#endif

/*

  CLine is the class for "lines" from the MUSH, which are shown in the output
  window.

  By making them a class we can produce a collection class which is a linked
  list of CLine's.

  Also, the constructor allocates memory for the text of the line and its first
  style item, and the destructor de-allocates the line's memory, and the style list.

*/

// CLine constructor

CLine::CLine (const long nLineNumber, 
              const unsigned int nWrapColumn,
              const unsigned short iFlags,      
              const COLORREF       iForeColour,
              const COLORREF       iBackColour,
              const bool bUnicode
              )
  {
  hard_return = false;
  len = 0;
  m_iPreambleOffset = 0;
  m_theTime = CTime::GetCurrentTime(); 
  QueryPerformanceCounter (&m_lineHighPerformanceTime);
  nCreationNumber = App.GetUniqueNumber ();
  m_nLineNumber = nLineNumber;

  if (bUnicode)
    iMemoryAllocated = nWrapColumn * 4;
  else
    iMemoryAllocated = nWrapColumn;

  // allocate 4 bytes per character for UTF-8

#ifdef USE_REALLOC
  text = (char *) malloc (iMemoryAllocated);
#else
  text = new char [iMemoryAllocated];
#endif
  ASSERT (text);
  if (!text)
    AfxThrowMemoryException ();
  flags = 0;      // no special flags yet (ie. normal output line)

  try
    {
    std::unique_ptr<CStyle> pStyle (NEWSTYLE);
    pStyle->iFlags = iFlags;
    pStyle->iForeColour = iForeColour;
    pStyle->iBackColour = iBackColour;

    // have at least one style item in the list
    styleList.AddTail (pStyle.get ());
    pStyle.release ();
    }
  catch (...)
    {
#ifdef USE_REALLOC
    free (text);
#else
    delete [] text;
#endif
    text = NULL;
    throw;
    }

  }   // end of CLine::CLine

// CLine destructor

CLine::~CLine ()
  {
#ifdef USE_REALLOC
  free (text);
#else
  delete [] text;
#endif

// delete styles list

  for (POSITION pos = styleList.GetHeadPosition(); pos; )
      DELETESTYLE (styleList.GetNext (pos));
  
  styleList.RemoveAll();

  }

void CLine::ResizeText (const int iNewSize)
  {
  ASSERT (iNewSize >= len);

#ifdef USE_REALLOC
  char * pNewText = (char *) realloc (text, iNewSize);
  if (!pNewText)
    AfxThrowMemoryException ();
#else
  char * pNewText = new char [iNewSize];
  if (!pNewText)
    AfxThrowMemoryException ();
  memcpy (pNewText, text, len);
  delete [] text;
#endif

  text = pNewText;
  iMemoryAllocated = iNewSize;
  } // end of CLine::ResizeText

// for tracking down style allocation errors

CStyle * GetNewStyle (const char * filename, const long linenumber)
  {
  CStyle * pNewStyle = new CStyle;
  pNewStyle->nCreationNumber = App.GetUniqueNumber ();
  pNewStyle->nRangeCreationNumber = pNewStyle->nCreationNumber;
  TRACE3 ("new CStyle at %p at file %s line %ld\n",
          pNewStyle,
          filename,
          linenumber);

  return pNewStyle;
  }

void DeleteStyle (CStyle * pStyle, const char * filename, const long linenumber)
  {
  TRACE3 ("delete CStyle at %p at file %s line %ld\n",
          pStyle,
          filename,
          linenumber);

  delete pStyle;
  }
