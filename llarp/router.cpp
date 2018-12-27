#include <buffer.hpp>
#include <encode.hpp>
#include <iwp.hpp>
#include <link/server.hpp>
#include <link/utp.hpp>
#include <link_message.hpp>
#include <logger.hpp>
#include <net.hpp>
#include <proto.hpp>
#include <router.hpp>
#include <rpc.hpp>
#include <str.hpp>
#include <crypto.hpp>

#include <fstream>
#include <cstdlib>
#if defined(RPI) || defined(ANDROID)
#include <unistd.h>
#endif

namespace llarp
{
  void
  router_iter_config(llarp_config_iterator *iter, const char *section,
                     const char *key, const char *val);

  struct async_verify_context
  {
    Router *router;
    TryConnectJob *establish_job;
  };

}  // namespace llarp

struct TryConnectJob
{
  llarp::RouterContact rc;
  llarp::ILinkLayer *link;
  llarp::Router *router;
  uint16_t triesLeft;
  TryConnectJob(const llarp::RouterContact &remote, llarp::ILinkLayer *l,
                uint16_t tries, llarp::Router *r)
      : rc(remote), link(l), router(r), triesLeft(tries)
  {
  }

  void
  Failed()
  {
    llarp::LogInfo("session to ", llarp::RouterID(rc.pubkey.data()), " closed");
    link->CloseSessionTo(rc.pubkey);
  }

  void
  Success()
  {
    router->FlushOutboundFor(rc.pubkey, link);
  }

  void
  AttemptTimedout()
  {
    router->routerProfiling.MarkTimeout(rc.pubkey);
    if(ShouldRetry())
    {
      Attempt();
      return;
    }
    if(!router->IsServiceNode())
    {
      if(router->routerProfiling.IsBad(rc.pubkey))
        router->nodedb->Remove(rc.pubkey);
    }
    // delete this
    router->pendingEstablishJobs.erase(rc.pubkey);
  }

  void
  Attempt()
  {
    --triesLeft;
    if(!link->TryEstablishTo(rc))
      llarp::LogError("did not attempt connection to ", rc.pubkey,
                      " and it has ", rc.addrs.size(), " advertised addresses");
  }

  bool
  ShouldRetry() const
  {
    return triesLeft > 0;
  }
};

static void
on_try_connecting(void *u)
{
  TryConnectJob *j = static_cast< TryConnectJob * >(u);
  j->Attempt();
}

bool
llarp_router_try_connect(llarp::Router *router,
                         const llarp::RouterContact &remote,
                         uint16_t numretries)
{
  // do we already have a pending job for this remote?
  if(router->HasPendingConnectJob(remote.pubkey))
  {
    llarp::LogDebug("We have pending connect jobs to ", remote.pubkey);
    return false;
  }

  auto link          = router->outboundLink.get();
  auto itr           = router->pendingEstablishJobs.insert(std::make_pair(
      remote.pubkey.data(),
      std::make_unique< TryConnectJob >(remote, link, numretries, router)));
  TryConnectJob *job = itr.first->second.get();
  // try establishing async
  router->logic->queue_job({job, &on_try_connecting});
  return true;
}

bool
llarp_findOrCreateIdentity(llarp::Crypto *crypto, const char *fpath,
                           byte_t *secretkey)
{
  llarp::LogDebug("find or create ", fpath);
  fs::path path(fpath);
  std::error_code ec;
  if(!fs::exists(path, ec))
  {
    llarp::LogInfo("generating new identity key");
    crypto->identity_keygen(secretkey);
    std::ofstream f(path.string(), std::ios::binary);
    if(f.is_open())
    {
      f.write((char *)secretkey, SECKEYSIZE);
    }
  }
  std::ifstream f(path.string(), std::ios::binary);
  if(f.is_open())
  {
    f.read((char *)secretkey, SECKEYSIZE);
    return true;
  }
  llarp::LogInfo("failed to get identity key");
  return false;
}

// C++ ...
bool
llarp_findOrCreateEncryption(llarp::Crypto *crypto, const char *fpath,
                             llarp::SecretKey &encryption)
{
  llarp::LogDebug("find or create ", fpath);
  fs::path path(fpath);
  std::error_code ec;
  if(!fs::exists(path, ec))
  {
    llarp::LogInfo("generating new encryption key");
    crypto->encryption_keygen(encryption);
    std::ofstream f(path.string(), std::ios::binary);
    if(f.is_open())
    {
      f.write((char *)encryption.data(), SECKEYSIZE);
    }
  }

  std::ifstream f(path.string(), std::ios::binary);
  if(f.is_open())
  {
    f.read((char *)encryption.data(), SECKEYSIZE);
    return true;
  }
  llarp::LogInfo("failed to get encryption key");
  return false;
}

namespace llarp
{
  void
  Router::OnSessionEstablished(llarp::RouterContact rc)
  {
    async_verify_RC(rc, nullptr);
    llarp::LogInfo("session with ", rc.pubkey, "established");
  }

  Router::Router(struct llarp_threadpool *_tp, struct llarp_ev_loop *_netloop,
                 llarp::Logic *_logic)
      : ready(false)
      , netloop(_netloop)
      , tp(_tp)
      , logic(_logic)
      , crypto(llarp::Crypto::sodium{})
      , paths(this)
      , exitContext(this)
      , dht(llarp_dht_context_new(this))
      , inbound_link_msg_parser(this)
      , hiddenServiceContext(this)
  {
    // set rational defaults
    this->ip4addr.sin_family = AF_INET;
    this->ip4addr.sin_port   = htons(1090);

#ifdef TESTNET
    disk = tp;
#else
    disk = llarp_init_threadpool(1, "llarp-diskio");
#endif
    _stopping.store(false);
    _running.store(false);
  }

