// worldsock.cpp : implementation file
//

// But does it get goat's blood out?



#include "stdafx.h"
#include "MUSHclient.h"

#include "mainfrm.h"

#include "doc.h"

#include <stddef.h>
#include <stdio.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char BASED_CODE THIS_FILE[] = __FILE__;
#endif

IMPLEMENT_DYNAMIC(CWorldSocket, CAsyncSocket)

// Compare identities before accessing an object saved across a callback.
static bool IsLiveWorldSocket (const CMUSHclientDoc * pDoc,
                                const __int64 iDocumentNumber,
                                const CWorldSocket * pSocket,
                                const __int64 iSocketNumber)
  {
  for (POSITION pos = App.m_pWorldDocTemplate->GetFirstDocPosition (); pos; )
    {
    CMUSHclientDoc * pLiveDoc =
      (CMUSHclientDoc *) App.m_pWorldDocTemplate->GetNextDoc (pos);
    if (pLiveDoc == pDoc &&
        pLiveDoc->m_iUniqueDocumentNumber == iDocumentNumber)
      return pLiveDoc->m_pSocket == pSocket &&
             pSocket->m_iSocketNumber == iSocketNumber;
    }
  return false;
  }

CWorldSocket::CWorldSocket(CMUSHclientDoc* pDoc)
  : m_iSocketNumber (App.GetUniqueNumber ())
{
  m_pDoc = pDoc;
  m_bInReceive = false;
  m_bReceivePending = false;
  m_bBufferedReadPending = false;
  m_iBufferedReadDocumentNumber = 0;
  m_hBufferedReadSocket = INVALID_SOCKET;
  m_pBufferedReadSSL = NULL;
}

// A transport FD_READ cannot report plaintext already buffered by OpenSSL.
// Use a later timer entry, after the original receive exception has unwound.
void CWorldSocket::CheckBufferedReads (void)
  {
  for (POSITION pos = App.m_pWorldDocTemplate->GetFirstDocPosition (); pos; )
    {
    CMUSHclientDoc * pDoc =
      (CMUSHclientDoc *) App.m_pWorldDocTemplate->GetNextDoc (pos);
    CWorldSocket * pSocket = pDoc->m_pSocket;
    if (!pSocket || !pSocket->m_bBufferedReadPending || pSocket->m_bInReceive)
      continue;

    pSocket->m_bBufferedReadPending = false;
    if (pSocket->m_pDoc != pDoc ||
        pDoc->m_iUniqueDocumentNumber != pSocket->m_iBufferedReadDocumentNumber ||
        pSocket->m_hSocket == INVALID_SOCKET ||
        pSocket->m_hSocket != pSocket->m_hBufferedReadSocket ||
        !pDoc->m_bSSL_Connected || !pDoc->m_pSSL ||
        pDoc->m_pSSL != pSocket->m_pBufferedReadSSL ||
        SSL_pending (pDoc->m_pSSL) <= 0)
      continue;

    pSocket->OnReceive (0);
    // Receive callbacks can delete any world, including the next list entry.
    // Do not use the saved position, document or socket after this call.
    return;
    }
  }

