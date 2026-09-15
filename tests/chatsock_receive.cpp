// Only the receive wrapper comes from production. The parser and Windows calls
// are modeled here. FD_READ rearms after recv, including MSG_PEEK and errors:
// https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsaasyncselect
#include <cassert>
#include <cstdio>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using __int64 = long long;
using POSITION = size_t;
using SOCKET = int;
constexpr SOCKET INVALID_SOCKET = -1;
constexpr int SOCKET_ERROR = -1, WSAEWOULDBLOCK = 10035, WSAECONNRESET = 10054;
constexpr int MSG_PEEK = 2, MB_OK = 0, MB_ICONERROR = 16, MB_TASKMODAL = 8192;
constexpr int FD_READ = 1, FD_WRITE = 2, FD_CLOSE = 32;
enum { eChatClosed, eChatConnecting, eChatAwaitingConnectConfirm,
       eChatAwaitingConnectionRequest, eChatConnected };
int lastError = 0, destroyed = 0;
std::string report;
std::function<void()> onReport;
int MessageBoxA(void *, const char *message, const char *, int flags) {
  assert(flags == (MB_OK | MB_ICONERROR | MB_TASKMODAL));
  report = message;
  if (onReport) onReport();
  return 1;
}
struct CAsyncSocket {
  SOCKET m_hSocket = 42;
  int eventMask = FD_READ | FD_WRITE | FD_CLOSE, peekCalls = 0, peekError = 0;
  bool enabled = true, queued = false, eof = false;
  std::string unread;
  void notify() {
    if (enabled && !unread.empty() && (eventMask & FD_READ)) {
      queued = true;
      enabled = false;
    }
  }
  void arrive(const std::string &data) { unread += data; notify(); }
  std::string read() {
    std::string data = unread;
    unread.clear();
    enabled = true;
    notify();
    return data;
  }
  int Receive(void *buffer, int length, int flags) {
    assert(m_hSocket != INVALID_SOCKET && length == 1 && flags == MSG_PEEK);
    ++peekCalls;
    enabled = true;
    notify();
    if (peekError) { lastError = peekError; return SOCKET_ERROR; }
    if (!unread.empty()) { *static_cast<char *>(buffer) = unread[0]; return 1; }
    if (eof) return 0;
    lastError = WSAEWOULDBLOCK;
    return SOCKET_ERROR;
  }
  static int GetLastError() { return lastError; }
};
struct CMUSHclientDoc;
struct CChatSocket : CAsyncSocket {
  CMUSHclientDoc *m_pDoc;
  long m_iChatID = 7;
  int m_iChatStatus = eChatConnected, reads = 0;
  bool m_bInReceive = false, m_bReceivePending = false, m_bDeleteMe = false;
  std::string parsed;
  std::function<void()> afterRead;
  explicit CChatSocket(CMUSHclientDoc *doc) : m_pDoc(doc) {}
  ~CChatSocket() { ++destroyed; }
  void OnReceive(int);
  void ReceiveOneNotification(int) {
    ++reads;
    parsed += read();
    // Copy the callback before it can delete the socket. No access after it.
    auto callback = afterRead;
    if (callback) callback();
  }
  void dispatch() { assert(queued); queued = false; OnReceive(0); }
};
template <class T> struct PointerList {
  std::vector<T *> values;
  POSITION GetHeadPosition() const { return values.empty() ? 0 : 1; }
  T *GetNext(POSITION &pos) {
    auto *value = values.at(pos - 1);
    pos = pos < values.size() ? pos + 1 : 0;
    return value;
  }
};
struct CMUSHclientDoc {
  __int64 m_iUniqueDocumentNumber = 101;
  PointerList<CChatSocket> m_ChatList;
};
struct DocTemplate : PointerList<CMUSHclientDoc> {
  POSITION GetFirstDocPosition() const { return GetHeadPosition(); }
  CMUSHclientDoc *GetNextDoc(POSITION &pos) { return GetNext(pos); }
} documents;
struct Application { DocTemplate *m_pWorldDocTemplate = &documents; } App;
#include "chatsock_receive.inc"

