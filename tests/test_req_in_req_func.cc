/**
 * @file test_nested_rpc.cc
 * @brief Test issuing requests from within request handlers.
 */
#include "test_basics.h"

// Set to true if the request handler or contination at server 0 or 1 should
// run in the background.
bool server_0_bg, server_1_bg;

static constexpr size_t kAppNumReqs = 30;
static_assert(kAppNumReqs > Session::kSessionReqWindow, "");

/// Request type used for client to server 0
static constexpr uint8_t kAppReqTypeCS = kAppReqType + 1;

/// Request type used for server 0 to server 1
static constexpr uint8_t kAppReqTypeSS = kAppReqType + 2;

/// Per-request info maintained at the server
class ServerReqInfo {
 public:
  /// The request size of the client-to-server request
  size_t req_size_cs;

  /// The request handle for the client-to-server request
  ReqHandle *req_handle_cs;

  /// The MsgBuffer used for the server-to-server request
  MsgBuffer req_msgbuf_ss;

  ServerReqInfo(size_t req_size_cs, ReqHandle *req_handle_cs)
      : req_size_cs(req_size_cs), req_handle_cs(req_handle_cs) {}
};

union tag_t {
  ServerReqInfo *srv_req_info_ptr;
  struct {
    uint16_t req_i;
    uint16_t msgbuf_i;
    uint32_t req_size;
  };
  size_t tag;

  tag_t(ServerReqInfo *srv_req_info_ptr) : srv_req_info_ptr(srv_req_info_ptr) {}
  tag_t(uint16_t req_i, uint16_t msgbuf_i, uint32_t req_size)
      : req_i(req_i), msgbuf_i(msgbuf_i), req_size(req_size) {}
  tag_t(size_t tag) : tag(tag) {}
};
static_assert(sizeof(tag_t) == sizeof(size_t), "");

/// Per-thread application context
class AppContext : public BasicAppContext {
 public:
  FastRand fast_rand;
  MsgBuffer req_msgbuf[Session::kSessionReqWindow];
  size_t num_reqs_sent = 0;
};

/// Pick a random message size (>= 1 byte)
size_t get_rand_msg_size(AppContext *app_context) {
  assert(app_context != nullptr);
  uint32_t sample = app_context->fast_rand.next_u32();
  uint32_t msg_size = sample % Rpc<IBTransport>::kMaxMsgSize;
  if (msg_size == 0) {
    msg_size = 1;
  }

  return (size_t)msg_size;
}

///
/// Server-side code
///

// Forward declaration
void server_cont_func(RespHandle *, void *, size_t);

/// Server 0's request handler for client to server requests. Forwards the
/// received request to server #1.
void req_handler_cs(ReqHandle *req_handle_cs, void *_context) {
  assert(req_handle_cs != nullptr);
  assert(_context != nullptr);

  auto *context = (AppContext *)_context;
  assert(!context->is_client);
  ASSERT_EQ(context->rpc->in_background(), server_0_bg);

  const MsgBuffer *req_msgbuf_cs = req_handle_cs->get_req_msgbuf();
  size_t req_size_cs = req_msgbuf_cs->get_data_size();

  test_printf("Server %u: Received client-server request of length %zu.\n",
              context->rpc->get_rpc_id(), req_size_cs);

  // Record info for the request we're now sending to server #1
  ServerReqInfo *srv_req_info = new ServerReqInfo(req_size_cs, req_handle_cs);

  MsgBuffer &req_msgbuf_ss = srv_req_info->req_msgbuf_ss;
  req_msgbuf_ss = context->rpc->alloc_msg_buffer(req_size_cs);
  assert(req_msgbuf_ss.buf != nullptr);

  // Request to server #1 = client-to-server request + 1
  for (size_t i = 0; i < req_size_cs; i++) {
    req_msgbuf_ss.buf[i] = req_msgbuf_cs->buf[i] + 1;
  }

  tag_t tag(srv_req_info);  // Save the request info pointer in the tag

  int ret = context->rpc->enqueue_request(
      context->session_num_arr[1], kAppReqTypeSS, &srv_req_info->req_msgbuf_ss,
      server_cont_func, tag.tag);
  _unused(ret);
  assert(ret == 0);
}

