// Model only the facilities used by the actual constructor and OnReceive.
// Winsock rearms FD_READ after recv, including MSG_PEEK and failed calls:
// https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsaasyncselect
#include <cassert>
#include <cstdio>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using std::string;
using __int64 = long long;
using UINT = unsigned;
using POSITION = unsigned;
using SOCKET = int;
const SOCKET INVALID_SOCKET = -1;
const int SOCKET_ERROR = -1, WSAEWOULDBLOCK = 10035, WSAECONNRESET = 10054;
const int MSG_PEEK = 2, MB_OK = 0, MB_ICONERROR = 16, MB_TASKMODAL = 8192;
const int FD_READ = 1, FD_WRITE = 2, FD_OOB = 4;
const int FD_ACCEPT = 8, FD_CONNECT = 16, FD_CLOSE = 32;
const int allEvents = FD_READ | FD_WRITE | FD_OOB | FD_ACCEPT | FD_CONNECT | FD_CLOSE;
int lastError = 0;
string report;
std::function<void()> onReport;
int MessageBoxA(void *, const char *text, const char *, int flags) {
  assert(flags == (MB_OK | MB_ICONERROR | MB_TASKMODAL));
  report = text;
  if (onReport) onReport();
  return 1;
}

struct CAsyncSocket {
  virtual ~CAsyncSocket() = default;
  SOCKET m_hSocket = 42;
  int eventMask = allEvents, peekCalls = 0, error = 0;
  int closeEvents = 0, baseCalls = 0, baseError = 0;
  bool enabled = false, queued = false, eof = false;
  string unread;
  void notify() {
    if (enabled && !unread.empty() && (eventMask & FD_READ)) {
      queued = true;
      enabled = false;
    }
  }
  void arrive(const string &data) { unread += data; notify(); }
  string read() {
    string data = unread;
    unread.clear();
    enabled = true;
    if (eof && (eventMask & FD_CLOSE)) ++closeEvents;
    return data;
  }
  int Receive(void *buffer, int length, int flags) {
    assert(m_hSocket != INVALID_SOCKET && length == 1 && flags == MSG_PEEK);
    ++peekCalls;
    enabled = true;
    notify();
    if (error) { lastError = error; return SOCKET_ERROR; }
    if (!unread.empty()) { *static_cast<char *>(buffer) = unread[0]; return 1; }
    if (eof) return 0;
    lastError = WSAEWOULDBLOCK;
    return SOCKET_ERROR;
  }
  static int GetLastError() { return lastError; }
  void OnReceive(int errorCode) { ++baseCalls; baseError = errorCode; }
};
#define DECLARE_DYNAMIC(type)
#include "worldsock.h"

struct ssl_st { int pending = 0; };
using SSL = ssl_st;
int SSL_pending(SSL *ssl) { return ssl->pending; }
class CMUSHclientDoc {
public:
  __int64 m_iUniqueDocumentNumber = 101;
  CWorldSocket *m_pSocket = nullptr;
  SSL *m_pSSL = nullptr;
  bool m_bSSL_Connected = false;
  std::function<void()> receive;
  void ReceiveMsg() { auto callback = receive; callback(); }
};
struct DocTemplate {
  std::vector<CMUSHclientDoc *> docs;
  POSITION GetFirstDocPosition() { return docs.empty() ? 0 : 1; }
  CMUSHclientDoc *GetNextDoc(POSITION &pos) {
    auto *doc = docs.at(pos - 1);
    pos = pos < docs.size() ? pos + 1 : 0;
    return doc;
  }
} docTemplate;
struct {
  DocTemplate *m_pWorldDocTemplate = &docTemplate;
  __int64 nextNumber = 1;
  __int64 GetUniqueNumber() { return nextNumber++; }
} App;
#include "worldsock_receive.inc"
struct CMDIFrameWnd { int baseTimers = 0; void OnTimer(UINT) { ++baseTimers; } };
struct CMainFrame : CMDIFrameWnd {
  int fallbacks = 0;
  std::function<void()> fallback;
  void CheckTimerFallback() { ++fallbacks; if (fallback) fallback(); }
  void OnTimer(UINT);
};
#include "worldsock_timer.inc"
// Supply the other vtable entries; these callbacks are not under test.
void CWorldSocket::OnSend(int) {}
void CWorldSocket::OnClose(int) {}
void CWorldSocket::OnConnect(int) {}

