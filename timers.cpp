// timers.cpp

// timer checking / resetting


#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"
#include "mainfrm.h"
#include "sendvw.h"

unsigned long CTimer::nNextCreateSequence = 0;

void CMUSHclientDoc::ResetOneTimer (CTimer * timer_item)
  {
CmcDateTime tNow = CmcDateTime::GetTimeNow();
CmcDateTimeSpan tsOneDay (1, 0, 0, 0);

  if (!timer_item->bEnabled)    // ignore un-enabled timers
    return;

// so we can see when it is likely to fire next 

  timer_item->tWhenFired = tNow;

// for timers that go off "at" a time, find today's date, and move the time in

  if (timer_item->iType == CTimer::eAtTime)
    {

    timer_item->tFireTime = CmcDateTime (tNow.GetYear (), tNow.GetMonth (), 
                                         tNow.GetDay (),  timer_item->iAtHour, 
                                         timer_item->iAtMinute, timer_item->fAtSecond);

// if this time has passed, go onto tomorrow

    if (timer_item->tFireTime < tNow)
      timer_item->tFireTime += tsOneDay;
    }
  else    // for periodic timers, find "now" and add the period to it
    timer_item->tFireTime = tNow + CmcDateTimeSpan (0,    // now plus the interval
                                              timer_item->iEveryHour, 
                                              timer_item->iEveryMinute, 
                                              timer_item->fEverySecond)
                                 - CmcDateTimeSpan (0,    // minus the offset
                                              timer_item->iOffsetHour, 
                                              timer_item->iOffsetMinute, 
                                              timer_item->fOffsetSecond);

  } // end of CMUSHclientDoc::ResetOneTimer

void CMUSHclientDoc::ResetAllTimers (CTimerMap & TimerMap)
  {
  CTimer * pTimer;
  CString strTimerName;

  for (POSITION pos = TimerMap.GetStartPosition(); pos; )
    {
    TimerMap.GetNextAssoc (pos, strTimerName, pTimer);
    ResetOneTimer (pTimer);
    }

  } // end of CMUSHclientDoc::ResetAllTimers