  Router::~Router()
  {
    llarp_dht_context_free(dht);
  }

  bool
  Router::HandleRecvLinkMessageBuffer(llarp::ILinkSession *session,
                                      llarp_buffer_t buf)
  {
    if(_stopping)
      return true;

    if(!session)
    {
      llarp::LogWarn("no link session");
      return false;
    }
    return inbound_link_msg_parser.ProcessFrom(session, buf);
  }

  void
  Router::PersistSessionUntil(const llarp::RouterID &remote, llarp_time_t until)
  {
    llarp::LogDebug("persist session to ", remote, " until ", until);
    m_PersistingSessions[remote] =
        std::max(until, m_PersistingSessions[remote]);
  }

  bool
  Router::GetRandomGoodRouter(RouterID &router)
  {
    auto sz = nodedb->entries.size();
    if(sz == 0)
      return false;
    auto itr = nodedb->entries.begin();
    if(sz > 1)
      std::advance(itr, randint() % sz);
    router = itr->first;
    return true;
  }

  constexpr size_t MaxPendingSendQueueSize = 8;

  bool
  Router::SendToOrQueue(const llarp::RouterID &remote,
                        const llarp::ILinkMessage *msg)
  {
    for(const auto &link : inboundLinks)
    {
      if(link->HasSessionTo(remote.data()))
      {
        SendTo(remote, msg, link.get());
        return true;
      }
    }
    if(outboundLink && outboundLink->HasSessionTo(remote.data()))
    {
      SendTo(remote, msg, outboundLink.get());
      return true;
    }

    // no link available

    // this will create an entry in the obmq if it's not already there
    auto itr = outboundMessageQueue.find(remote);
    if(itr == outboundMessageQueue.end())
    {
      outboundMessageQueue.insert(std::make_pair(remote, MessageQueue()));
    }
    // encode
    llarp_buffer_t buf =
        llarp::StackBuffer< decltype(linkmsg_buffer) >(linkmsg_buffer);
    if(!msg->BEncode(&buf))
      return false;
    // queue buffer
    auto &q = outboundMessageQueue[remote];

    if(q.size() < MaxPendingSendQueueSize)
    {
      buf.sz = buf.cur - buf.base;
      q.emplace(buf.sz);
      memcpy(q.back().data(), buf.base, buf.sz);
    }
    else
    {
      llarp::LogWarn("tried to queue a message to ", remote,
                     " but the queue is full so we drop it like it's hawt");
    }
    llarp::RouterContact remoteRC;
    // we don't have an open session to that router right now
    if(nodedb->Get(remote, remoteRC))
    {
      // try connecting directly as the rc is loaded from disk
      llarp_router_try_connect(this, remoteRC, 10);
      return true;
    }

    // we don't have the RC locally so do a dht lookup
    dht->impl.LookupRouter(remote,
                           std::bind(&Router::HandleDHTLookupForSendTo, this,
                                     remote, std::placeholders::_1));
    return true;
  }

  void
  Router::HandleDHTLookupForSendTo(
      llarp::RouterID remote,
      const std::vector< llarp::RouterContact > &results)
  {
    if(results.size())
    {
      if(whitelistRouters
         && lokinetRouters.find(remote) == lokinetRouters.end())
      {
        return;
      }
      if(results[0].Verify(&crypto, Now()))
      {
        nodedb->Insert(results[0]);
        llarp_router_try_connect(this, results[0], 10);
        return;
      }
    }
    DiscardOutboundFor(remote);
  }

  void
  Router::ForEachPeer(
      std::function< void(const llarp::ILinkSession *, bool) > visit) const
  {
    outboundLink->ForEachSession(
        [visit](const llarp::ILinkSession *peer) { visit(peer, true); });
    for(const auto &link : inboundLinks)
    {
      link->ForEachSession(
          [visit](const llarp::ILinkSession *peer) { visit(peer, false); });
    }
  }

  void
  Router::ForEachPeer(std::function< void(llarp::ILinkSession *) > visit)
  {
    outboundLink->ForEachSession(
        [visit](llarp::ILinkSession *peer) { visit(peer); });
    for(const auto &link : inboundLinks)
    {
      link->ForEachSession([visit](llarp::ILinkSession *peer) { visit(peer); });
    }
  }

  void
  Router::try_connect(fs::path rcfile)
  {
    llarp::RouterContact remote;
    if(!remote.Read(rcfile.string().c_str()))
    {
      llarp::LogError("failure to decode or verify of remote RC");
      return;
    }
    if(remote.Verify(&crypto, Now()))
    {
      llarp::LogDebug("verified signature");
      // store into filesystem
      if(!nodedb->Insert(remote))
      {
        llarp::LogWarn("failed to store");
      }
      if(!llarp_router_try_connect(this, remote, 10))
      {
        // or error?
        llarp::LogWarn("session already made");
      }
    }
    else
      llarp::LogError(rcfile, " contains invalid RC");
  }

  bool
  Router::EnsureIdentity()
  {
    if(!EnsureEncryptionKey())
      return false;
    return llarp_findOrCreateIdentity(&crypto, ident_keyfile.string().c_str(),
                                      identity);
  }

  bool
  Router::EnsureEncryptionKey()
  {
    return llarp_findOrCreateEncryption(
        &crypto, encryption_keyfile.string().c_str(), encryption);
  }

