// TimerWnd.cpp : implementation file
//

#include "stdafx.h"
#include "mushclient.h"
#include "doc.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

/////////////////////////////////////////////////////////////////////////////
// CTimerWnd

CTimerWnd::CTimerWnd(CMUSHclientDoc * pDoc)
{
  m_pDoc = pDoc;
  m_iTimer = 0;
  m_bProcessingQueue = false;
  m_bDrainQueue = false;
}

CTimerWnd::~CTimerWnd()
{
}


BEGIN_MESSAGE_MAP(CTimerWnd, CWnd)
	//{{AFX_MSG_MAP(CTimerWnd)
	ON_WM_TIMER()
	ON_WM_DESTROY()
	//}}AFX_MSG_MAP
END_MESSAGE_MAP()


/////////////////////////////////////////////////////////////////////////////
// CTimerWnd message handlers

void CTimerWnd::OnTimer(UINT nIDEvent) 
{
  DrainQueue (true);

  if (m_iTimer && m_pDoc->m_iSpeedWalkDelay == 0)
    {
    KillTimer (m_iTimer);
    m_iTimer = 0;
    }
}

void CTimerWnd::DrainQueue (const bool bStopAfterDelayedCommand)
{
  if (m_bProcessingQueue)
    {
    if (!bStopAfterDelayedCommand)
      m_bDrainQueue = true;
    return;
    }

  // no queued commands - don't update status line
  if (m_pDoc->m_QueuedCommandsList.IsEmpty ())
    {
    m_bDrainQueue = false;
    return;
    }

  m_bProcessingQueue = true;
  bool bStopAfterDelay = bStopAfterDelayedCommand;

  try
    {
    while (!m_pDoc->m_QueuedCommandsList.IsEmpty ())
      {
      CString strCommand = m_pDoc->m_QueuedCommandsList.RemoveHead ();

      unsigned char cQueueFlags = (unsigned char) strCommand [0];
      bool bSuppressPluginSend =
        (cQueueFlags & QUEUE_SUPPRESS_PLUGIN_SEND) != 0;
      char cMessageType =
        (char) (cQueueFlags & ~QUEUE_SUPPRESS_PLUGIN_SEND);
      bool bEcho = toupper ((unsigned char) cMessageType) == QUEUE_WITH_ECHO ||
                   toupper ((unsigned char) cMessageType) == IMMEDIATE_WITH_ECHO;
      bool bLog = cMessageType >= 'A';

      if (bSuppressPluginSend)
        {
        CBoolStateGuard processingGuard
          (m_pDoc->m_bPluginProcessingSend, true);
        m_pDoc->DoSendMsg (strCommand.Mid (1), bEcho, bLog);
        }
      else
        m_pDoc->DoSendMsg (strCommand.Mid (1), bEcho, bLog);

      if (m_bDrainQueue)
        {
        bStopAfterDelay = false;
        m_bDrainQueue = false;
        }

      if (bStopAfterDelay &&
          (toupper ((unsigned char) cMessageType) == QUEUE_WITH_ECHO ||
           toupper ((unsigned char) cMessageType) == QUEUE_WITHOUT_ECHO))
        break;    // if we need to wait, don't keep pulling them out
      }
    }
  catch (...)
    {
    bool bDrainQueue = m_bDrainQueue || !bStopAfterDelayedCommand;
    m_bProcessingQueue = false;
    m_bDrainQueue = bDrainQueue;
    if (bDrainQueue && !m_pDoc->m_QueuedCommandsList.IsEmpty () && !m_iTimer)
      m_iTimer = SetTimer (COMMAND_QUEUE_TIMER_ID,
                           MAX ((int) m_pDoc->m_iSpeedWalkDelay, 1), NULL);
    throw;
    }

  m_bProcessingQueue = false;
  m_bDrainQueue = false;
  m_pDoc->ShowQueuedCommands ();    // update status line
}

void CTimerWnd::OnDestroy() 
{
  if (m_iTimer)
      KillTimer (m_iTimer);

  CWnd::OnDestroy();
	
}

void CTimerWnd::ChangeTimerRate (const int iRate)
  {
  int iNewRate = iRate;

  // make sure valid
  if (iNewRate < 0)
    iNewRate = 0;

  if (iNewRate > 30000)
    iNewRate = 30000;

  // get rid of old timer
  if (m_iTimer)
    {
    KillTimer (m_iTimer);
    m_iTimer = 0;
    }

  // if zero, no timer wanted
  if (iNewRate)
    m_iTimer = SetTimer(COMMAND_QUEUE_TIMER_ID, iNewRate, NULL); 
  else
    DrainQueue (false);

  }  // end of ChangeTimerRate

