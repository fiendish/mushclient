// methods_tracing.cpp

// Related to the tracing feature

#include "stdafx.h"
#include "..\..\MUSHclient.h"
#include "..\..\doc.h"
#include "..\errors.h"

// Implements:

//    Trace
//    TraceOut



// routine for doing traces

void CMUSHclientDoc::Trace (LPCTSTR lpszFormat, ...)
{         
  // do nothing if not tracing

  if (!m_bTrace || m_bInTrace || m_bTraceOutputRedraw || !(m_iTraceCategories & eTraceOther))
    return;

  CWorldDocumentOperationGuard operationGuard (this);

	ASSERT(AfxIsValidString(lpszFormat, FALSE));

CString strMsg;

	va_list argList;
	va_start(argList, lpszFormat);
	strMsg.FormatV(lpszFormat, argList);
	va_end(argList);

  TraceForCategory (eTraceOther, strMsg);
}  // end of CMUSHclientDoc::Trace

void CMUSHclientDoc::TraceForCategory (const unsigned int category, const CString & message, const bool continuation)
{
  if (!m_bTrace || m_bInTrace || m_bTraceOutputRedraw || !(m_iTraceCategories & category))
    return;

  CWorldDocumentOperationGuard operationGuard (this);
  CTraceEventGuard eventGuard (this);
  const string plugin = m_CurrentPlugin ? string ((LPCTSTR) m_CurrentPlugin->m_strName) : "";
  m_traceOutput.Add (plugin, category, (LPCTSTR) message);
}  // end of CMUSHclientDoc::TraceForCategory

void CMUSHclientDoc::BeginTraceEvent ()
  {
  ++m_traceOutput.events;
  }

void CMUSHclientDoc::EndTraceEvent ()
  {
  ASSERT (m_traceOutput.events > 0);
  if (--m_traceOutput.events == 0)
    {
    if (std::uncaught_exception ())
      m_traceOutput.entries.clear ();
    else
      FlushTraceEvent ();
    }
  }

void CMUSHclientDoc::FlushTraceEvent ()
  {
  vector<CTraceOutput::Entry> entries;
  entries.swap (m_traceOutput.entries);
  if (entries.empty ())
    return;
  ASSERT (m_traceOutput.parent == CTraceOutput::noParent);
  CValueStateGuard<bool> outputGuard (m_bInTrace, true);
  CValueStateGuard<bool> traceGuard (m_bTrace, false);
  CValueStateGuard<bool> groupGuard (m_bTraceGroupHasOutput, false);
  CValueStateGuard<bool> separatorGuard (m_bTraceGroupNeedsSeparator, false);
  CValueStateGuard<bool> queuedGuard (m_bTraceGroupOutputQueued, false);
  size_t last = entries.size ();
  while (last && entries [last - 1].message.empty ())
    --last;
  vector<unsigned int> depths (entries.size (), 0);
  for (size_t i = 0; i < last; ++i)
    {
    const CTraceOutput::Entry & entry = entries [i];
    if (entry.parent != CTraceOutput::noParent)
      depths [i] = depths [entry.parent] + (entries [entry.parent].message.empty () ? 0 : 1);
    if (entry.message.empty ())
      continue;
    CString strMsg (string (depths [i] * 2, ' ').c_str ());
    if (!entry.plugin.empty ())
      strMsg += TFormat ("Plugin \"%s\": ", entry.plugin.c_str ());
    string context;
    for (size_t ancestor = entry.parent; ancestor != CTraceOutput::noParent &&
         entries [ancestor].message.empty (); ancestor = entries [ancestor].parent)
      {
      const CTraceOutput::Entry & hidden = entries [ancestor];
      if (!hidden.hiddenCallback.empty ())
        {
        const string source = hidden.plugin.empty () ? "" :
          string ("Plugin \"" ) + hidden.plugin + "\": ";
        context = string (" [via ") + source + hidden.hiddenCallback + "]" + context;
        }
      }
    string message = entry.message;
    if (!context.empty ())
      {
      const size_t end = message.find_last_not_of ("\r\n");
      message.insert (end == string::npos ? 0 : end + 1, context);
      }
    strMsg += message.c_str ();
    if (strMsg.Right (2) != ENDLINE)
      {
      if (strMsg.Right (1) == "\n")
        strMsg = strMsg.Left (strMsg.GetLength () - 1);
      strMsg += ENDLINE;
      }
    // The final message also separates this event in trace receivers that log it.
    if (i + 1 == last && strMsg.Right (4) != "\r\n\r\n")
      strMsg += ENDLINE;
    if (!SendToFirstPluginCallbacks (ON_PLUGIN_TRACE, strMsg))
      {
      CString strFullMsg = "TRACE: ";
      strFullMsg += strMsg;
      DisplayMsg (strFullMsg, strFullMsg.GetLength (), COMMENT);
      }
    }
  // A receiver can filter the last entry; still separate the displayed event.
  if (m_bTraceGroupHasOutput && m_bTraceGroupNeedsSeparator)
    {
    if (m_bTraceGroupOutputQueued)
      {
      CPluginNotesGuard notesGuard (this);
      Note (ENDLINE);
      }
    else
      DisplayMsg (ENDLINE, sizeof ENDLINE - 1, COMMENT);
    }
  }