  void
  Router::AddInboundLink(std::unique_ptr< llarp::ILinkLayer > &link)
  {
    inboundLinks.push_back(std::move(link));
  }

  bool
  Router::Configure(struct llarp_config *conf)
  {
    llarp_config_iterator iter;
    iter.user  = this;
    iter.visit = llarp::router_iter_config;
    llarp_config_iter(conf, &iter);
    if(!InitOutboundLink())
      return false;
    if(!Ready())
    {
      return false;
    }
    return EnsureIdentity();
  }

  bool
  Router::Ready()
  {
    return outboundLink != nullptr;
  }

  bool
  Router::SaveRC()
  {
    llarp::LogDebug("verify RC signature");
    if(!rc().Verify(&crypto, Now()))
    {
      rc().Dump< MAX_RC_SIZE >();
      llarp::LogError("RC is invalid, not saving");
      return false;
    }
    return rc().Write(our_rc_file.string().c_str());
  }

  bool
  Router::IsServiceNode() const
  {
    return inboundLinks.size() > 0;
  }

  void
  Router::Close()
  {
    llarp::LogInfo("closing router");
    llarp_ev_loop_stop(netloop);
    inboundLinks.clear();
    outboundLink.reset(nullptr);
  }

  void
  Router::on_verify_client_rc(llarp_async_verify_rc *job)
  {
    llarp::async_verify_context *ctx =
        static_cast< llarp::async_verify_context * >(job->user);
    ctx->router->pendingEstablishJobs.erase(job->rc.pubkey);
    auto router = ctx->router;
    llarp::PubKey pk(job->rc.pubkey);
    router->FlushOutboundFor(pk, router->GetLinkWithSessionByPubkey(pk));
    delete ctx;
    router->pendingVerifyRC.erase(pk);
  }

  void
  Router::on_verify_server_rc(llarp_async_verify_rc *job)
  {
    llarp::async_verify_context *ctx =
        static_cast< llarp::async_verify_context * >(job->user);
    auto router = ctx->router;
    llarp::PubKey pk(job->rc.pubkey);
    if(!job->valid)
    {
      if(ctx->establish_job)
      {
        // was an outbound attempt
        ctx->establish_job->Failed();
      }
      delete ctx;
      router->DiscardOutboundFor(pk);
      router->pendingVerifyRC.erase(pk);

      return;
    }
    // we're valid, which means it's already been committed to the nodedb

    llarp::LogDebug("rc verified and saved to nodedb");

    if(router->validRouters.count(pk))
    {
      router->validRouters.erase(pk);
    }

    llarp::RouterContact rc = job->rc;

    router->validRouters.insert(std::make_pair(pk.data(), rc));

    // track valid router in dht
    router->dht->impl.nodes->PutNode(rc);

    // mark success in profile
    router->routerProfiling.MarkSuccess(pk);

    // this was an outbound establish job
    if(ctx->establish_job)
    {
      ctx->establish_job->Success();
    }
    else
      router->FlushOutboundFor(pk, router->GetLinkWithSessionByPubkey(pk));
    delete ctx;
    router->pendingVerifyRC.erase(pk);
  }

  void
  Router::handle_router_ticker(void *user, uint64_t orig, uint64_t left)
  {
    if(left)
      return;
    Router *self        = static_cast< Router * >(user);
    self->ticker_job_id = 0;
    self->Tick();
    self->ScheduleTicker(orig);
  }

  bool
  Router::ParseRoutingMessageBuffer(llarp_buffer_t buf, routing::IMessageHandler * h, PathID_t rxid)
  {
    return inbound_routing_msg_parser.ParseMessageBuffer(buf, h, rxid, this);
  }

  bool
  Router::ConnectionToRouterAllowed(const llarp::RouterID &router) const
  {
    if(strictConnectPubkeys.size() && strictConnectPubkeys.count(router) == 0)
      return false;
    else if(IsServiceNode() && whitelistRouters)
      return lokinetRouters.count(router) != 0;
    else
      return true;
  }

  void
  Router::HandleDHTLookupForExplore(
      llarp::RouterID remote,
      const std::vector< llarp::RouterContact > &results)
  {
    if(results.size() == 0)
      return;
    for(const auto &rc : results)
    {
      if(rc.Verify(&crypto, Now()))
        nodedb->Insert(rc);
      else
        return;
    }
    if(ConnectionToRouterAllowed(remote))
    {
      TryEstablishTo(remote);
    }
  }

  void
  Router::TryEstablishTo(const llarp::RouterID &remote)
  {
    if(!ConnectionToRouterAllowed(remote))
    {
      llarp::LogWarn("not connecting to ", remote,
                     " as it's not permitted by config");
      return;
    }

    llarp::RouterContact rc;
    if(nodedb->Get(remote, rc))
    {
      // try connecting async
      llarp_router_try_connect(this, rc, 5);
    }
    else if(IsServiceNode() || !routerProfiling.IsBad(remote))
    {
      if(dht->impl.HasRouterLookup(remote))
        return;
      llarp::LogInfo("looking up router ", remote);
      // dht lookup as we don't know it
      dht->impl.LookupRouter(
          remote,
          std::bind(&Router::HandleDHTLookupForTryEstablishTo, this, remote,
                    std::placeholders::_1));
    }
    else
    {
      llarp::LogWarn("not connecting to ", remote, " as it's unreliable");
    }
  }

  void
  Router::OnConnectTimeout(ILinkSession *session)
  {
    auto itr = pendingEstablishJobs.find(session->GetPubKey());
    if(itr != pendingEstablishJobs.end())
    {
      itr->second->AttemptTimedout();
    }
  }