struct ReceiveFailure {};
struct Fixture {
  CMUSHclientDoc doc;
  CWorldSocket socket{&doc};
  ReceiveFailure failure;
  Fixture() {
    doc.m_pSocket = &socket;
    docTemplate.docs = {&doc};
    report.clear();
    onReport = nullptr;
  }
  void fail() {
    bool caught = false;
    try { socket.OnReceive(0); }
    catch (ReceiveFailure *value) { assert(value == &failure); caught = true; }
    assert(caught); // Preserve the exact pointer, as for an MFC exception.
  }
  void dispatch() {
    assert(socket.queued);
    socket.queued = false;
    socket.OnReceive(0);
  }
};

static void bufferedReadCases();

int main() {
  {
    Fixture f;
    int calls = 0, depth = 0;
    f.doc.receive = [&] {
      assert(++depth == 1);
      if (++calls == 1) for (int i = 0; i < 3; ++i) f.socket.OnReceive(0);
      --depth;
    };
    f.socket.OnReceive(7);
    assert(calls == 2 && f.socket.peekCalls == 0);
    assert(f.socket.baseCalls == 1 && f.socket.baseError == 7);
    assert(!f.socket.m_bInReceive && !f.socket.m_bReceivePending);
    SSL ssl{3};
    f.doc.m_pSSL = &ssl; f.doc.m_bSSL_Connected = true;
    calls = 0;
    f.doc.receive = [&] { ++calls; --ssl.pending; };
    f.socket.OnReceive(0);
    assert(calls == 3 && ssl.pending == 0 && f.socket.peekCalls == 0);
  }
  std::cout << "PASS: nested callbacks stay serial; TLS data drains\n";

  for (int mask : {allEvents, FD_READ | FD_WRITE | FD_CONNECT | FD_CLOSE}) {
    Fixture f;
    f.socket.eventMask = mask;
    f.socket.unread = "first";
    string output;
    int calls = 0;
    f.doc.receive = [&] {
      ++calls;
      output += f.socket.read();
      if (calls == 1) {
        f.socket.arrive("second");
        f.dispatch(); // A modal loop consumes FD_READ without a recv.
        throw &f.failure;
      }
    };
    f.fail();
    assert(calls == 1 && output == "first"); // No synchronous retry.
    assert(f.socket.unread == "second" && f.socket.queued);
    assert(f.socket.peekCalls == 1 && f.socket.eventMask == mask);
    assert(!f.socket.m_bInReceive && !f.socket.m_bReceivePending && report.empty());
    f.dispatch();
    assert(calls == 2 && output == "firstsecond");
    f.socket.arrive("third");
    f.dispatch();
    assert(output == "firstsecondthird" && f.socket.enabled);
    assert(f.socket.closeEvents == 0 && f.socket.baseCalls == 2);
  }
  std::cout << "PASS: exception preserves queued bytes and later packets for both masks\n";

  {
    Fixture f;
    SSL ssl{0};
    f.doc.m_pSSL = &ssl; f.doc.m_bSSL_Connected = true;
    f.socket.unread = "first";
    int calls = 0;
    string output;
    f.doc.receive = [&] {
      ++calls;
      if (ssl.pending) { --ssl.pending; output += "decrypted"; return; }
      output += f.socket.read();
      if (calls == 1) {
        f.socket.arrive("second");
        f.dispatch();
        throw &f.failure;
      }
      ssl.pending = 2;
    };
    f.fail();
    assert(calls == 1 && f.socket.queued);
    f.dispatch();
    assert(calls == 4 && ssl.pending == 0);
    assert(output == "firstseconddecrypteddecrypted");
  }
  std::cout << "PASS: deferred TLS callback resumes normal decrypted-data draining\n";

  for (int mode = 0; mode < 5; ++mode) {
    Fixture f;
    // No deferred event; stale event; EOF; socket error; disabled events.
    if (mode == 2) { f.socket.eof = true; f.socket.closeEvents = 1; }
    if (mode == 3) f.socket.error = WSAECONNRESET;
    if (mode == 4) { f.socket.eventMask = 0; f.socket.unread = "retained"; }
    f.doc.receive = [&] {
      if (mode) f.socket.OnReceive(0);
      throw &f.failure;
    };
    f.fail();
    assert(f.socket.peekCalls == (mode ? 1 : 0) && !f.socket.queued);
    assert(f.socket.closeEvents == (mode == 2 ? 1 : 0));
    assert(!f.socket.m_bInReceive && !f.socket.m_bReceivePending);
    if (mode == 3) assert(report.find("10054") != string::npos);
    else assert(report.empty());
    if (mode == 1) {
      f.socket.arrive("later");
      f.doc.receive = [&] { assert(f.socket.read() == "later"); };
      f.dispatch();
    }
    if (mode == 4) assert(f.socket.eventMask == 0 && f.socket.unread == "retained");
  }
  std::cout << "PASS: no pending event, would-block, EOF, visible error, disabled mask\n";

  for (int mode = 0; mode < 4; ++mode) {
    Fixture f;
    CWorldSocket replacement{&f.doc};
    f.doc.receive = [&] {
      f.socket.OnReceive(0);
      if (mode == 0) f.doc.m_pSocket = &replacement;
      if (mode == 1) docTemplate.docs.clear();
      if (mode == 2) f.socket.m_hSocket = INVALID_SOCKET;
      if (mode == 3) f.socket.m_hSocket = 99;
      throw &f.failure;
    };
    f.fail();
    assert(f.socket.peekCalls == 0 && replacement.peekCalls == 0);
    assert(!replacement.m_bInReceive && !replacement.m_bReceivePending);
  }
  std::cout << "PASS: replaced socket, removed document, closed or changed handle\n";

  for (bool deleteDuringReport : {false, true}) {
    Fixture f;
    auto socket = std::make_unique<CWorldSocket>(&f.doc);
    f.doc.m_pSocket = socket.get();
    socket->error = WSAECONNRESET;
    auto removeSocket = [&] { f.doc.m_pSocket = nullptr; socket.reset(); };
    if (deleteDuringReport) onReport = removeSocket;
    f.doc.receive = [&] {
      socket->OnReceive(0);
      if (!deleteDuringReport) removeSocket();
      throw &f.failure;
    };
    bool caught = false;
    try { socket->OnReceive(0); }
    catch (ReceiveFailure *value) { assert(value == &f.failure); caught = true; }
    assert(caught && !socket);
    onReport = nullptr;
  }
  std::cout << "PASS: socket deletion before catch and during error report\n";
  bufferedReadCases();
}

