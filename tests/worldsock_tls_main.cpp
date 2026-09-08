int main() {
  CMainFrame frame;
  SSL_CTX *clientCtx = SSL_CTX_new(TLS_client_method());
  SSL_CTX *serverCtx = SSL_CTX_new(TLS_server_method());
  assert(clientCtx && serverCtx);
  for(auto ctx : {clientCtx, serverCtx}) {
    assert(SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION));
    assert(SSL_CTX_set_max_proto_version(ctx, TLS1_2_VERSION));
    assert(SSL_CTX_set_cipher_list(ctx, "PSK-AES128-GCM-SHA256"));
  }
  SSL_CTX_set_psk_client_callback(clientCtx, clientPsk);
  SSL_CTX_set_psk_server_callback(serverCtx, serverPsk);
  SSL *client = SSL_new(clientCtx), *server = SSL_new(serverCtx);
  assert(client && server);
  BIO *clientIn = BIO_new(BIO_s_mem()), *clientOut = BIO_new(BIO_s_mem());
  BIO *serverIn = BIO_new(BIO_s_mem()), *serverOut = BIO_new(BIO_s_mem());
  BIO_METHOD *method = BIO_meth_new(BIO_TYPE_FILTER, "count transport reads");
  assert(method);
  BIO_meth_set_read(method, traceRead); BIO_meth_set_write(method, traceWrite);
  BIO_meth_set_ctrl(method, traceCtrl);
  BIO *filter = BIO_new(method); assert(filter);
  BIO_set_init(filter, 1); BIO_push(filter, clientIn);
  encryptedInput = clientIn;
  SSL_set_bio(client, filter, clientOut); SSL_set_bio(server, serverIn, serverOut);
  SSL_set_connect_state(client); SSL_set_accept_state(server);
  for(int i = 0; i < 20 && (!SSL_is_init_finished(client) || !SSL_is_init_finished(server)); ++i) {
    int a = SSL_do_handshake(client);
    assert(a == 1 || SSL_get_error(client, a) == SSL_ERROR_WANT_READ);
    moveBytes(clientOut, serverIn);
    int b = SSL_do_handshake(server);
    assert(b == 1 || SSL_get_error(server, b) == SSL_ERROR_WANT_READ);
    moveBytes(serverOut, clientIn);
  }
  assert(SSL_is_init_finished(client) && SSL_is_init_finished(server));
  assert(BIO_ctrl_pending(clientIn) == 0);
  string message(16000, 'X');
  assert(SSL_write(server, message.data(), message.size()) == 16000);
  moveBytes(serverOut, clientIn);
  recordReadsEnabled = true;
  CMUSHclientDoc doc; CWorldSocket socket(&doc);
  doc.m_pSocket = &socket; doc.m_pSSL = client; docTemplate.docs = {&doc};
  int calls = 0, delivered = 0, pendingBeforeModal = 0;
  struct ReceiveFailure {} failure;
  doc.receive = [&] {
    ++calls;
    char buff[9000]; // Exact ReceiveMsg capacity and SSL_read call size.
    int count = SSL_read(doc.m_pSSL, buff, sizeof(buff) - 1);
    if(count <= 0) {
      assert(SSL_get_error(doc.m_pSSL, count) == SSL_ERROR_WANT_READ);
      return; // ReceiveMsg returns on SSL_ERROR_WANT_READ.
    }
    delivered += count;
    if(calls == 1) {
      pendingBeforeModal = SSL_pending(client);
      assert(count == 8999 && pendingBeforeModal == 7001);
      assert(BIO_ctrl_pending(clientIn) == 0 && staleReadQueued);
      staleReadQueued = false;
      #if NESTED_READ
      socket.OnReceive(0); // A modal callback dispatches the queued stale FD_READ.
      #endif
      frame.OnTimer(1); // No retry while ReceiveMsg is still active.
      assert(delivered == 8999);
      #if THROW_AFTER_CALLBACK
      throw &failure; // Native failure after the callback returns.
#endif
    }
  };
  bool caught = false;
  try { socket.OnReceive(0); }
  catch(ReceiveFailure *value) { assert(value == &failure); caught = true; }
  assert(caught == bool(THROW_AFTER_CALLBACK));
  if (THROW_AFTER_CALLBACK) {
    assert(delivered == 8999 && SSL_pending(client) == 7001);
    assert(!socket.nextReadQueued && socket.peekCalls == NESTED_READ);
  }
  frame.OnTimer(1);
  assert(delivered == 16000 && SSL_pending(client) == 0);
  assert(frame.fallbacks == 2 && frame.baseTimers == 2);
  int afterDrainCalls = calls;
  frame.OnTimer(1); // Completed retries do not read again.
  assert(calls == afterDrainCalls);
  string later = "later packet";
  assert(SSL_write(server, later.data(), later.size()) == int(later.size()));
  moveBytes(serverOut, clientIn);
  socket.OnReceive(0);
  assert(delivered == 16000 + int(later.size()) && SSL_pending(client) == 0);
  std::cout << OpenSSL_version(OPENSSL_VERSION) << '\n';
  std::cout << "transport_reads=";
  for(int n : transportReads) std::cout << n << ',';
  std::cout << " pending_before_modal=" << pendingBeforeModal
            << " transport_bytes=" << BIO_ctrl_pending(clientIn)
            << " decrypted_bytes=" << SSL_pending(client)
            << " delivered=" << delivered << " receive_calls=" << calls
            << " peek_calls=" << socket.peekCalls
            << " queued=" << socket.nextReadQueued << '\n';
  assert(!socket.m_bInReceive && !socket.m_bReceivePending);
  SSL_free(client); SSL_free(server); SSL_CTX_free(clientCtx); SSL_CTX_free(serverCtx);
  BIO_meth_free(method);
}