  void
  Router::HandleDHTLookupForTryEstablishTo(
      llarp::RouterID remote,
      const std::vector< llarp::RouterContact > &results)
  {
    if(results.size() == 0)
    {
      if(!IsServiceNode())
        routerProfiling.MarkTimeout(remote);
    }
    for(const auto &result : results)
    {
      if(whitelistRouters
         && lokinetRouters.find(result.pubkey) == lokinetRouters.end())
        continue;
      nodedb->Insert(result);
      llarp_router_try_connect(this, result, 10);
    }
  }

  size_t
  Router::NumberOfConnectedRouters() const
  {
    return validRouters.size();
  }

  bool
  Router::UpdateOurRC(bool rotateKeys)
  {
    llarp::SecretKey nextOnionKey;
    llarp::RouterContact nextRC = _rc;
    if(rotateKeys)
    {
      crypto.encryption_keygen(nextOnionKey);
      nextRC.enckey = llarp::seckey_topublic(nextOnionKey);
    }
    nextRC.last_updated = Now();
    if(!nextRC.Sign(&crypto, identity))
      return false;
    _rc = nextRC;
    if(rotateKeys)
    {
      encryption = nextOnionKey;
      // propagate RC by renegotiating sessions
      ForEachPeer([](llarp::ILinkSession *s) {
        if(s->RenegotiateSession())
          llarp::LogInfo("renegotiated session");
        else
          llarp::LogWarn("failed to renegotiate session");
      });
    }
    // TODO: do this async
    return SaveRC();
  }  // namespace llarp

  bool
  Router::CheckRenegotiateValid(RouterContact newrc, RouterContact oldrc)
  {
    // missmatch of identity ?
    if(newrc.pubkey != oldrc.pubkey)
      return false;

    // store it in nodedb async
    nodedb->InsertAsync(newrc);
    // update dht if required
    if(dht->impl.nodes->HasNode(newrc.pubkey.data()))
    {
      dht->impl.nodes->PutNode(newrc);
    }
    // update valid routers
    {
      auto itr = validRouters.find(newrc.pubkey);
      if(itr == validRouters.end())
        validRouters[newrc.pubkey] = newrc;
      else
        itr->second = newrc;
    }
    // TODO: check for other places that need updating the RC
    return true;
  }

  void
  Router::ServiceNodeLookupRouterWhenExpired(RouterID router)
  {
    dht->impl.LookupRouter(router,
                           std::bind(&Router::HandleDHTLookupForExplore, this,
                                     router, std::placeholders::_1));
  }

  void
  Router::Tick()
  {
    // llarp::LogDebug("tick router");
    auto now = llarp_ev_loop_time_now_ms(netloop);

    if(_rc.ExpiresSoon(now, llarp::randint() % 10000))
    {
      llarp::LogInfo("regenerating RC");
      if(!UpdateOurRC(IsServiceNode()))
        llarp::LogError("Failed to update our RC");
    }

    if(IsServiceNode())
    {
      // only do this as service node
      // client endpoints do this on their own
      nodedb->visit([&](const RouterContact &rc) -> bool {
        if(rc.ExpiresSoon(now, llarp::randint() % 10000))
          ServiceNodeLookupRouterWhenExpired(rc.pubkey);
        return true;
      });
    }
    paths.TickPaths(now);
    paths.ExpirePaths(now);
    {
      auto itr = m_PersistingSessions.begin();
      while(itr != m_PersistingSessions.end())
      {
        auto link = GetLinkWithSessionByPubkey(itr->first);
        if(now < itr->second)
        {
          if(link)
          {
            llarp::LogDebug("keepalive to ", itr->first);
            link->KeepAliveSessionTo(itr->first);
          }
          else
          {
            llarp::LogDebug("establish to ", itr->first);
            TryEstablishTo(itr->first);
          }
          ++itr;
        }
        else
        {
          llarp::LogInfo("commit to ", itr->first, " expired");
          itr = m_PersistingSessions.erase(itr);
        }
      }
    }

    size_t N = nodedb->num_loaded();
    if(N < minRequiredRouters)
    {
      llarp::LogInfo("We need at least ", minRequiredRouters,
                     " service nodes to build paths but we have ", N,
                     " in nodedb");
      // TODO: only connect to random subset
      if(bootstrapRCList.size())
      {
        for(const auto &rc : bootstrapRCList)
        {
          llarp_router_try_connect(this, rc, 4);
          dht->impl.ExploreNetworkVia(rc.pubkey.data());
        }
      }
      else
        llarp::LogError("we have no bootstrap nodes specified");
    }

    if(inboundLinks.size() == 0)
    {
      paths.BuildPaths(now);
      hiddenServiceContext.Tick(now);
    }
    if(NumberOfConnectedRouters() < minConnectedRouters)
    {
      ConnectToRandomRouters(minConnectedRouters);
    }
    exitContext.Tick(now);
    if(rpcCaller)
      rpcCaller->Tick(now);
  }

  bool
  Router::Sign(llarp::Signature &sig, llarp_buffer_t buf) const
  {
    return crypto.sign(sig, identity, buf);
  }

