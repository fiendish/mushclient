// UDPsocket.cpp : implementation file
//

#include "stdafx.h"
#include "mushclient.h"
#include "doc.h"
#include "UDPsocket.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

/////////////////////////////////////////////////////////////////////////////
// UDPsocket

UDPsocket::UDPsocket(CMUSHclientDoc * pDoc)
{
m_pDoc = pDoc;
m_iPluginInstanceNumber = 0;
m_bInReceive = false;
m_bReceivePending = false;
m_bDeleteWhenDone = false;

}

UDPsocket::~UDPsocket()
{
}


// Do not edit the following lines, which are needed by ClassWizard.
#if 0
BEGIN_MESSAGE_MAP(UDPsocket, CAsyncSocket)
	//{{AFX_MSG_MAP(UDPsocket)
	//}}AFX_MSG_MAP
END_MESSAGE_MAP()
#endif	// 0

/////////////////////////////////////////////////////////////////////////////
// UDPsocket member functions

void UDPsocket::OnReceive(int nErrorCode)
  {
  if (m_bInReceive)
    {
    m_bReceivePending = true;
    return;
    }

  m_bInReceive = true;
  try
    {
    do
      {
      m_bReceivePending = false;
      ReceiveOneDatagram ();
      } while (m_bReceivePending && !m_bDeleteWhenDone);
    }
  catch (...)
    {
    bool bDeleteWhenDone = m_bDeleteWhenDone;
    m_bInReceive = false;
    m_bReceivePending = false;
    if (bDeleteWhenDone)
      delete this;
    throw;
    }

  bool bDeleteWhenDone = m_bDeleteWhenDone;
  m_bInReceive = false;
  m_bReceivePending = false;
  if (bDeleteWhenDone)
    delete this;
  }

void UDPsocket::ReceiveOneDatagram (void)
  {
  char buff [1000];
  int count = Receive (buff, sizeof (buff) - 1);

  if (count == SOCKET_ERROR)
    return;   // strange

  buff [count] = 0;   // null terminate it

  // call script
  if (m_pDoc->GetPluginInstance (m_strPluginID,
                                 m_iPluginInstanceNumber))
    m_pDoc->CallPlugin (m_strPluginID, m_strScript, buff);

  }
