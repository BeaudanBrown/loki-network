#ifndef LLARP_LINK_SERVER_HPP
#define LLARP_LINK_SERVER_HPP

#include <crypto.hpp>
#include <ev.h>
#include <link/session.hpp>
#include <logic.hpp>
#include <net.hpp>
#include <router_contact.hpp>
#include <threading.hpp>

#include <list>
#include <unordered_map>

namespace llarp
{
  struct Router;

  struct ILinkLayer
  {
    virtual ~ILinkLayer();
    /// get current time via event loop
    llarp_time_t
    now() const
    {
      return llarp_ev_loop_time_now_ms(m_Loop);
    }
    bool
    HasSessionTo(const byte_t* pk);

    bool
    HasSessionVia(const Addr& addr);

    void
    ForEachSession(std::function< void(const ILinkSession*) > visit) const;

    static void
    udp_tick(llarp_udp_io* udp)
    {
      static_cast< ILinkLayer* >(udp->user)->Pump();
    }

    static void
    udp_recv_from(llarp_udp_io* udp, const sockaddr* from, llarp_buffer_t buf)
    {
      if(!udp)
      {
        llarp::LogWarn("no udp set");
        return;
      }
      // maybe chekc from too?
      // no it's never null
      static_cast< ILinkLayer* >(udp->user)->RecvFrom(*from, buf.base, buf.sz);
    }

    bool
    Configure(llarp_ev_loop* loop, const std::string& ifname, int af,
              uint16_t port);

    virtual ILinkSession*
    NewOutboundSession(const RouterContact& rc, const AddressInfo& ai) = 0;

    virtual void
    Pump();

    virtual void
    RecvFrom(const Addr& from, const void* buf, size_t sz) = 0;

    bool
    PickAddress(const RouterContact& rc, AddressInfo& picked) const;

    bool
    TryEstablishTo(const RouterContact& rc);

    bool
    Start(llarp::Logic* l);

    void
    Stop();

    virtual const char*
    Name() const = 0;

    void
    CloseSessionTo(const byte_t* remote);

    void
    KeepAliveSessionTo(const byte_t* remote);

    bool
    SendTo(const byte_t* remote, llarp_buffer_t buf);

    bool
    GetOurAddressInfo(AddressInfo& addr) const;

    virtual uint16_t
    Rank() const = 0;

    virtual bool
    KeyGen(SecretKey&) = 0;

    const byte_t*
    TransportPubKey() const;

    const byte_t*
    TransportSecretKey() const;

    bool
    EnsureKeys(const char* fpath);

    void
    MapAddr(const byte_t* pk, ILinkSession* s);

    virtual void
    Tick(__attribute__((unused)) llarp_time_t now)
    {
    }

   private:
    static void
    on_timer_tick(void* user, uint64_t orig, uint64_t left)
    {
      // timer cancelled
      if(left)
        return;
      static_cast< ILinkLayer* >(user)->OnTick(orig);
    }

    void
    OnTick(uint64_t interval);

    void
    ScheduleTick(uint64_t interval);

    uint32_t tick_id;

   protected:
    using Lock  = util::NullLock;
    using Mutex = util::NullMutex;

    void
    PutSession(ILinkSession* s);

    llarp::Logic* m_Logic = nullptr;
    llarp_ev_loop* m_Loop = nullptr;
    Addr m_ourAddr;
    llarp_udp_io m_udp;
    SecretKey m_SecretKey;

    Mutex m_AuthedLinksMutex;
    std::unordered_multimap< RouterID, std::unique_ptr< ILinkSession >,
                             RouterID::Hash >
        m_AuthedLinks;
    Mutex m_PendingMutex;
    std::list< std::unique_ptr< ILinkSession > > m_Pending;
  };
}  // namespace llarp

#endif