  void
  Router::SendTo(llarp::RouterID remote, const llarp::ILinkMessage *msg,
                 llarp::ILinkLayer *selected)
  {
    llarp_buffer_t buf =
        llarp::StackBuffer< decltype(linkmsg_buffer) >(linkmsg_buffer);

    if(!msg->BEncode(&buf))
    {
      llarp::LogWarn("failed to encode outbound message, buffer size left: ",
                     llarp_buffer_size_left(buf));
      return;
    }
    // set size of message
    buf.sz  = buf.cur - buf.base;
    buf.cur = buf.base;
    llarp::LogDebug("send ", buf.sz, " bytes to ", remote);
    if(selected)
    {
      if(selected->SendTo(remote, buf))
        return;
    }
    bool sent = outboundLink->SendTo(remote, buf);
    if(!sent)
    {
      for(const auto &link : inboundLinks)
      {
        if(!sent)
        {
          sent = link->SendTo(remote, buf);
        }
      }
    }
    if(!sent)
      llarp::LogWarn("message to ", remote, " was dropped");
  }

  void
  Router::ScheduleTicker(uint64_t ms)
  {
    ticker_job_id = logic->call_later({ms, this, &handle_router_ticker});
  }

  void
  Router::SessionClosed(llarp::RouterID remote)
  {
    __llarp_dht_remove_peer(dht, remote);
    // remove from valid routers if it's a valid router
    validRouters.erase(remote);
    llarp::LogInfo("Session to ", remote, " fully closed");
  }

  llarp::ILinkLayer *
  Router::GetLinkWithSessionByPubkey(const llarp::RouterID &pubkey)
  {
    if(outboundLink && outboundLink->HasSessionTo(pubkey))
      return outboundLink.get();
    for(const auto &link : inboundLinks)
    {
      if(link->HasSessionTo(pubkey))
        return link.get();
    }
    return nullptr;
  }

  void
  Router::FlushOutboundFor(llarp::RouterID remote, llarp::ILinkLayer *chosen)
  {
    llarp::LogDebug("Flush outbound for ", remote);

    auto itr = outboundMessageQueue.find(remote);
    if(itr == outboundMessageQueue.end())
    {
      pendingEstablishJobs.erase(remote);
      return;
    }
    if(!chosen)
    {
      DiscardOutboundFor(remote);
      pendingEstablishJobs.erase(remote);
      return;
    }
    while(itr->second.size())
    {
      auto buf = llarp::ConstBuffer(itr->second.front());
      if(!chosen->SendTo(remote, buf))
        llarp::LogWarn("failed to send outboud message to ", remote, " via ",
                       chosen->Name());

      itr->second.pop();
    }
    pendingEstablishJobs.erase(remote);
  }

  void
  Router::DiscardOutboundFor(const llarp::RouterID &remote)
  {
    outboundMessageQueue.erase(remote);
  }

  bool
  Router::GetRandomConnectedRouter(llarp::RouterContact &result) const
  {
    auto sz = validRouters.size();
    if(sz)
    {
      auto itr = validRouters.begin();
      if(sz > 1)
        std::advance(itr, llarp::randint() % sz);
      result = itr->second;
      return true;
    }
    return false;
  }

  void
  Router::async_verify_RC(const llarp::RouterContact &rc,
                          llarp::ILinkLayer *link)
  {
    if(pendingVerifyRC.count(rc.pubkey))
      return;
    if(rc.IsPublicRouter() && whitelistRouters)
    {
      if(lokinetRouters.find(rc.pubkey) == lokinetRouters.end())
      {
        llarp::LogInfo(rc.pubkey, " is NOT a valid service node, rejecting");
        link->CloseSessionTo(rc.pubkey);
        return;
      }
    }
    llarp_async_verify_rc *job       = &pendingVerifyRC[rc.pubkey];
    llarp::async_verify_context *ctx = new llarp::async_verify_context();
    ctx->router                      = this;
    ctx->establish_job               = nullptr;

    auto itr = pendingEstablishJobs.find(rc.pubkey);
    if(itr != pendingEstablishJobs.end())
      ctx->establish_job = itr->second.get();

    job->user  = ctx;
    job->rc    = rc;
    job->valid = false;
    job->hook  = nullptr;

    job->nodedb = nodedb;
    job->logic  = logic;
    // job->crypto = &crypto; // we already have this
    job->cryptoworker = tp;
    job->diskworker   = disk;
    if(rc.IsPublicRouter())
      job->hook = &Router::on_verify_server_rc;
    else
      job->hook = &Router::on_verify_client_rc;

    llarp_nodedb_async_verify(job);
  }

