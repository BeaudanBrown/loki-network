#include <gtest/gtest.h>
#include <libabyss.hpp>
#include <ev.h>
#include <threading.hpp>
#include <net.hpp>

struct AbyssTestBase : public ::testing::Test
{
  llarp::Crypto crypto;
  llarp_threadpool* threadpool = nullptr;
  llarp_ev_loop* loop          = nullptr;
  std::unique_ptr< llarp::Logic > logic;
  abyss::httpd::BaseReqHandler* server = nullptr;
  abyss::http::JSONRPC* client         = nullptr;
  const std::string method             = "test.method";
  bool called                          = false;

  AbyssTestBase() : crypto(llarp::Crypto::sodium{})
  {
  }

  void
  AssertMethod(const std::string& meth) const
  {
    ASSERT_TRUE(meth == method);
  }

  void
  SetUp()
  {
    llarp::SetLogLevel(llarp::eLogDebug);
    // for llarp::randint
  }

  static void
  CancelIt(void* u, __attribute__((unused)) uint64_t orig, uint64_t left)
  {
    if(left)
      return;
    static_cast< AbyssTestBase* >(u)->Stop();
  }

  static void
  StopIt(void* u)
  {
    static_cast< AbyssTestBase* >(u)->Stop();
  }

  void
  Start()
  {
    threadpool = llarp_init_same_process_threadpool();
    llarp_ev_loop_alloc(&loop);
    logic.reset(new llarp::Logic(threadpool));

    sockaddr_in addr;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons((llarp::randint() % 2000) + 2000);
    addr.sin_family      = AF_INET;
    llarp::Addr a(addr);
    while(true)
    {
      if(server->ServeAsync(loop, logic.get(), a))
      {
        client->RunAsync(loop, a.ToString());
        logic->call_later({1000, this, &CancelIt});
        return;
      }
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }

  void
  Stop()
  {
    llarp::LogDebug("test case Stop() called");
    if(server)
      server->Close();
    llarp_ev_loop_stop(loop);
  }

  void
  AsyncStop()
  {
    logic->queue_job({this, &StopIt});
  }

  void
  TearDown()
  {
    logic.reset();
    llarp_ev_loop_free(&loop);
    llarp_free_threadpool(&threadpool);
    llarp::SetLogLevel(llarp::eLogInfo);
  }
};

struct ClientHandler : public abyss::http::IRPCClientHandler
{
  AbyssTestBase* test;
  ClientHandler(abyss::http::ConnImpl* impl, AbyssTestBase* parent)
      : abyss::http::IRPCClientHandler(impl), test(parent)
  {
  }

  void
  HandleError()
  {
    ASSERT_TRUE(false);
  }

  void
  PopulateReqHeaders(__attribute__((unused)) abyss::http::Headers_t& hdr)
  {
  }

  bool
  HandleResponse(__attribute__((unused)) abyss::http::RPC_Response response)
  {
    test->AsyncStop();
    return true;
  }
};

struct ServerHandler : public abyss::httpd::IRPCHandler
{
  AbyssTestBase* test;
  ServerHandler(abyss::httpd::ConnImpl* impl, AbyssTestBase* parent)
      : abyss::httpd::IRPCHandler(impl), test(parent)
  {
  }

  bool
  HandleJSONRPC(Method_t method, __attribute__((unused)) const Params& params,
                __attribute__((unused)) Response& response)
  {
    test->AssertMethod(method);
    test->called = true;
    return true;
  }
};

struct AbyssTest : public AbyssTestBase,
                   public abyss::http::JSONRPC,
                   public abyss::httpd::BaseReqHandler
{
  AbyssTest()
      : AbyssTestBase()
      , abyss::http::JSONRPC()
      , abyss::httpd::BaseReqHandler(1000)

  {
  }

  abyss::http::IRPCClientHandler*
  NewConn(abyss::http::ConnImpl* impl)
  {
    return new ClientHandler(impl, this);
  }

  abyss::httpd::IRPCHandler*
  CreateHandler(abyss::httpd::ConnImpl* impl)
  {
    return new ServerHandler(impl, this);
  }

  void
  SetUp()
  {
    AbyssTestBase::SetUp();
    client = this;
    server = this;
  }

  static void
  FlushIt(void* u)
  {
    static_cast< AbyssTest* >(u)->Flush();
  }

  void
  AsyncFlush()
  {
    logic->queue_job({this, &FlushIt});
  }

  void
  RunLoop()
  {
    llarp_ev_loop_run_single_process(loop, threadpool, logic.get());
  }
};

TEST_F(AbyssTest, TestClientAndServer)
{
  Start();
  abyss::json::Value params;
  params.SetObject();
  QueueRPC(method, std::move(params),
           std::bind(&AbyssTest::NewConn, this, std::placeholders::_1));

  AsyncFlush();
  RunLoop();
  ASSERT_TRUE(called);
};