struct OriginalFailure {};
void expectOriginal(const std::function<void()> &action) {
  bool caught = false;
  try { action(); } catch (const OriginalFailure &) { caught = true; }
  assert(caught);
}
struct Fixture {
  CMUSHclientDoc doc;
  CChatSocket *socket = new CChatSocket(&doc);
  Fixture() {
    documents.values = {&doc};
    doc.m_ChatList.values = {socket};
    report.clear();
    onReport = {};
  }
  void eraseSocket() {
    doc.m_ChatList.values.clear();
    delete socket;
    socket = nullptr;
  }
  ~Fixture() { delete socket; documents.values.clear(); onReport = {}; }
};
int main() {
  for (int status : {eChatConnecting, eChatAwaitingConnectConfirm,
                     eChatAwaitingConnectionRequest, eChatConnected})
  for (int mask : {FD_READ, FD_READ | FD_WRITE | FD_CLOSE}) {
    Fixture f;
    auto *s = f.socket;
    s->eventMask = mask;
    s->m_iChatStatus = status;
    s->afterRead = [&] {
      s->arrive("B"); s->dispatch();
      assert(s->reads == 1 && s->m_bReceivePending);
      throw OriginalFailure{};
    };
    s->arrive("A");
    expectOriginal([&] { s->dispatch(); });
    assert(s->peekCalls == 1 && s->unread == "B" && s->queued);
    assert(!s->m_bInReceive && !s->m_bReceivePending && s->eventMask == mask);
    assert(s->parsed == "A"); // Recovery must not parse inside catch.
    s->afterRead = {};
    s->dispatch();
    s->arrive("C"); s->dispatch();
    assert(s->parsed == "ABC" && s->reads == 3 && report.empty());
  }
  std::cout << "PASS connecting and connected recovery preserves bytes, later reads and event masks\n";
  {
    Fixture f;
    auto *s = f.socket;
    s->afterRead = [&] {
      if (s->reads == 1) { s->arrive("B"); s->dispatch(); assert(s->reads == 1); }
    };
    s->arrive("A"); s->dispatch();
    assert(s->reads == 2 && s->parsed == "AB" && s->peekCalls == 0);
  }
  {
    Fixture f;
    auto *s = f.socket;
    s->afterRead = [] { throw OriginalFailure{}; };
    s->arrive("A"); expectOriginal([&] { s->dispatch(); });
    assert(s->peekCalls == 0 && !s->m_bInReceive && !s->m_bReceivePending);
  }
  std::cout << "PASS normal serial drain and exception without a pending read\n";
  {
    Fixture f;
    auto *s = f.socket;
    OriginalFailure failure;
    s->afterRead = [&] {
      s->arrive("B"); s->dispatch();
      throw &failure; // MFC throws exception pointers.
    };
    s->arrive("A");
    bool caught = false;
    try { s->dispatch(); }
    catch (OriginalFailure *original) { assert(original == &failure); caught = true; }
    assert(caught && s->peekCalls == 1 && s->queued && s->unread == "B");
  }
  {
    Fixture f;
    auto *s = f.socket;
    s->afterRead = [&] {
      s->arrive("B"); s->dispatch();
      s->eventMask = 0;
      throw OriginalFailure{};
    };
    s->arrive("A"); expectOriginal([&] { s->dispatch(); });
    assert(s->peekCalls == 1 && s->eventMask == 0 && !s->queued && s->unread == "B");
  }
  std::cout << "PASS exact MFC exception pointer and disabled event mask\n";

  for (int mode = 0; mode < 3; ++mode) {
    Fixture f;
    auto *s = f.socket;
    s->eof = mode == 1;
    s->peekError = mode == 2 ? WSAECONNRESET : 0;
    s->afterRead = [&] { s->OnReceive(0); throw OriginalFailure{}; };
    s->arrive("A"); expectOriginal([&] { s->dispatch(); });
    assert(s->peekCalls == 1 && !s->m_bInReceive && !s->m_bReceivePending);
    if (mode == 2) assert(report.find("10054") != std::string::npos);
    else assert(report.empty());
  }
  std::cout << "PASS would-block, EOF and visible unexpected socket errors\n";
  for (int mode = 0; mode < 8; ++mode) {
    Fixture f;
    auto *s = f.socket;
    s->afterRead = [&] {
      s->arrive("B"); s->dispatch();
      if (mode == 0) s->m_bDeleteMe = true;
      if (mode == 1) s->m_iChatStatus = eChatClosed;
      if (mode == 2) s->m_hSocket = INVALID_SOCKET;
      if (mode == 3) s->m_hSocket = 99;
      if (mode == 4) f.doc.m_ChatList.values.clear();
      if (mode == 5) documents.values.clear();
      if (mode == 6) ++f.doc.m_iUniqueDocumentNumber;
      if (mode == 7) ++s->m_iChatID;
      throw OriginalFailure{};
    };
    s->arrive("A"); expectOriginal([&] { s->dispatch(); });
    assert(s->peekCalls == 0);
    if (mode < 4) assert(!s->m_bInReceive && !s->m_bReceivePending);
    else assert(s->m_bInReceive && s->m_bReceivePending); // Do not touch a stale identity.
  }
  std::cout << "PASS pending deletion, closed/replaced handle and document/chat identities\n";
  {
    Fixture f;
    auto *s = f.socket;
    s->afterRead = [&] { f.eraseSocket(); throw OriginalFailure{}; };
    s->arrive("A"); expectOriginal([&] { s->dispatch(); });
    assert(f.socket == nullptr);
  }
  {
    Fixture f;
    auto *s = f.socket;
    s->peekError = WSAECONNRESET;
    s->afterRead = [&] { s->arrive("B"); s->dispatch(); throw OriginalFailure{}; };
    onReport = [&] {
      assert(!s->m_bInReceive && !s->m_bReceivePending);
      f.eraseSocket();
    };
    s->arrive("A"); expectOriginal([&] { s->dispatch(); });
    assert(f.socket == nullptr && report.find("10054") != std::string::npos);
  }
  std::cout << "PASS socket deletion before catch and during error reporting\n";
  {
    auto *doc = new CMUSHclientDoc;
    auto *s = new CChatSocket(doc);
    documents.values = {doc};
    doc->m_ChatList.values = {s};
    s->afterRead = [&] {
      s->arrive("B"); s->dispatch();
      documents.values.clear();
      delete doc;
      delete s;
      throw OriginalFailure{};
    };
    s->arrive("A"); expectOriginal([&] { s->dispatch(); });
  }
  std::cout << "PASS document and chat destruction during the callback\n";

  {
    Fixture f;
    auto *s = f.socket;
    s->afterRead = [&] {
      s->arrive("B"); s->dispatch();
      s->m_bDeleteMe = true;
      // This is the production timer deletion predicate.
      assert(!(s->m_bDeleteMe && !s->m_bInReceive));
    };
    s->arrive("A"); s->dispatch();
    assert(s->reads == 1 && !s->m_bInReceive && s->m_bDeleteMe);
    assert(s->m_bDeleteMe && !s->m_bInReceive);
  }
  std::cout << "PASS pending deletion stops the drain and remains eligible for timer cleanup\n";
}