  bool
  Router::Run(struct llarp_nodedb *nodedb)
  {
    if(_running || _stopping)
      return false;
    this->nodedb = nodedb;

    if(enableRPCServer)
    {
      if(rpcBindAddr.empty())
      {
        rpcBindAddr = DefaultRPCBindAddr;
      }
      rpcServer = std::make_unique< llarp::rpc::Server >(this);
      while(!rpcServer->Start(rpcBindAddr))
      {
        llarp::LogError("failed to bind jsonrpc to ", rpcBindAddr);
#if defined(ANDROID) || defined(RPI)
        sleep(1);
#else
        std::this_thread::sleep_for(std::chrono::seconds(1));
#endif
      }
      llarp::LogInfo("Bound RPC server to ", rpcBindAddr);
    }

    llarp_threadpool_start(tp);
    llarp_threadpool_start(disk);

    routerProfiling.Load(routerProfilesFile.c_str());

    llarp::Addr publicAddr(this->addrInfo);

    if(this->publicOverride)
    {
      llarp::LogDebug("public address:port ", publicAddr);
    }

    llarp::LogInfo("You have ", inboundLinks.size(), " inbound links");
    for(const auto &link : inboundLinks)
    {
      llarp::AddressInfo addr;
      if(!link->GetOurAddressInfo(addr))
        continue;
      llarp::Addr a(addr);
      if(this->publicOverride && a.sameAddr(publicAddr))
      {
        llarp::LogInfo("Found adapter for public address");
      }
      if(!llarp::IsBogon(*a.addr6()))
      {
        llarp::LogInfo("Loading Addr: ", a, " into our RC");
        _rc.addrs.push_back(addr);
      }
    };
    if(this->publicOverride)
    {
      llarp::ILinkLayer *link = nullptr;
      // llarp::LogWarn("Need to load our public IP into RC!");
      if(inboundLinks.size() == 1)
      {
        link = inboundLinks[0].get();
      }
      else
      {
        if(inboundLinks.size())
        {
          link = inboundLinks[0].get();
        }
        else
        {
          llarp::LogWarn(
              "No need to set public ipv4 and port if no external interface "
              "binds, turning off public override");
          this->publicOverride = false;
          link                 = nullptr;
        }
      }
      if(link && link->GetOurAddressInfo(this->addrInfo))
      {
        // override ip and port
        this->addrInfo.ip   = *publicAddr.addr6();
        this->addrInfo.port = publicAddr.port();
        llarp::LogInfo("Loaded our public ", publicAddr, " override into RC!");
        _rc.addrs.push_back(this->addrInfo);
      }
    }

    // set public encryption key
    _rc.enckey = llarp::seckey_topublic(encryption);
    // set public signing key
    _rc.pubkey = llarp::seckey_topublic(identity);
    if(ExitEnabled())
    {
      llarp::nuint32_t a = publicAddr.xtonl();
      // TODO: enable this once the network can serialize xi
      //_rc.exits.emplace_back(_rc.pubkey, a);
      llarp::LogInfo(
          "Neato tehl33toh, You are a freaking exit relay. w00t!!!!! your "
          "exit "
          "is advertised as exiting at ",
          a);
    }
    llarp::LogInfo("Signing rc...");
    if(!_rc.Sign(&crypto, identity))
    {
      llarp::LogError("failed to sign rc");
      return false;
    }

    if(!SaveRC())
    {
      llarp::LogError("failed to save RC");
      return false;
    }

    llarp::LogInfo("have ", nodedb->num_loaded(), " routers");

    llarp::LogDebug("starting outbound link");
    if(!outboundLink->Start(logic))
    {
      llarp::LogWarn("outbound link failed to start");
      return false;
    }

    int IBLinksStarted = 0;

    // start links
    for(const auto &link : inboundLinks)
    {
      if(link->Start(logic))
      {
        llarp::LogDebug("Link ", link->Name(), " started");
        IBLinksStarted++;
      }
      else
        llarp::LogWarn("Link ", link->Name(), " failed to start");
    }

    if(IBLinksStarted > 0)
    {
      // initialize as service node
      if(!InitServiceNode())
      {
        llarp::LogError("Failed to initialize service node");
        return false;
      }
      llarp::RouterID us = pubkey();
      llarp::LogInfo("initalized service node: ", us);
    }
    else
    {
      // we are a client
      // regenerate keys and resign rc before everything else
      crypto.identity_keygen(identity);
      crypto.encryption_keygen(encryption);
      _rc.pubkey = llarp::seckey_topublic(identity);
      _rc.enckey = llarp::seckey_topublic(encryption);
      if(!_rc.Sign(&crypto, identity))
      {
        llarp::LogError("failed to regenerate keys and sign RC");
        return false;
      }
      // generate default hidden service
      llarp::LogInfo("setting up default network endpoint");
      if(!CreateDefaultHiddenService())
      {
        llarp::LogError("failed to set up default network endpoint");
        return false;
      }
    }

    llarp::LogInfo("starting hidden service context...");
    if(!hiddenServiceContext.StartAll())
    {
      llarp::LogError("Failed to start hidden service context");
      return false;
    }
    llarp_dht_context_start(dht, pubkey());
    ScheduleTicker(1000);
    _running.store(true);
    return _running;
  }

  static void
  RouterAfterStopLinks(void *u, uint64_t, uint64_t)
  {
    Router *self = static_cast< Router * >(u);
    self->Close();
  }

  static void
  RouterAfterStopIssued(void *u, uint64_t, uint64_t)
  {
    Router *self = static_cast< Router * >(u);
    self->StopLinks();
    self->logic->call_later({200, self, &RouterAfterStopLinks});
  }

  void
  Router::StopLinks()
  {
    llarp::LogInfo("stopping links");
    outboundLink->Stop();
    for(auto &link : inboundLinks)
      link->Stop();
  }

  void
  Router::Stop()
  {
    if(!_running)
      return;
    if(_stopping)
      return;

    _stopping.store(true);
    llarp::LogInfo("stopping router");
    hiddenServiceContext.StopAll();
    exitContext.Stop();
    if(rpcServer)
      rpcServer->Stop();
    logic->call_later({200, this, &RouterAfterStopIssued});
  }

  bool
  Router::HasSessionTo(const llarp::RouterID &remote) const
  {
    return validRouters.find(remote) != validRouters.end();
  }

  void
  Router::ConnectToRandomRouters(int want)
  {
    int wanted   = want;
    Router *self = this;

    self->nodedb->visit(
        [self, &want](const llarp::RouterContact &other) -> bool {
          // check if we really want to
          if(other.ExpiresSoon(self->Now(), 30000))
            return want > 0;
          if(!self->ConnectionToRouterAllowed(other.pubkey))
            return want > 0;
          if(llarp::randint() % 2 == 0
             && !(self->HasSessionTo(other.pubkey)
                  || self->HasPendingConnectJob(other.pubkey)))
          {
            llarp_router_try_connect(self, other, 5);
            --want;
          }
          return want > 0;
        });
    if(wanted != want)
      llarp::LogInfo("connecting to ", abs(want - wanted), " out of ", wanted,
                     " random routers");
  }

