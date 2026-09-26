// chatlistensock.cpp : implementation file
//

// But does it get goat's blood out?



#include "stdafx.h"
#include "MUSHclient.h"
#include "doc.h"

#include <stddef.h>

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char BASED_CODE THIS_FILE[] = __FILE__;
#endif

IMPLEMENT_DYNAMIC(CChatListenSocket, CAsyncSocket)

CChatListenSocket::CChatListenSocket(CMUSHclientDoc* pDoc)
{
	m_pDoc = pDoc;

}   // end of constructor

CChatListenSocket::~CChatListenSocket()
{

// cancel callbacks for closed sockets

  Close ();

}     // end of destructor

void CChatListenSocket::OnAccept(int nErrorCode)
  {
  // A chat-display callback may stop listening and delete this socket. Finish
  // using it before the first callback, and retain only the guarded document.
  CMUSHclientDoc * pDoc = m_pDoc;
  CWorldDocumentOperationGuard operationGuard (pDoc);

std::unique_ptr<CChatSocket> pSocket (new CChatSocket (pDoc));
int SockAddrLen = sizeof(pSocket->m_ServerAddr) ;

  const BOOL bAccepted = Accept (*pSocket,
                                (SOCKADDR*) &pSocket->m_ServerAddr,
                                &SockAddrLen);

  pDoc->ChatNote (eChatConnection, "Incoming chat call");

  if (!bAccepted)
    {
    pDoc->ChatNote (eChatConnection, "Cannot accept call.");
    return;
    }

  pSocket->AsyncSelect ();
  pSocket->m_bIncoming = true;

  pSocket->m_strServerName = inet_ntoa (pSocket->m_ServerAddr.sin_addr);


  pDoc->ChatNote (eChatConnection,
                  TFormat (
                    "Accepted call from %s port %d",
                            (LPCTSTR) pSocket->m_strServerName,
                            ntohs (pSocket->m_ServerAddr.sin_port)));

  pSocket->m_iChatStatus = eChatAwaitingConnectionRequest;

  pDoc->m_ChatList.AddTail (pSocket.get ());
  pSocket.release ();
  } // end of OnAccept