CTraceEventGuard::CTraceEventGuard (CMUSHclientDoc * pDoc, bool independent) :
  m_pDoc (pDoc->m_bInTrace || pDoc->m_bTraceOutputRedraw ? NULL : pDoc),
  m_independent (m_pDoc && independent && pDoc->m_traceOutput.events != 0)
  {
  if (m_pDoc)
    {
    if (m_independent)
      std::swap (m_saved, m_pDoc->m_traceOutput);
    m_pDoc->BeginTraceEvent ();
    }
  }

CTraceEventGuard::~CTraceEventGuard () noexcept(false)
  {
  if (m_pDoc)
    {
    try
      {
      m_pDoc->EndTraceEvent ();
      }
    catch (...)
      {
      if (m_independent)
        std::swap (m_saved, m_pDoc->m_traceOutput);
      throw;
      }
    if (m_independent)
      std::swap (m_saved, m_pDoc->m_traceOutput);
    }
  }

CTraceScope::CTraceScope (CMUSHclientDoc * pDoc, unsigned int category,
                        bool enabled, bool continuation, bool deferred,
                        LPCTSTR procedure) :
  m_event (pDoc), m_pDoc (pDoc), m_previous (pDoc->m_traceOutput.parent),
  m_slot (CTraceOutput::noParent), m_caller (enabled ? pDoc->m_traceOutput.caller : ""),
  m_plugin (enabled && pDoc->m_CurrentPlugin ? string ((LPCTSTR) pDoc->m_CurrentPlugin->m_strName) : ""),
  m_category (category), m_enabled (enabled), m_continuation (continuation)
  {
  pDoc->m_traceOutput.caller.clear ();
  if (enabled)
    {
    if (deferred)
      {
      if (continuation)
        pDoc->m_traceOutput.parent = pDoc->m_traceOutput.MetadataParent (m_plugin, category);
      m_continuation = false;
      }
    else
      StartFunction ();
    }
  // A filtered display callback keeps a parent for its visible calls.
  if (!enabled && category == CMUSHclientDoc::eTraceDisplay && procedure &&
      pDoc->m_bTrace && !pDoc->m_bInTrace && !pDoc->m_bTraceOutputRedraw)
    {
    string callback (procedure);
    m_plugin = pDoc->m_CurrentPlugin ? string ((LPCTSTR) pDoc->m_CurrentPlugin->m_strName) : "";
    m_slot = pDoc->m_traceOutput.Reserve (m_plugin, category, false);
    pDoc->m_traceOutput.entries [m_slot].hiddenCallback.swap (callback);
    pDoc->m_traceOutput.parent = m_slot;
    }
  }

void CTraceScope::StartFunction ()
  {
  if (m_enabled && m_slot == CTraceOutput::noParent)
    {
    m_slot = m_pDoc->m_traceOutput.Reserve (m_plugin, m_category, m_continuation);
    m_pDoc->m_traceOutput.parent = m_slot;
    }
  }

void CTraceScope::FirstLuaCall (void * scope)
  {
  static_cast<CTraceScope *> (scope)->StartFunction ();
  }

CTraceScope::~CTraceScope ()
  {
  m_pDoc->m_traceOutput.parent = m_previous;
  }

void CTraceScope::Function (const CString & message)
  {
  if (!m_enabled || !m_pDoc->m_bTrace ||
      m_pDoc->m_bInTrace || m_pDoc->m_bTraceOutputRedraw)
    return;
  StartFunction ();
  CTraceOutput::Entry & entry = m_pDoc->m_traceOutput.entries [m_slot];
  if (!(m_pDoc->m_iTraceCategories & entry.category))
    return;
  entry.message = (LPCTSTR) message;
  if (!m_caller.empty ())
    entry.message += string (" [CallPlugin from \"") + m_caller + "\"]";
  }