  bool
  Router::InitServiceNode()
  {
    llarp::LogInfo("accepting transit traffic");
    paths.AllowTransit();
    llarp_dht_allow_transit(dht);
    return exitContext.AddExitEndpoint("default-connectivity", netConfig);
  }

  /// validate a new configuration against an already made and running
  /// router
  struct RouterConfigValidator
  {
    static void
    ValidateEntry(llarp_config_iterator *i, const char *section,
                  const char *key, const char *val)
    {
      RouterConfigValidator *self =
          static_cast< RouterConfigValidator * >(i->user);
      if(self->valid)
      {
        if(!self->OnEntry(section, key, val))
        {
          llarp::LogError("invalid entry in section [", section, "]: '", key,
                          "'='", val, "'");
          self->valid = false;
        }
      }
    }

    const Router *router;
    llarp_config *config;
    bool valid;
    RouterConfigValidator(const Router *r, llarp_config *conf)
        : router(r), config(conf), valid(true)
    {
    }

    /// checks the (section, key, value) config tuple
    /// return false if that entry conflicts
    /// with existing configuration in router
    bool
    OnEntry(const char *, const char *, const char *) const
    {
      // TODO: implement me
      return true;
    }

    /// do validation
    /// return true if this config is valid
    /// return false if this config is not valid
    bool
    Validate()
    {
      llarp_config_iterator iter;
      iter.user  = this;
      iter.visit = &ValidateEntry;
      llarp_config_iter(config, &iter);
      return valid;
    }
  };

  bool
  Router::ValidateConfig(llarp_config *conf) const
  {
    RouterConfigValidator validator(this, conf);
    return validator.Validate();
  }

  bool
  Router::Reconfigure(llarp_config *)
  {
    // TODO: implement me
    return true;
  }

  bool
  Router::InitOutboundLink()
  {
    if(outboundLink)
      return true;

    auto link = llarp::utp::NewServerFromRouter(this);

    if(!link->EnsureKeys(transport_keyfile.string().c_str()))
    {
      llarp::LogError("failed to load ", transport_keyfile);
      return false;
    }

    auto afs = {AF_INET, AF_INET6};

    for(auto af : afs)
    {
      if(link->Configure(netloop, "*", af, 0))
      {
        outboundLink = std::move(link);
        llarp::LogInfo("outbound link ready");
        return true;
      }
    }
    return false;
  }

  bool
  Router::CreateDefaultHiddenService()
  {
    // fallback defaults
    static const std::unordered_map< std::string,
                                     std::function< std::string(void) > >
        netConfigDefaults = {
            {"ifname", llarp::findFreeLokiTunIfName},
            {"ifaddr", llarp::findFreePrivateRange},
            {"local-dns", []() -> std::string { return "127.0.0.1:53"; }},
            {"upstream-dns", []() -> std::string { return "1.1.1.1:53"; }}};
    // populate with fallback defaults if values not present
    auto itr = netConfigDefaults.begin();
    while(itr != netConfigDefaults.end())
    {
      auto found = netConfig.find(itr->first);
      if(found == netConfig.end() || found->second.empty())
      {
        netConfig.emplace(std::make_pair(itr->first, itr->second()));
      }
      ++itr;
    }
    // add endpoint
    return hiddenServiceContext.AddDefaultEndpoint(netConfig);
  }

  bool
  Router::HasPendingConnectJob(const llarp::RouterID &remote)
  {
    return pendingEstablishJobs.find(remote) != pendingEstablishJobs.end();
  }

  bool
  Router::LoadHiddenServiceConfig(const char *fname)
  {
    llarp::LogDebug("opening hidden service config ", fname);
    llarp::service::Config conf;
    if(!conf.Load(fname))
      return false;
    for(const auto &config : conf.services)
    {
      llarp::service::Config::section_t filteredConfig;
      mergeHiddenServiceConfig(config.second, filteredConfig.second);
      filteredConfig.first = config.first;
      if(!hiddenServiceContext.AddEndpoint(filteredConfig))
        return false;
    }
    return true;
  }