/// Server 1's request handler for server to server requests. Echoes the
/// received request back to server 0.
void req_handler_ss(ReqHandle *req_handle, void *_context) {
  assert(req_handle != nullptr);
  assert(_context != nullptr);

  auto *context = (AppContext *)_context;
  assert(!context->is_client);
  ASSERT_EQ(context->rpc->in_background(), server_1_bg);

  const MsgBuffer *req_msgbuf_ss = req_handle->get_req_msgbuf();
  size_t req_size = req_msgbuf_ss->get_data_size();

  test_printf("Server %u: Received server-server request of length %zu.\n",
              context->rpc->get_rpc_id(), req_size);

  // eRPC will free dyn_resp_msgbuf
  req_handle->dyn_resp_msgbuf = context->rpc->alloc_msg_buffer(req_size);
  assert(req_handle->dyn_resp_msgbuf.buf != nullptr);

  // Response to server #0 = server-to-server request + 1
  for (size_t i = 0; i < req_size; i++) {
    req_handle->dyn_resp_msgbuf.buf[i] = req_msgbuf_ss->buf[i] + 1;
  }

  req_handle->prealloc_used = false;
  context->rpc->enqueue_response(req_handle);
}

/// Server 0's continuation invoked when it gets a response from server 1
void server_cont_func(RespHandle *resp_handle_ss, void *_context, size_t _tag) {
  assert(resp_handle_ss != nullptr);
  assert(_context != nullptr);

  auto *context = (AppContext *)_context;
  assert(!context->is_client);
  ASSERT_EQ(context->rpc->in_background(), server_0_bg);

  const MsgBuffer *resp_msgbuf_ss = resp_handle_ss->get_resp_msgbuf();
  test_printf("Server %u: Received server-server response of length %zu.\n",
              context->rpc->get_rpc_id(),
              (char *)resp_msgbuf_ss->get_data_size());

  // Extract the request info
  tag_t tag(_tag);
  ServerReqInfo *srv_req_info = (ServerReqInfo *)tag.srv_req_info_ptr;
  size_t req_size_cs = srv_req_info->req_size_cs;
  ReqHandle *req_handle_cs = srv_req_info->req_handle_cs;
  MsgBuffer &req_msgbuf_ss = srv_req_info->req_msgbuf_ss;

  assert(resp_msgbuf_ss->get_data_size() == req_size_cs);

  // Check the response from server #1
  for (size_t i = 0; i < req_size_cs; i++) {
    assert(req_msgbuf_ss.buf[i] + 1 == resp_msgbuf_ss->buf[i]);
  }

  // eRPC will free dyn_resp_msgbuf
  req_handle_cs->dyn_resp_msgbuf = context->rpc->alloc_msg_buffer(req_size_cs);

  // Response to client = server-to-server response + 1
  for (size_t i = 0; i < req_size_cs; i++) {
    req_handle_cs->dyn_resp_msgbuf.buf[i] = resp_msgbuf_ss->buf[i] + 1;
  }

  // Free resources of the server-to-server request
  context->rpc->free_msg_buffer(req_msgbuf_ss);
  delete srv_req_info;

  // Release the server-server response
  context->rpc->release_respone(resp_handle_ss);

  // Send response to the client
  req_handle_cs->prealloc_used = false;
  context->rpc->enqueue_response(req_handle_cs);
}

///
/// Client-side code
///
void client_cont_func(RespHandle *, void *, size_t);  // Forward declaration

/// Enqueue a request to server 0 using the request MsgBuffer index msgbuf_i
void client_request_helper(AppContext *context, size_t msgbuf_i) {
  assert(context != nullptr && msgbuf_i < Session::kSessionReqWindow);

  size_t req_size = get_rand_msg_size(context);
  context->rpc->resize_msg_buffer(&context->req_msgbuf[msgbuf_i], req_size);

  // Fill in all the bytes of the request MsgBuffer with msgbuf_i
  MsgBuffer &req_msgbuf = context->req_msgbuf[msgbuf_i];
  for (size_t i = 0; i < req_size; i++) {
    req_msgbuf.buf[i] = (uint8_t)msgbuf_i;
  }

  tag_t tag((uint16_t)context->num_reqs_sent, (uint16_t)msgbuf_i,
            (uint32_t)req_size);
  test_printf("Client: Sending request %zu of size %zu\n",
              context->num_reqs_sent, req_size);

  int ret =
      context->rpc->enqueue_request(context->session_num_arr[0], kAppReqTypeCS,
                                    &req_msgbuf, client_cont_func, tag.tag);
  _unused(ret);
  assert(ret == 0);

  context->num_reqs_sent++;
}