void CWorldSocket::OnReceive(int nErrorCode)
{
  if (m_bInReceive)
    {
    m_bReceivePending = true;
    return;
    }

  m_bInReceive = true;
  m_bBufferedReadPending = false;

  CMUSHclientDoc * pDoc = m_pDoc;
  const __int64 iDocumentNumber = pDoc->m_iUniqueDocumentNumber;
  const __int64 iSocketNumber = m_iSocketNumber;
  const SOCKET hSocket = m_hSocket;
  struct ssl_st * pSSL = pDoc->m_pSSL;

  try
    {
    do
      {
      m_bReceivePending = false;
      pDoc->ReceiveMsg();

      if (!IsLiveWorldSocket (pDoc, iDocumentNumber, this, iSocketNumber))
        return;
      if (m_hSocket != hSocket || (pSSL && pDoc->m_pSSL != pSSL))
        {
        m_bInReceive = false;
        m_bReceivePending = false;
        return;
        }

      // A proxy response can start TLS on this socket during ReceiveMsg.
      if (!pSSL)
        pSSL = pDoc->m_pSSL;

      // SSL may have buffered more decrypted data than one SSL_read consumed.
      // Since we will not get FD_READ for buffered data, drain it now.
      if (pDoc->m_pSSL && pDoc->m_bSSL_Connected)
        {
        while (SSL_pending (pDoc->m_pSSL) > 0)
          {
          pDoc->ReceiveMsg();
          if (!IsLiveWorldSocket (pDoc, iDocumentNumber, this, iSocketNumber))
            return;
          if (m_hSocket != hSocket || (pSSL && pDoc->m_pSSL != pSSL))
            {
            m_bInReceive = false;
            m_bReceivePending = false;
            return;
            }
          if (!pDoc->m_pSSL || !pDoc->m_bSSL_Connected)
            break;
          }
        }
      } while (m_bReceivePending);
    }
  catch (...)
    {
    if (IsLiveWorldSocket (pDoc, iDocumentNumber, this, iSocketNumber))
      {
      m_bInReceive = false;
      if (m_bReceivePending && hSocket != INVALID_SOCKET &&
          m_hSocket == hSocket)
        {
        // Keep plain FD_READ recovery. This consumes no bytes and changes no
        // event mask. A later notification can read remaining transport data.
        char c;
        const int nResult = CAsyncSocket::Receive (&c, 1, MSG_PEEK);
        const int nError = nResult == SOCKET_ERROR ? GetLastError () : 0;
        m_bReceivePending = false;
        if (nError != 0 && nError != WSAEWOULDBLOCK)
          {
          char szMessage [160];
          snprintf (szMessage, sizeof szMessage, "Unable to rearm socket read notification (Winsock error %d).",
                    nError);
          ::MessageBoxA (NULL, szMessage, "MUSHclient", MB_OK | MB_ICONERROR | MB_TASKMODAL);
          }
        }
      else
        m_bReceivePending = false;
      }

    // Error reporting can run callbacks, destroy worlds or reconnect. Check
    // again and publish the retry only immediately before rethrowing.
    if (IsLiveWorldSocket (pDoc, iDocumentNumber, this, iSocketNumber) &&
        m_hSocket == hSocket && hSocket != INVALID_SOCKET &&
        pSSL && pDoc->m_pSSL == pSSL && pDoc->m_bSSL_Connected &&
        SSL_pending (pSSL) > 0)
      {
      m_iBufferedReadDocumentNumber = iDocumentNumber;
      m_hBufferedReadSocket = hSocket;
      m_pBufferedReadSSL = pSSL;
      m_bBufferedReadPending = true;
      }
    throw;
    }

  m_bInReceive = false;
  m_bReceivePending = false;
  CAsyncSocket::OnReceive(nErrorCode);
}

void CWorldSocket::OnSend(int nErrorCode)
{

  // save m_pDoc locally — if the TLS handshake fails, 'this' (the socket) gets
  // deleted inside ContinueSSLHandshake, so we must not touch 'this' afterwards
  CMUSHclientDoc * pDoc = m_pDoc;

int count;

   if (nErrorCode)    // had an error, give up!
      return;

  // if we are in the middle of a TLS handshake, continue it
  if (pDoc->m_iConnectPhase == eConnectAwaitingSSLHandshake)
    {
    pDoc->ContinueSSLHandshake ();
    return;  // always return - if handshake failed, socket may be destroyed
    }

// if we have outstanding data to send, do it
  if (m_outstanding_data.empty ())
    return;

  // SSL-aware send
  if (pDoc->m_pSSL && pDoc->m_bSSL_Connected)
    {
    count = SSL_write (pDoc->m_pSSL, m_outstanding_data.data (),
                       m_outstanding_data.length ());
    if (count > 0)
      {
      pDoc->m_nBytesOut += count;
      m_outstanding_data.erase (0, count);
      }
    else
      {
      int ssl_err = SSL_get_error (pDoc->m_pSSL, count);
      if (ssl_err != SSL_ERROR_WANT_WRITE && ssl_err != SSL_ERROR_WANT_READ)
        {
        ShutDownSocket (*this);
        m_outstanding_data.erase ();
        }
      }
    }
  else
    {
    count = Send (m_outstanding_data.data (), m_outstanding_data.length ());

    if (count != SOCKET_ERROR)
      pDoc->m_nBytesOut += count; // count bytes out

    if (count > 0)    // good send - do rest later
      m_outstanding_data.erase (0, count);
    else
      {
      int nError = GetLastError ();
      if (count == SOCKET_ERROR && nError != WSAEWOULDBLOCK)
        {
        ShutDownSocket (*this);
  //       m_pSocket->OnClose (nError);      // ????
        m_outstanding_data.erase ();

        }   // end of an error other than "would block"
      } // end of an error
    }

}

