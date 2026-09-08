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

struct SSL { int pending = 0; };
int SSL_pending(SSL *ssl) { return ssl->pending; }
class CMUSHclientDoc {
public:
  CWorldSocket *m_pSocket = nullptr;
  SSL *m_pSSL = nullptr;
  bool m_bSSL_Connected = false;
  std::function<void()> receive;
  void ReceiveMsg() { receive(); }
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
struct { DocTemplate *m_pWorldDocTemplate = &docTemplate; } App;
#include "worldsock_receive.inc"
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
}