  void
  router_iter_config(llarp_config_iterator *iter, const char *section,
                     const char *key, const char *val)
  {
    llarp::LogDebug(section, " ", key, "=", val);
    Router *self = static_cast< Router * >(iter->user);

    int af;
    uint16_t proto;
    if(StrEq(val, "eth"))
    {
#ifdef AF_LINK
      af = AF_LINK;
#endif
#ifdef AF_PACKET
      af = AF_PACKET;
#endif
      proto = LLARP_ETH_PROTO;
    }
    else
    {
      // try IPv4 first
      af    = AF_INET;
      proto = std::atoi(val);
    }

    if(StrEq(section, "bind"))
    {
      if(!StrEq(key, "*"))
      {
        auto server = llarp::utp::NewServerFromRouter(self);
        if(!server->EnsureKeys(self->transport_keyfile.string().c_str()))
        {
          llarp::LogError("failed to ensure keyfile ", self->transport_keyfile);
          return;
        }
        if(server->Configure(self->netloop, key, af, proto))
        {
          self->AddInboundLink(server);
          return;
        }
        if(af == AF_INET6)
        {
          // we failed to configure IPv6
          // try IPv4
          llarp::LogInfo("link ", key,
                         " failed to configure IPv6, trying IPv4");
          af = AF_INET;
          if(server->Configure(self->netloop, key, af, proto))
          {
            self->AddInboundLink(server);
            return;
          }
        }
        llarp::LogError("Failed to set up curvecp link");
      }
    }
    else if(StrEq(section, "network"))
    {
      if(StrEq(key, "profiles"))
      {
        self->routerProfilesFile = val;
        self->routerProfiling.Load(val);
        llarp::LogInfo("setting profiles to ", self->routerProfilesFile);
      }
      else if(StrEq(key, "strict-connect"))
      {
        if(self->IsServiceNode())
        {
          llarp::LogError("cannot use strict-connect option as service node");
          return;
        }
        llarp::RouterID snode;
        llarp::PubKey pk;
        if(pk.FromString(val))
        {
          if(self->strictConnectPubkeys.insert(pk.data()).second)
            llarp::LogInfo("added ", pk, " to strict connect list");
          else
            llarp::LogWarn("duplicate key for strict connect: ", pk);
        }
        else if(snode.FromString(val))
        {
          if(self->strictConnectPubkeys.insert(snode).second)
            llarp::LogInfo("added ", snode, " to strict connect list");
          else
            llarp::LogWarn("duplicate key for strict connect: ", snode);
        }
        else
          llarp::LogError("invalid key for strict-connect: ", val);
      }
      else
      {
        self->netConfig.insert(std::make_pair(key, val));
      }
    }
    else if(StrEq(section, "api"))
    {
      if(StrEq(key, "enabled"))
      {
        self->enableRPCServer = IsTrueValue(val);
      }
      if(StrEq(key, "bind"))
      {
        self->rpcBindAddr = val;
      }
      if(StrEq(key, "authkey"))
      {
        // TODO: add pubkey to whitelist
      }
    }
    else if(StrEq(section, "services"))
    {
      if(self->LoadHiddenServiceConfig(val))
      {
        llarp::LogInfo("loaded hidden service config for ", key);
      }
      else
      {
        llarp::LogWarn("failed to load hidden service config for ", key);
      }
    }
    else if(StrEq(section, "lokid"))
    {
      if(StrEq(key, "enabled"))
      {
        self->whitelistRouters = IsTrueValue(val);
      }
      if(StrEq(key, "jsonrpc"))
      {
        self->lokidRPCAddr = val;
      }
    }
    else if(StrEq(section, "dns"))
    {
      if(StrEq(key, "upstream"))
      {
        llarp::LogInfo("add upstream resolver ", val);
        self->netConfig.emplace(std::make_pair("upstream-dns", val));
      }
      if(StrEq(key, "bind"))
      {
        llarp::LogInfo("set local dns to ", val);
        self->netConfig.emplace(std::make_pair("local-dns", val));
      }
    }
    else if(StrEq(section, "connect")
            || (StrEq(section, "bootstrap") && StrEq(key, "add-node")))
    {
      self->bootstrapRCList.emplace_back();
      auto &rc = self->bootstrapRCList.back();
      if(rc.Read(val) && rc.Verify(&self->crypto, self->Now()))
      {
        llarp::LogInfo("Added bootstrap node ", RouterID(rc.pubkey.data()));
      }
      else if(self->Now() - rc.last_updated > RouterContact::Lifetime)
      {
        llarp::LogWarn("Bootstrap node ", RouterID(rc.pubkey.data()),
                       " is too old and needs to be refreshed");
        self->bootstrapRCList.pop_back();
      }
      else
      {
        llarp::LogError("malformed rc file: ", val);
        self->bootstrapRCList.pop_back();
      }
    }
    else if(StrEq(section, "router"))
    {
      if(StrEq(key, "netid"))
      {
        if(strlen(val) <= self->_rc.netID.size())
        {
          llarp::LogWarn("!!!! you have manually set netid to be '", val,
                         "' which does not equal '", LLARP_NET_ID,
                         "' you will run as a different network, good luck and "
                         "don't forget: something something MUH traffic shape "
                         "correlation !!!!");
          llarp::NetID::DefaultValue = (const byte_t *)strdup(val);
        }
        else
          llarp::LogError("invalid netid '", val, "', is too long");
      }
      if(StrEq(key, "nickname"))
      {
        self->_rc.SetNick(val);
        // set logger name here
        _glog.nodeName = self->rc().Nick();
      }
      if(StrEq(key, "encryption-privkey"))
      {
        self->encryption_keyfile = val;
      }
      if(StrEq(key, "contact-file"))
      {
        self->our_rc_file = val;
      }
      if(StrEq(key, "transport-privkey"))
      {
        self->transport_keyfile = val;
      }
      if(StrEq(key, "ident-privkey"))
      {
        self->ident_keyfile = val;
      }
      if(StrEq(key, "public-address"))
      {
        llarp::LogInfo("public ip ", val, " size ", strlen(val));
        if(strlen(val) < 17)
        {
          // assume IPv4
          // inet_pton(AF_INET, val, &self->ip4addr.sin_addr);
          // struct sockaddr dest;
          // sockaddr *dest = (sockaddr *)&self->ip4addr;
          llarp::Addr a(val);
          llarp::LogInfo("setting public ipv4 ", a);
          self->addrInfo.ip    = *a.addr6();
          self->publicOverride = true;
        }
        // llarp::Addr a(val);
      }
      if(StrEq(key, "public-port"))
      {
        llarp::LogInfo("Setting public port ", val);
        int p = atoi(val);
        // Not needed to flip upside-down - this is done in llarp::Addr(const
        // AddressInfo&)
        self->ip4addr.sin_port = p;
        self->addrInfo.port    = p;
        self->publicOverride   = true;
      }
    }
  }  // namespace llarp
}  // namespace llarp