void CMUSHclientDoc::CheckTimerList (CTimerMap & TimerMap)
  {
CTimer * timer_item;
CString strTimerName;
CmcDateTime tNow = CmcDateTime::GetTimeNow();
CmcDateTimeSpan tsOneDay (1, 0, 0, 0);

// check for deleted chat sessions

  for (POSITION chatpos = m_ChatList.GetHeadPosition (); chatpos; )
    {
    POSITION oldpos = chatpos;
    CChatSocket * pSocket = m_ChatList.GetNext (chatpos);
    if (pSocket->m_bDeleteMe)
      {
      m_ChatList.RemoveAt (oldpos);
      delete pSocket;
      break;    // list is no longer valid
      }
    }

  map <string, __int64> firedTimersList;
  POSITION pos;

// iterate through all timers for this document - first build list of them

  for (pos = TimerMap.GetStartPosition(); pos; )
    {


    TimerMap.GetNextAssoc (pos, strTimerName, timer_item);

    if (!timer_item->bEnabled || timer_item->bExecutingScript)
      continue;

    // no timer activity whilst closed or in the middle of connecting, or if not enabled

    if (!timer_item->bActiveWhenClosed)
      if (m_iConnectPhase != eConnectConnectedToMud)
        continue;

    // if not ready to fire yet, ignore it

    if (timer_item->tFireTime > tNow)
      continue;

    firedTimersList [(LPCTSTR) strTimerName] = timer_item->nCreationNumber;
    }


  // now process list, checking timer still exists in case a script deleted one
  // see: http://www.gammon.com.au/forum/?id=10358

  for (map <string, __int64>::iterator it = firedTimersList.begin ();
       it != firedTimersList.end ();
       it++)
    {
    // get next fired timer from list
    strTimerName = it->first.c_str ();

    // check still exists, get pointer if so
    if (!TimerMap.Lookup (strTimerName, timer_item))
      continue;

    // A previous timer callback can replace, disable, or reschedule this
    // timer. Fire only the same timer instance that was ready in the snapshot.
    if (timer_item->nCreationNumber != it->second ||
        !timer_item->bEnabled ||
        timer_item->bExecutingScript)
      continue;
    if (!timer_item->bActiveWhenClosed &&
        m_iConnectPhase != eConnectConnectedToMud)
      continue;
    if (timer_item->tFireTime > tNow)
      continue;

    CPluginCallGuard pluginCallGuard (m_CurrentPlugin, true);
    CString strTimerLabel = timer_item->strLabel;
    if (strTimerLabel.IsEmpty ())
      strTimerLabel = strTimerName;
    __int64 iFiredTimerCreationNumber = timer_item->nCreationNumber;

    {
    CTimerExecutionGuard executingGuard (this, timer_item);

    timer_item->nMatched++;   // count timer matches
    timer_item->tWhenFired = tNow;  // when it fired

    m_iTimersFiredCount++;
    m_iTimersFiredThisSessionCount++;

//    TRACE1 ("Fired at = %10.8f\n", timer_item->tWhenFired.m_dt);

    if (timer_item->strLabel.IsEmpty ())
      Trace ("Fired unlabelled timer ");
    else
      Trace ("Fired timer %s", (LPCTSTR) timer_item->strLabel);

//    TRACE1 ("Fire time = %10.8f\n", timer_item->tFireTime.m_dt);

// update fire time - before calling the script, in case it takes a long time

    if (timer_item->iType == CTimer::eAtTime)
      timer_item->tFireTime += tsOneDay;
    else
      timer_item->tFireTime += CmcDateTimeSpan (0,    // add the interval
                                          timer_item->iEveryHour, 
                                          timer_item->iEveryMinute, 
                                          timer_item->fEverySecond);

    // in case clock changes or some such thing, make sure timer will be due to
    // fire in the future, not the past, or it might go mad and keep firing

    if (timer_item->tFireTime <= tNow)
      ResetOneTimer (timer_item);

    // if one-shot, disable it, so if the timer routine finds it again while
    // it is still executing (eg. due to a syntax error dialog box) then
    // it won't fire again.

    if (timer_item->bOneShot)
      timer_item->bEnabled = false;


    // send timer message, if this timer list is "active"

    CString strExtraOutput;
    CValueStateGuard<unsigned short> actionSourceGuard
      (m_iCurrentActionSource, eTimerFired);
    SendTo (timer_item->iSendTo, 
            timer_item->strContents, 
            timer_item->bOmitFromOutput, // omit from output
            timer_item->bOmitFromLog,    // omit from log
            TFormat ("Timer: %s", (LPCTSTR) timer_item->strLabel),
            timer_item->strVariable,
            strExtraOutput
            );

    // display any stuff sent to output window

    if (!strExtraOutput.IsEmpty ())
       DisplayMsg (strExtraOutput, strExtraOutput.GetLength (), COMMENT);

// invoke script subroutine, if any

    if (!timer_item->strProcedure.IsEmpty ())
      if (CheckScriptingAvailable ("Timer", timer_item->dispid, timer_item->strProcedure))
         continue;

    if (timer_item->dispid != DISPID_UNKNOWN)        // if we have a dispatch id
      {
      
      CString strType = "timer";
      CString strReason =  TFormat ("processing timer \"%s\"", 
                                    (LPCTSTR) timer_item->strLabel);

      // get unlabelled timer's internal name
      const char * pLabel = strTimerLabel;

      if (GetScriptEngine () && GetScriptEngine ()->IsLua ())
        {
        list<double> nparams;
        list<string> sparams;
        sparams.push_back (pLabel);
        CTimerExecutionGuard executingGuard (this, timer_item);
        GetScriptEngine ()->ExecuteLua (timer_item->dispid, 
                                       timer_item->strProcedure, 
                                       eTimerFired,
                                       strType, 
                                       strReason,
                                       nparams,
                                       sparams, 
                                       timer_item->nInvocationCount);
        }   // end of Lua
      else
        {
        // prepare for the arguments (so far, 1 which is the timer name)
  
        // WARNING - arguments should appear in REVERSE order to what the sub expects them!

        enum
          {
          eTimerName,
          eArgCount,     // this MUST be last
          };    

        COleVariant args [eArgCount];
        DISPPARAMS params = { args, NULL, eArgCount, 0 };

  //      args [eTimerName] = strTimerName;
        args [eTimerName] = pLabel;
        CTimerExecutionGuard executingGuard (this, timer_item);
        ExecuteScript (timer_item->dispid,  
                       timer_item->strProcedure,
                       eTimerFired,
                       strType, 
                       strReason,
                       params, 
                       timer_item->nInvocationCount);
        } // not Lua
      }     // end of having a dispatch ID

    }


    // If they passed the wrong arguments to the timer routine, the dialog box
    // might appear, and the timer be deleted, before we get a chance to
    // do this code, in which case the timer has gone.
    // Just get it again to be sure ...  [#430]

    if (!TimerMap.Lookup (strTimerName, timer_item))
      return;

// if one-shot timer, delete from list

    if (timer_item->nCreationNumber == iFiredTimerCreationNumber &&
        timer_item->bOneShot)
      {
      set<CTimer *> timersToDelete;
      timersToDelete.insert (timer_item);
      SortTimers (&timersToDelete);
      VERIFY (TimerMap.RemoveKey (strTimerName));
      delete timer_item;
      }
    }   // end of processing each timer

  } // end of CMUSHclientDoc::CheckTimerMap