unsigned int CMUSHclientDoc::GetTimerTraceCategory (const CTimer & timer) const
{
  return timer.bOneShot ? eTraceOther : eTraceRepeatingTimers;
}

unsigned int CMUSHclientDoc::GetScriptTraceCategory (LPCTSTR procedure, LPCTSTR type) const
{
  if (strcmp (type, "timer") == 0)
    return m_iTimerTraceCategory;

  if (strncmp (type, "Plugin ", 7) == 0)
    {
    if (ON_PLUGIN_TICK == procedure)
      return eTraceIdleTicks;
    if (ON_PLUGIN_DRAW_OUTPUT_WINDOW == procedure ||
        ON_PLUGIN_SCREENDRAW == procedure ||
        ON_PLUGIN_CHAT_DISPLAY == procedure ||
        ON_PLUGIN_WORLD_OUTPUT_RESIZED == procedure ||
        ON_PLUGIN_SELECTION_CHANGED == procedure)
      return eTraceDisplay;
    }
  return eTraceOther;
}

CTraceScriptGuard::CTraceScriptGuard (CMUSHclientDoc * pDoc,
                                    LPCTSTR procedure, LPCTSTR type) :
  m_pDoc (pDoc), m_pPlugin (pDoc->m_CurrentPlugin),
  m_bSavedRedraw (pDoc->m_bTraceOutputRedraw),
  m_iRedrawRequests (pDoc->m_iTraceRedrawRequests),
  m_iTraceOutputRequests (pDoc->m_iTraceOutputRedrawRequests),
  m_bDisplay (pDoc->GetScriptTraceCategory (procedure, type) == CMUSHclientDoc::eTraceDisplay)
  {
  if (!m_pPlugin)
    return;

  const bool traceOutput = pDoc->m_bInTrace || m_bSavedRedraw;
  const bool periodic = strcmp (type, "timer") == 0 ||
    (strncmp (type, "Plugin ", 7) == 0 && ON_PLUGIN_TICK == procedure);
  if (traceOutput)
    m_pPlugin->m_bTraceRedrawPending = true;
  else if (!periodic &&
           !(strncmp (type, "Plugin ", 7) == 0 && ON_PLUGIN_DRAW_OUTPUT_WINDOW == procedure))
    m_pPlugin->m_bTraceRedrawPending = false;

  if (periodic && m_pPlugin->m_bTraceRedrawPending)
    pDoc->m_bTraceOutputRedraw = true;
  }

void CTraceScriptGuard::BeginCallback ()
  {
  // The callback can queue more work after its trace line changes the display.
  if (m_bDisplay && m_pDoc->m_iTraceOutputRedrawRequests != m_iTraceOutputRequests)
    {
    m_pDoc->m_bTraceOutputRedraw = true;
    if (m_pPlugin)
      m_pPlugin->m_bTraceRedrawPending = true;
    }
  }

CTraceScriptGuard::~CTraceScriptGuard ()
  {
  // A repaint consumes deferred work; an idle tick does not.
  if (m_pPlugin && m_pDoc->m_iTraceRedrawRequests != m_iRedrawRequests)
    m_pPlugin->m_bTraceRedrawPending = false;
  m_pDoc->m_bTraceOutputRedraw = m_bSavedRedraw;
  }


void CMUSHclientDoc::TraceScript (LPCTSTR procedure, LPCTSTR type)
{
  const unsigned int category = GetScriptTraceCategory (procedure, type);
  // Display callbacks can request further updates while showing trace output.
  if (m_bTraceOutputRedraw)
    return;
  if (m_bTrace && !m_bInTrace && (m_iTraceCategories & category))
    {
    LPCTSTR traceType = m_CurrentPlugin && strncmp (type, "Plugin ", 7) == 0 ? "plugin" : type;
    TraceForCategory (category, TFormat ("Executing %s script \"%s\"", traceType, procedure));
    }
}

void CMUSHclientDoc::TraceOut(LPCTSTR Message) 
{
  Trace (Message);
}   // end of CMUSHclientDoc::TraceOut


BOOL CMUSHclientDoc::GetTrace() 
{
	return m_bTrace;
}  // end of CMUSHclientDoc::GetTrace

void CMUSHclientDoc::SetTrace(BOOL bNewValue) 
{

bNewValue = bNewValue != 0;   // make boolean

// if they are changing the value - go ahead and do it
if (bNewValue && !m_bTrace ||
    !bNewValue && m_bTrace)
  OnGameTrace ();

} // end of CMUSHclientDoc::SetTrace