void CWorldSocket::OnClose(int nErrorCode)
  {

bool bWasClosed = m_pDoc->m_iConnectPhase == eConnectNotConnected;

  TRACE1 ("CWorldSocket::OnClose, error code %i\n", nErrorCode);

  m_pDoc->SSLCleanup ();

  m_pDoc->m_iConnectPhase = eConnectDisconnecting;

  m_pDoc->UpdateAllViews (NULL);

  m_pDoc->MXP_Off (true);   // turn off MXP now

  // update button bar - make button red
  if (m_pDoc->m_view_number >= 1 &&
      m_pDoc->m_view_number <= 10)
    Frame.OnUpdateBtnWorlds (m_pDoc->m_view_number, NULL);

  // execute "disconnect" script
  if (m_pDoc->m_ScriptEngine)
    {
    if (m_pDoc->SeeIfHandlerCanExecute (m_pDoc->m_strWorldDisconnect))
      {
      DISPPARAMS params = { NULL, NULL, 0, 0 };
      long nInvocationCount = 0;

      m_pDoc->ExecuteScript (m_pDoc->m_dispidWorldDisconnect,  
                             m_pDoc->m_strWorldDisconnect,
                             eWorldAction,
                             "world disconnect", 
                             "disconnecting from world",
                             params, 
                             nInvocationCount); 
      }
    } // end of executing disconnect script

  m_pDoc->SendToAllPluginCallbacks (ON_PLUGIN_DISCONNECT);

  // close log file if we auto-opened it
  if (!m_pDoc->m_strAutoLogFileName.IsEmpty ())
    m_pDoc->CloseLog ();

  if (m_pDoc->m_bShowConnectDisconnect && !bWasClosed)
    {
    CTime theTime;
    theTime = CTime::GetCurrentTime();

    CString strConnected;
    strConnected = theTime.Format (TranslateTime ("--- Disconnected on %A, %B %d, %Y, %#I:%M %p ---"));

    m_pDoc->Note (strConnected);  

    // find time spent connected
    CTimeSpan ts = CTime::GetCurrentTime() - m_pDoc->m_tConnectTime;
      
    CString strDuration = TFormat ("--- Connected for %i day%s, %i hour%s, %i minute%s, %i second%s. ---",
                  PLURAL ((long) ts.GetDays()),
                  PLURAL ((long) ts.GetHours()),
                  PLURAL ((long) ts.GetMinutes()),
                  PLURAL ((long) ts.GetSeconds()));

    m_pDoc->Note (strDuration);  

    // and a horizontal rule

    m_pDoc->m_pCurrentLine->flags = HORIZ_RULE;  
    m_pDoc->StartNewLine (true, 0); 
    }  // end of message in world window wanted

  CString strInfo = TFormat ("--- Received %i line%s, sent %i line%s.",
                PLURAL (m_pDoc->m_nTotalLinesReceived),
                PLURAL (m_pDoc->m_nTotalLinesSent)
                );
  
  m_pDoc->Note (strInfo);  

  strInfo = TFormat ("--- Output buffer has %i/%i line%s in it (%.1f%% full).",
                m_pDoc->m_LineList.GetCount (),
                m_pDoc->m_maxlines,
                (m_pDoc->m_LineList.GetCount ()) == 1 ? "" : "s",
                (double) m_pDoc->m_LineList.GetCount () / (double) m_pDoc->m_maxlines * 100.0
                );

  m_pDoc->Note (strInfo);  

  strInfo = TFormat ("--- Matched %i trigger%s, %i alias%s, and %i timer%s fired.",
                PLURAL (m_pDoc->m_iTriggersMatchedThisSessionCount),   
                PLURALES (m_pDoc->m_iAliasesMatchedThisSessionCount),    
                PLURAL (m_pDoc->m_iTimersFiredThisSessionCount)       
                );

  m_pDoc->Note (strInfo);  

CString str;

  str = TFormat ("The \"%s\" server has closed the connection", 
              (const char *) m_pDoc->m_mush_name);

  if (App.m_bNotifyOnDisconnect && !m_pDoc->m_bDisconnectOK)
    {
    if (App.m_bErrorNotificationToOutputWindow)
      m_pDoc->Note (str);
    else
      ::UMessageBox (str, MB_ICONEXCLAMATION);
    }
  else
    Frame.SetStatusMessage (str);
  m_pDoc->m_iConnectPhase = eConnectNotConnected;

  m_pDoc->UpdateAllViews  (NULL);     // force window title to be redrawn

  } // end of OnClose

void CWorldSocket::OnConnect(int nErrorCode)
  {

  m_pDoc->OnConnect (nErrorCode);
  } // end of OnConnect