void CMUSHclientDoc::CheckTimers ()
  {

  // make sure status line is showing the right thing (after 5 seconds)
  if (m_pActiveCommandView || m_pActiveOutputView)
    {
    CTimeSpan ts = CTime::GetCurrentTime() - m_tStatusDisplayed;
    if (ts.GetTotalSeconds () > 5)
      ShowStatusLine ();
    }   // end of this being the active world

  // flush log file to disk every 2 minutes
  if (m_logfile)
    {
    CTimeSpan ts = CTime::GetCurrentTime() - m_LastFlushTime;

    // flush every 2 minutes
    if (ts.GetTotalSeconds () > 120.0)
      {
      m_LastFlushTime = CTime::GetCurrentTime();
//      fflush (m_logfile);
      
      fclose (m_logfile);
      m_logfile = fopen (m_logfile_name, "a+");

      }

    }   // end of log file open
  
// if reconnection wanted, attempt it now ...

// if they want automatic reconnection, do it

  if (App.m_bReconnectOnLinkFailure && 
      !m_bDisconnectOK &&
      m_iConnectPhase == eConnectNotConnected)
    {
    Frame.SetStatusMessage (Translate ("Reconnecting ..."));
    ConnectSocket();      // reconnect
    }

  if (m_bEnableTimers)
    {

  // timer has kicked in unexpectedly - ignore it
  if (m_CurrentPlugin)
    return;

  CPluginContextGuard timerContextGuard (this, NULL);

   // Do only the plugin instances that existed at the start of this tick.
   CPluginInstanceSnapshot plugins;
   GetPluginInstanceSnapshot (m_PluginList, plugins);
  CheckTimerList (GetTimerMap ());
   for (size_t iPlugin = 0; iPlugin < plugins.size (); iPlugin++)
      {
      m_CurrentPlugin = GetPluginInstance (
        plugins [iPlugin].m_strID,
        plugins [iPlugin].m_iPluginInstanceNumber);
      if (!m_CurrentPlugin)
        continue;
      if (m_CurrentPlugin->m_bEnabled)
        CheckTimerList (GetTimerMap ());
      } // end of doing each plugin
    m_CurrentPlugin = NULL;
    }
  } // end of CMUSHclientDoc::CheckTimers

void CMUSHclientDoc::CheckTickTimers ()
  {

  // timer has kicked in unexpectedly - ignore it
  if (m_CurrentPlugin)
    return;

  // check for selection change in command window
  // I know, this is a crappy way of doing it, but the CEditView does 
  // not notify of selection changes.

  for(POSITION commandpos=GetFirstViewPosition();commandpos!=NULL;)
    {
    CView* pView = GetNextView(commandpos);

    if (pView->IsKindOf(RUNTIME_CLASS(CSendView)))
      {
      CSendView* pmyView = (CSendView*)pView;

      pmyView->CheckForSelectionChange ();
      }	  // end of being a CSendView
    }

  SendToAllPluginCallbacks (ON_PLUGIN_TICK);

  // for output window fading
  if (m_iFadeOutputBufferAfterSeconds > 0 &&
    CTime::GetCurrentTime ().GetTime () - m_timeLastWindowDraw.GetTime () > 0)
    Redraw ();


  } // end of CMUSHclientDoc::CheckTickTimers


void CMUSHclientDoc::OnGameResetalltimers() 
{
  CPluginContextGuard timerContextGuard (this, NULL);

  ResetAllTimers (GetTimerMap ());
  // do plugins
 for (PluginListIterator pit = m_PluginList.begin (); 
       pit != m_PluginList.end (); 
       ++pit)
    {
    m_CurrentPlugin = *pit;
    if (m_CurrentPlugin->m_bEnabled)
      ResetAllTimers (GetTimerMap ());
    } // end of doing each plugin
  m_CurrentPlugin = NULL;
}

void CMUSHclientDoc::OnUpdateGameResetalltimers(CCmdUI* pCmdUI) 
{
  DoFixMenus (pCmdUI);  // remove accelerators from menus
  pCmdUI->Enable (!m_TimerMap.IsEmpty ());
}


void CMUSHclientDoc::BuildTimerIndex (
  CTimerRevMap & timerRevMap,
  const set<CTimer *> * pExclude)
  {

CString strTimerName;
CTimer * pTimer;
POSITION pos;

  // extract pointers into a simple array
  for (pos = GetTimerMap ().GetStartPosition(); pos; )
    {
     GetTimerMap ().GetNextAssoc (pos, strTimerName, pTimer);
     if (pExclude && pExclude->find (pTimer) != pExclude->end ())
       continue;
     timerRevMap [pTimer] = strTimerName;
    }
  }

void  CMUSHclientDoc::SortTimers (const set<CTimer *> * pExclude)
  {
  CTimerRevMap newTimerRevMap;
  BuildTimerIndex (newTimerRevMap, pExclude);

  GetTimerRevMap ().swap (newTimerRevMap);

  } // end of CMUSHclientDoc::SortTimers


