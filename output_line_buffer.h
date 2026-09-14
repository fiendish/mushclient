// Reserve line storage before callbacks so restoration does not allocate.
#pragma once

class COutputLineBuffer
  {
  public:
    explicit COutputLineBuffer (const int iCapacity) :
      m_pText (NULL), m_iCapacity (iCapacity)
      {
#ifdef USE_REALLOC
      m_pText = static_cast<char *> (malloc (iCapacity));
#else
      m_pText = new char [iCapacity];
#endif
      if (!m_pText)
        AfxThrowMemoryException ();
      }

    ~COutputLineBuffer ()
      {
#ifdef USE_REALLOC
      free (m_pText);
#else
      delete [] m_pText;
#endif
      }

    void RestoreCapacity (CLine * pLine)
      {
      if (pLine->iMemoryAllocated >= m_iCapacity)
        return;
      // Keep any callback changes to the retained prefix.
      memcpy (m_pText, pLine->text, pLine->len);
      std::swap (m_pText, pLine->text);
      std::swap (m_iCapacity, pLine->iMemoryAllocated);
      }

  private:
    COutputLineBuffer (const COutputLineBuffer &);
    COutputLineBuffer & operator= (const COutputLineBuffer &);
    char * m_pText;
    int m_iCapacity;
  };
