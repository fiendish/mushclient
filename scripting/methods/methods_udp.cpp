// methods_udp.cpp

// Related to the UDP send/receive stuff


#include "stdafx.h"
#include "..\..\MUSHclient.h"
#include "..\..\doc.h"
#include "..\..\UDPsocket.h"
#include "..\errors.h"

// Implements:

//    GetUdpPort
//    UdpListen
//    UdpPortList
//    UdpSend


// /UdpSend ("10.0.0.3", 4111, "m,textcolour,5555")

long CMUSHclientDoc::UdpSend(LPCTSTR IP, short Port, LPCTSTR Text) 
{
// timer t ("UdpSend");

int s;  // socket
struct sockaddr_in sa; 	

  // work out address to send to
  memset (&sa, 0, sizeof(sa));
  sa.sin_port = htons (Port);
  sa.sin_family = AF_INET;

  if ((sa.sin_addr.s_addr = inet_addr (IP)) == INADDR_NONE)
    return -1;     // address no good

  // make socket
  s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s == INVALID_SOCKET)
    return WSAGetLastError ();    // cannot create socket

  // allow broadcasts
  int x = 1;
  setsockopt( s, SOL_SOCKET, SO_BROADCAST, (const char *) &x, sizeof x);

  // send it
  long result = sendto (s, 
                        Text, strlen (Text), 
                        0,     // flags
                        (struct sockaddr *) &sa, sizeof sa);

  // close it regardless
  closesocket (s);

  if (result == SOCKET_ERROR)
	  return WSAGetLastError ();    // return error reason

  return 0;
}  // end of  CMUSHclientDoc::UdpSend


// /world.UdpListen "0.0.0.0", 4222, "OnUDP"

long CMUSHclientDoc::UdpListen(LPCTSTR IP, short Port, LPCTSTR Script) 
{

  // I think we'll insist plugins do this
  if (m_CurrentPlugin == NULL)
    return eNotAPlugin;

  // see if we already have one for this port
  map<int, UDPsocket *>::iterator it = m_UDPsocketMap.find (Port);
  UDPsocket * pOldSocket = it == m_UDPsocketMap.end () ? NULL : it->second;

  // don't re-use another plugin's port
  if (pOldSocket && pOldSocket->m_strPluginID != m_CurrentPlugin->m_strID)
    return eBadParameter;

  // no script deletes an existing listener
  if (strlen (Script) == 0)
    {
    if (pOldSocket)
      {
      m_UDPsocketMap.erase (it);
      if (pOldSocket->m_bInReceive)
        {
        pOldSocket->Close ();
        pOldSocket->m_bDeleteWhenDone = true;
        }
      else
        delete pOldSocket;
      }
    return eNoNameSpecified;
    }

  // Allocate and configure the replacement before closing the old listener.
  std::unique_ptr<UDPsocket> pSocket (new UDPsocket (this));
  pSocket->m_strScript = Script;
  pSocket->m_strPluginID =  m_CurrentPlugin->m_strID;
  pSocket->m_iPluginInstanceNumber =
    m_CurrentPlugin->m_iPluginInstanceNumber;

  // The old socket must be closed before the same port can be bound again.
  CString strOldIP;
  if (pOldSocket)
    {
    SOCKADDR_IN oldAddress;
    int iOldAddressLength = sizeof oldAddress;
    if (!pOldSocket->GetSockName ((SOCKADDR *) &oldAddress,
                                  &iOldAddressLength))
      return eBadParameter;
    strOldIP = inet_ntoa (oldAddress.sin_addr);
    pOldSocket->Close ();
    }

	if (!pSocket->Create (Port, SOCK_DGRAM, FD_READ, IP))
	  {
	  if (pOldSocket)
	    {
	    if (!pOldSocket->Create (Port, SOCK_DGRAM, FD_READ, strOldIP))
	      {
	      m_UDPsocketMap.erase (it);
	      if (pOldSocket->m_bInReceive)
	        pOldSocket->m_bDeleteWhenDone = true;
	      else
	        delete pOldSocket;
	      }
	    }
		return eBadParameter;
	  }     // end of can't create socket

  // keep a map of them

  if (pOldSocket)
    {
    it->second = pSocket.get ();
    pSocket.release ();
    if (pOldSocket->m_bInReceive)
      pOldSocket->m_bDeleteWhenDone = true;
    else
      delete pOldSocket;
    }
  else
    {
    try
      {
      m_UDPsocketMap [Port] = pSocket.get ();
      }
    catch (...)
      {
      pSocket->Close ();
      throw;
      }
    pSocket.release ();
    }
	return eOK;
}   // end of CMUSHclientDoc::UdpListen


VARIANT CMUSHclientDoc::UdpPortList() 
{
  COleSafeArray sa;   // for wildcard list

  long iCount = m_UDPsocketMap.size ();

  if (iCount) // cannot create empty array dimension
    {
    sa.CreateOneDim (VT_VARIANT, iCount);
  
    map<int, UDPsocket *>::const_iterator it;

    for (iCount = 0, it = m_UDPsocketMap.begin (); 
         it != m_UDPsocketMap.end (); it++, iCount++)
      {
      // the array must be a bloody array of variants, or VBscript kicks up
      COleVariant v ((long) it->first);
      sa.PutElement (&iCount, &v);
      }      // end of looping through each colour
    } // end of having at least one

	return sa.Detach ();
}     // end of CMUSHclientDoc::UdpPortList


long CMUSHclientDoc::GetUdpPort(long First, long Last) 
{
  CMUSHclientDoc * pDoc;
  POSITION pos;

  if (First > Last || 
      First < 1 ||
      Last > 65535)
    return 0;   // bad range

  set<long> UsedPortsSet;
  set<long>::const_iterator it;

  map<int, UDPsocket *>::const_iterator i;

  // get all worlds
  for (pos = App.m_pWorldDocTemplate->GetFirstDocPosition(); pos != NULL; )
    {
    pDoc = (CMUSHclientDoc *) App.m_pWorldDocTemplate->GetNextDoc(pos);

    // remember all UDP ports used by this world
    for (i = pDoc->m_UDPsocketMap.begin ();
         i != pDoc->m_UDPsocketMap.end ();
         i++)
      UsedPortsSet.insert (i->first);

    }      // end of looping through each world

  // now UsedPortsSet contains an entry for every world - find the first free one

  long result;

  for (result = First; result < Last; result++)
    {
    it = UsedPortsSet.find (result);
    if (it == UsedPortsSet.end ())
      return result;    // this one wasn't used
    }

  return 0;   // couldn't find one
}  // end of CMUSHclientDoc::GetUdpPort
