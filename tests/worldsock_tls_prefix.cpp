#include <openssl/ssl.h>
#include <openssl/err.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <vector>
using std::string;
using __int64 = long long;
using UINT = unsigned;
using POSITION = unsigned;
using SOCKET = int;
inline constexpr int INVALID_SOCKET = -1, SOCKET_ERROR = -1, WSAEWOULDBLOCK = 10035;
inline constexpr int MSG_PEEK = 2, MB_OK = 0, MB_ICONERROR = 16, MB_TASKMODAL = 8192;
static bool staleReadQueued = false;
static bool recordReadsEnabled = false;
static int lastError = 0;
static BIO *encryptedInput;
static std::vector<int> transportReads;
int MessageBoxA(void *, const char *, const char *, int) { assert(false); return 0; }
static int traceRead(BIO *bio, char *data, int size) {
  BIO_clear_retry_flags(bio);
  int got = BIO_read(BIO_next(bio), data, size);
  BIO_copy_next_retry(bio);
  if(recordReadsEnabled && got > 0) {
    transportReads.push_back(got);
    // Model a possible Winsock event posted after recv reenables FD_READ
    // while payload remains. A later recv does not remove that queued event.
    if(BIO_ctrl_pending(BIO_next(bio)) > 0) staleReadQueued = true;
  }
  return got;
}
static int traceWrite(BIO *bio, const char *data, int size) {
  BIO_clear_retry_flags(bio);
  int got = BIO_write(BIO_next(bio), data, size);
  BIO_copy_next_retry(bio);
  return got;
}
static long traceCtrl(BIO *bio, int cmd, long value, void *ptr) {
  return BIO_ctrl(BIO_next(bio), cmd, value, ptr);
}
static unsigned int clientPsk(SSL *, const char *, char *identity, unsigned int maxId,
                             unsigned char *psk, unsigned int maxPsk) {
  assert(maxId > 8 && maxPsk >= 16);
  std::strcpy(identity, "fixture"); std::memset(psk, 0x42, 16); return 16;
}
static unsigned int serverPsk(SSL *, const char *identity, unsigned char *psk, unsigned int maxPsk) {
  assert(std::strcmp(identity, "fixture") == 0 && maxPsk >= 16);
  std::memset(psk, 0x42, 16); return 16;
}
static void moveBytes(BIO *from, BIO *to) {
  char data[32768];
  while(BIO_ctrl_pending(from)) {
    int n = BIO_read(from, data, sizeof(data)); assert(n > 0);
    assert(BIO_write(to, data, n) == n);
  }
}
struct CAsyncSocket {
  virtual ~CAsyncSocket() = default;
  SOCKET m_hSocket = 42;
  int peekCalls = 0;
  bool nextReadQueued = false;
  int Receive(void *, int count, int flags) {
    assert(count == 1 && flags == MSG_PEEK);
    ++peekCalls;
    nextReadQueued = BIO_ctrl_pending(encryptedInput) > 0;
    if(nextReadQueued) return 1;
    lastError = WSAEWOULDBLOCK; return SOCKET_ERROR;
  }
  static int GetLastError() { return lastError; }
  void OnReceive(int) {}
};
#define DECLARE_DYNAMIC(type)
#include "worldsock.h"
class CMUSHclientDoc {
public:
  __int64 m_iUniqueDocumentNumber = 101;
  CWorldSocket *m_pSocket = nullptr;
  SSL *m_pSSL = nullptr;
  bool m_bSSL_Connected = true;
  std::function<void()> receive;
  void ReceiveMsg() { receive(); }
};
struct DocTemplate {
  std::vector<CMUSHclientDoc *> docs;
  POSITION GetFirstDocPosition() { return docs.empty() ? 0 : 1; }
  CMUSHclientDoc *GetNextDoc(POSITION &p) {
    auto result = docs.at(p - 1); p = p < docs.size() ? p + 1 : 0; return result;
  }
} docTemplate;
struct TestApp {
  DocTemplate *m_pWorldDocTemplate = &docTemplate;
  __int64 nextNumber = 1;
  __int64 GetUniqueNumber() { return nextNumber++; }
};
inline TestApp App;

struct CMDIFrameWnd { int baseTimers = 0; void OnTimer(UINT) { ++baseTimers; } };
struct CMainFrame : CMDIFrameWnd {
  int fallbacks = 0;
  void CheckTimerFallback() { ++fallbacks; }
  void OnTimer(UINT);
};
#include "worldsock_receive.inc"
#include "worldsock_timer.inc"
void CWorldSocket::OnSend(int) {}
void CWorldSocket::OnClose(int) {}
void CWorldSocket::OnConnect(int) {}