static void bufferedReadCases() {
  for (bool nested : {false, true}) {
    Fixture f; CMainFrame frame; SSL ssl{7};
    f.doc.m_pSSL = &ssl; f.doc.m_bSSL_Connected = true;
    int calls = 0, depth = 0;
    f.doc.receive = [&] {
      assert(++depth == 1); ++calls;
      if (calls == 1) {
        if (nested) f.socket.OnReceive(0);
        --depth; throw &f.failure;
      }
      frame.OnTimer(1); // A modal turn inside the retry must not recurse.
      assert(f.socket.m_bInReceive);
      ssl.pending = 0; --depth;
    };
    f.fail();
    assert(calls == 1 && f.socket.m_bBufferedReadPending);
    assert(!f.socket.queued && f.socket.peekCalls == int(nested));
    f.socket.m_bInReceive = true;
    frame.OnTimer(1);
    assert(calls == 1 && f.socket.m_bBufferedReadPending);
    f.socket.m_bInReceive = false;
    frame.OnTimer(1);
    assert(calls == 2 && ssl.pending == 0 && !f.socket.m_bBufferedReadPending);
    assert(frame.fallbacks == 3 && frame.baseTimers == 3);
    frame.OnTimer(1); assert(calls == 2);
  }
  std::cout << "PASS: buffered TLS retries after unwind, including no nested event; timer receive stays serial\n";

  {
    Fixture f; CMainFrame frame; SSL ssl{2}; int calls = 0;
    f.doc.m_pSSL = &ssl; f.doc.m_bSSL_Connected = true;
    f.doc.receive = [&] { ++calls; throw &f.failure; };
    f.fail();
    bool caught = false;
    try { frame.OnTimer(1); } catch (ReceiveFailure *e) { assert(e == &f.failure); caught = true; }
    assert(caught && calls == 2 && f.socket.m_bBufferedReadPending);
    assert(frame.fallbacks == 1 && frame.baseTimers == 1);
    f.doc.receive = [&] { ++calls; ssl.pending = 0; };
    // A transport event can consume the retry first. The timer must not repeat it.
    f.socket.OnReceive(0); frame.OnTimer(1);
    assert(calls == 3 && !f.socket.m_bBufferedReadPending);
  }
  std::cout << "PASS: retry exceptions propagate unchanged and later FD_READ consumes the pending retry\n";


  {
    CMUSHclientDoc first, next;
    next.m_iUniqueDocumentNumber = 202;
    CWorldSocket a(&first), b(&next);
    CMainFrame frame;
    SSL sa{1}, sb{1};
    ReceiveFailure failure;
    first.m_pSocket = &a; next.m_pSocket = &b;
    first.m_pSSL = &sa; next.m_pSSL = &sb;
    first.m_bSSL_Connected = next.m_bSSL_Connected = true;
    docTemplate.docs = {&first, &next};
    first.receive = next.receive = [&] { throw &failure; };
    for (auto *socket : {&a, &b}) {
      bool caught = false;
      try { socket->OnReceive(0); }
      catch (ReceiveFailure *e) { assert(e == &failure); caught = true; }
      assert(caught);
    }
    int firstCalls = 0, nextCalls = 0;
    first.receive = [&] { ++firstCalls; sa.pending = 0; };
    next.receive = [&] { ++nextCalls; sb.pending = 0; };
    frame.OnTimer(1);
    assert(firstCalls == 1 && nextCalls == 0 && b.m_bBufferedReadPending);
    frame.OnTimer(1);
    assert(firstCalls == 1 && nextCalls == 1 && !b.m_bBufferedReadPending);
  }
  std::cout << "PASS: separate timer entries resume separate pending worlds\n";

  for (bool replace : {false, true}) {
    Fixture f;
    SSL ssl{2}, replacement{2};
    int calls = 0;
    if (replace) { f.doc.m_pSSL = &ssl; f.doc.m_bSSL_Connected = true; }
    f.doc.receive = [&] {
      ++calls;
      if (calls == 1) {
        // A proxy response can create TLS; an established session can be replaced.
        f.doc.m_pSSL = replace ? &replacement : &ssl;
        f.doc.m_bSSL_Connected = true;
      } else {
        --ssl.pending;
      }
    };
    f.socket.OnReceive(0);
    assert(calls == (replace ? 1 : 3));
    assert(replacement.pending == 2 && !f.socket.m_bInReceive);
  }
  std::cout << "PASS: proxy TLS creation drains normally; replaced sessions stop the old receive\n";

  for (int mode = 0; mode < 9; ++mode) {
    Fixture f; CMainFrame frame; SSL ssl{2}, replacementSSL{2};
    CWorldSocket replacement{&f.doc};
    f.doc.m_pSSL = &ssl; f.doc.m_bSSL_Connected = true;
    int calls = 0;
    f.doc.receive = [&] { ++calls; throw &f.failure; };
    f.fail(); assert(f.socket.m_bBufferedReadPending);
    if (mode == 0) f.doc.m_pSSL = &replacementSSL;
    if (mode == 1) f.doc.m_bSSL_Connected = false;
    if (mode == 2) f.doc.m_pSSL = nullptr;
    if (mode == 3) f.socket.m_hSocket = 99;
    if (mode == 4) f.socket.m_hSocket = INVALID_SOCKET;
    if (mode == 5) ++f.doc.m_iUniqueDocumentNumber;
    if (mode == 6) f.doc.m_pSocket = &replacement;
    if (mode == 7) docTemplate.docs.clear();
    if (mode == 8) ssl.pending = 0;
    frame.OnTimer(1);
    assert(calls == 1 && !replacement.m_bBufferedReadPending);
    if (mode != 6 && mode != 7) assert(!f.socket.m_bBufferedReadPending);
  }
  std::cout << "PASS: timer rejects changed TLS session, state, handle, document, socket and drained input\n";

  for (int mode = 0; mode < 5; ++mode) {
    Fixture f; CMainFrame frame; SSL ssl{2}, replacementSSL{2};
    auto socket = std::make_unique<CWorldSocket>(&f.doc);
    f.doc.m_pSocket = socket.get(); f.doc.m_pSSL = &ssl; f.doc.m_bSSL_Connected = true;
    socket->error = WSAECONNRESET;
    int calls = 0;
    f.doc.receive = [&] { ++calls; socket->OnReceive(0); throw &f.failure; };
    onReport = [&] {
      assert(!socket->m_bBufferedReadPending);
      frame.OnTimer(1); assert(calls == 1); // Original exception is still in catch.
      if (mode == 1) f.doc.m_pSSL = &replacementSSL;
      if (mode == 2) { f.doc.m_pSocket = nullptr; socket.reset(); }
      if (mode == 3) ++f.doc.m_iUniqueDocumentNumber;
      if (mode == 4) socket->m_hSocket = 99;
    };
    bool caught = false;
    try { socket->OnReceive(0); } catch (ReceiveFailure *e) { assert(e == &f.failure); caught = true; }
    assert(caught && calls == 1 && report.find("10054") != string::npos);
    onReport = nullptr;
    if (socket) assert(socket->m_bBufferedReadPending == (mode == 0));
    f.doc.receive = [&] { ++calls; ssl.pending = 0; };
    frame.OnTimer(1); assert(calls == (mode == 0 ? 2 : 1));
    f.doc.m_pSocket = nullptr;
  }
  std::cout << "PASS: error dialog cannot run the retry before unwind; catch rechecks all live identities\n";

  for (bool fail : {false, true}) {
    CMainFrame frame; auto doc = std::make_unique<CMUSHclientDoc>();
    auto socket = std::make_unique<CWorldSocket>(doc.get());
    SSL ssl{2}; ReceiveFailure failure;
    doc->m_pSSL = &ssl; doc->m_bSSL_Connected = true; doc->m_pSocket = socket.get();
    docTemplate.docs = {doc.get()};
    doc->receive = [&] { docTemplate.docs.clear(); socket.reset(); doc.reset(); if (fail) throw &failure; };
    bool caught = false;
    try { socket->OnReceive(0); } catch (ReceiveFailure *e) { assert(e == &failure); caught = true; }
    assert(caught == fail && !doc && !socket);
    frame.OnTimer(1);
  }
  std::cout << "PASS: successful and throwing callbacks can delete the receiving document and socket\n";

  for (bool fail : {false, true}) {
    CMainFrame frame; auto first = std::make_unique<CMUSHclientDoc>();
    auto next = std::make_unique<CMUSHclientDoc>();
    next->m_iUniqueDocumentNumber = 202;
    auto a = std::make_unique<CWorldSocket>(first.get());
    auto b = std::make_unique<CWorldSocket>(next.get());
    SSL sa{2}, sb{2}; ReceiveFailure failure;
    first->m_pSocket=a.get(); next->m_pSocket=b.get();
    first->m_pSSL=&sa; next->m_pSSL=&sb;
    first->m_bSSL_Connected=next->m_bSSL_Connected=true;
    docTemplate.docs={first.get(),next.get()};
    first->receive=next->receive=[&] { throw &failure; };
    for (auto *s : {a.get(),b.get()}) {
      bool caught=false;try{s->OnReceive(0);}catch(ReceiveFailure *e){assert(e==&failure);caught=true;}assert(caught);
    }
    int calls=0;
    next->receive=[&]{assert(false);};
    first->receive=[&]{++calls;docTemplate.docs.clear();a.reset();b.reset();first.reset();next.reset();if(fail)throw &failure;};
    bool caught=false;try{frame.OnTimer(1);}catch(ReceiveFailure *e){assert(e==&failure);caught=true;}
    assert(caught==fail && calls==1 && !a && !b && !first && !next);
  }
  std::cout << "PASS: one timer receive can delete both current and next worlds without using a stale position\n";

  {
    Fixture f; CMainFrame frame; SSL ssl{2};
    f.doc.m_pSSL=&ssl; f.doc.m_bSSL_Connected=true;
    f.doc.receive=[&]{throw &f.failure;}; f.fail();
    frame.fallback=[&]{docTemplate.docs.clear();};
    frame.OnTimer(1);assert(f.socket.m_bBufferedReadPending);
    assert(frame.fallbacks==1 && frame.baseTimers==1);
  }
  {
    Fixture f; CMainFrame frame; SSL ssl{2};
    auto socket=std::make_unique<CWorldSocket>(&f.doc);f.doc.m_pSocket=socket.get();
    f.doc.m_pSSL=&ssl;f.doc.m_bSSL_Connected=true;
    auto oldId=socket->m_iSocketNumber;
    f.doc.receive=[&]{
      // Model allocator address reuse. The replacement has a distinct identity.
      auto *where=socket.get();where->~CWorldSocket();new(where) CWorldSocket(&f.doc);
      throw &f.failure;
    };
    bool caught=false;try{socket->OnReceive(0);}catch(ReceiveFailure *e){assert(e==&f.failure);caught=true;}
    assert(caught && socket->m_iSocketNumber!=oldId && !socket->m_bBufferedReadPending && !socket->m_bInReceive);
    frame.OnTimer(1);f.doc.m_pSocket=nullptr;
  }
  std::cout << "PASS: fallback deletion and same-address socket replacement do not reuse pending state\n";
  docTemplate.docs.clear();
}