void client_cont_func(RespHandle *resp_handle, void *_context, size_t _tag) {
  assert(resp_handle != nullptr);
  assert(_context != nullptr);

  auto *context = (AppContext *)_context;
  assert(context->is_client);

  const MsgBuffer *resp_msgbuf = resp_handle->get_resp_msgbuf();

  // Extract info from tag
  tag_t tag(_tag);
  size_t req_size = (size_t)(tag.req_size);
  size_t msgbuf_i = (size_t)(tag.msgbuf_i);

  test_printf("Client: Received response for req %u, length = %zu.\n",
              tag.req_i, (char *)resp_msgbuf->get_data_size());

  // Check the response
  ASSERT_EQ(resp_msgbuf->get_data_size(), req_size);
  for (size_t i = 0; i < req_size; i++) {
    ASSERT_EQ(resp_msgbuf->buf[i], ((uint8_t)msgbuf_i) + 3);
  }

  context->num_rpc_resps++;
  context->rpc->release_respone(resp_handle);

  if (context->num_reqs_sent < kAppNumReqs) {
    client_request_helper(context, ((tag_t)tag).msgbuf_i);
  }
}

void client_thread(Nexus<IBTransport> *nexus, size_t num_sessions) {
  // Create the Rpc and connect the sessions
  AppContext context;
  client_connect_sessions(nexus, context, num_sessions, basic_sm_handler);

  Rpc<IBTransport> *rpc = context.rpc;

  // Start by filling the request window
  for (size_t i = 0; i < Session::kSessionReqWindow; i++) {
    context.req_msgbuf[i] =
        rpc->alloc_msg_buffer(Rpc<IBTransport>::kMaxMsgSize);
    assert(context.req_msgbuf[i].buf != nullptr);
    client_request_helper(&context, i);
  }

  wait_for_rpc_resps_or_timeout(context, kAppNumReqs, nexus->freq_ghz);
  assert(context.num_rpc_resps == kAppNumReqs);

  for (size_t i = 0; i < Session::kSessionReqWindow; i++) {
    rpc->free_msg_buffer(context.req_msgbuf[i]);
  }

  // Disconnect the sessions
  context.num_sm_resps = 0;
  for (size_t i = 0; i < num_sessions; i++) {
    rpc->destroy_session(context.session_num_arr[i]);
  }
  wait_for_sm_resps_or_timeout(context, num_sessions, nexus->freq_ghz);

  // Free resources
  delete rpc;
  client_done = true;
}

/// Both server 0 and server 1 run in the foreground
TEST(SendReqInReqFunc, BothForeground) {
  server_0_bg = false;
  server_1_bg = false;

  auto reg_info_vec = {
      ReqFuncRegInfo(kAppReqTypeCS, req_handler_cs,
                     ReqFuncType::kFgNonterminal),
      ReqFuncRegInfo(kAppReqTypeSS, req_handler_ss, ReqFuncType::kFgTerminal)};

  // 2 client sessions (=> 2 server threads), 0 background threads
  launch_server_client_threads(2, 0, client_thread, reg_info_vec,
                               ConnectServers::kTrue);
}

/// Server 0 runs in background, server 1 in foreground
TEST(SendReqInReqFunc, ServerZeroBackground) {
  server_0_bg = true;
  server_1_bg = false;

  auto reg_info_vec = {
      ReqFuncRegInfo(kAppReqTypeCS, req_handler_cs, ReqFuncType::kBackground),
      ReqFuncRegInfo(kAppReqTypeSS, req_handler_ss, ReqFuncType::kFgTerminal)};

  // 2 client sessions (=> 2 server threads), 1 background thread
  launch_server_client_threads(2, 1, client_thread, reg_info_vec,
                               ConnectServers::kTrue);
}

/// Both server 0 and server 1 run in the background
TEST(SendReqInReqFunc, BothBackground) {
  server_0_bg = true;
  server_1_bg = true;

  auto reg_info_vec = {
      ReqFuncRegInfo(kAppReqTypeCS, req_handler_cs, ReqFuncType::kBackground),
      ReqFuncRegInfo(kAppReqTypeSS, req_handler_ss, ReqFuncType::kBackground)};

  // 2 client sessions (=> 2 server threads), 1 background thread
  launch_server_client_threads(2, 1, client_thread, reg_info_vec,
                               ConnectServers::kTrue);
}

int main(int argc, char **argv) {
  Nexus<IBTransport>::get_hostname(local_hostname);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
