#include <unistd.h>
#include <algorithm>
#include <chrono>
#include "fly/base/logger.hpp"
#include "fly/net/server.hpp"
#include "fly/net/client.hpp"
#include "net/p2p/node.hpp"
#include "message.hpp"
#include "version.hpp"
#include "blockchain.hpp"
#include "utilstrencodings.h"

using namespace std::placeholders;

namespace net {
namespace p2p {

Node::Node()
{
}

Node::~Node()
{
}

bool Node::start(uint16 port)
{
    int32 cpu_num = sysconf(_SC_NPROCESSORS_ONLN);
    cpu_num = cpu_num < 4 ? 4 : cpu_num;
    m_poller.reset(new fly::net::Poller<Json>(cpu_num));
    std::unique_ptr<fly::net::Server<Json>> server(new fly::net::Server<Json>(fly::net::Addr("0.0.0.0", port),
                                                                              std::bind(&Node::allow, this, _1),
                                                                              std::bind(&Node::init, this, _1),
                                                                              std::bind(&Node::dispatch, this, _1),
                                                                              std::bind(&Node::close, this, _1),
                                                                              std::bind(&Node::be_closed, this, _1),
                                                                              m_poller, 1024 * 1024)); // todo, max_msg_length?
    m_poller->start();
    m_port = port;
    
    if(server->start())
    {
        CONSOLE_LOG_INFO("start p2p node success");
        m_server = std::move(server);
        std::thread timer_thread(std::bind(&Node::timer_proc, this));
        m_timer_thread = std::move(timer_thread);
        std::thread connect_thread(std::bind(&Node::connect_proc, this));
        m_connect_thread = std::move(connect_thread);
        
        return true;
    }
    
    CONSOLE_LOG_FATAL("start p2p node failed!");

    return false;
}

void Node::stop()
{
    m_stop.store(true, std::memory_order_relaxed);
    m_server->stop();
    m_poller->stop();
    CONSOLE_LOG_INFO("stop p2p node success");
}

void Node::timer_proc()
{
    while(!m_stop.load(std::memory_order_relaxed))
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        m_timer_ctl.run();
    }
}

void Node::connect_proc()
{
    while(!m_stop.load(std::memory_order_relaxed))
    {
        uint32 peer_num = 0;
        {
            std::lock_guard<std::mutex> guard(m_peer_mutex);
            peer_num = m_peers.size() + m_unreg_peers.size();
        }
        
        if(peer_num >= m_max_conn)
        {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            
            continue;
        }
        
        std::unique_lock<std::mutex> lock(m_score_mutex);
        bool iter_all = true;

        for(auto iter = m_peer_scores.begin(); iter != m_peer_scores.end(); ++iter)
        {
            std::shared_ptr<Peer_Score> peer_score = *iter;
            uint8 expect = 0;

            if(m_banned_peers.find(peer_score->key()) != m_banned_peers.end())
            {
                LOG_DEBUG_INFO("try to connect banned peer %s, skipped", peer_score->key().c_str());

                continue;
            }
            
            if(peer_score->m_state.compare_exchange_strong(expect, 1))
            {
                iter_all = false;
                lock.unlock();
                const fly::net::Addr &addr = peer_score->addr();
                std::unique_ptr<fly::net::Client<Json>> client(new fly::net::Client<Json>(addr,
                                                                                          std::bind(&Node::init, this, _1),
                                                                                          std::bind(&Node::dispatch, this, _1),
                                                                                          std::bind(&Node::close, this, _1),
                                                                                          std::bind(&Node::be_closed, this, _1),
                                                                                          m_poller, 1024 * 1024)); // todo, max_msg_length
                LOG_DEBUG_INFO("try to connect peer from peer_score %s", peer_score->key().c_str());

                if(client->connect(1000))
                {
                    LOG_DEBUG_INFO("connect to peer (%s:%u) success", addr.m_host.c_str(), addr.m_port);
                }
                else
                {
                    LOG_DEBUG_ERROR("connect to peer (%s:%u) failed", addr.m_host.c_str(), addr.m_port);
                    peer_score->m_state.store(0, std::memory_order_relaxed);
                    lock.lock();
                    peer_score->sub_score(10);
                }

                break;
            }
        }

        if(iter_all)
        {
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }
}

void Node::wait()
{
    m_timer_thread.join();
    m_connect_thread.join();
    m_server->wait();
    m_poller->wait();
}

void Node::set_host(std::string host)
{
    m_host = host;
}

void Node::set_max_conn(uint32 num)
{
    m_max_conn = num;
}

uint32 Node::get_max_conn()
{
    return m_max_conn;
}

bool Node::allow(std::shared_ptr<fly::net::Connection<Json>> connection)
{
    uint32 peer_num = 0;
    {
        std::lock_guard<std::mutex> guard(m_peer_mutex);
        peer_num = m_peers.size() + m_unreg_peers.size();
    }
    
    if(peer_num > m_max_conn)
    {
        return false;
    }
    
    return true;
}

void Node::init_verify(std::shared_ptr<fly::net::Connection<Json>> connection, uint64 id)
{
    uint64 conn_id = connection->id();
    std::shared_ptr<Peer> peer = std::make_shared<Peer>();
    peer->m_timer_id = m_timer_ctl.add_timer([=]() {
            connection->close();
        }, 10, true);
    peer->m_connection = connection;
    std::unique_lock<std::mutex> lock(m_peer_mutex);
    m_unreg_peers.insert(std::make_pair(conn_id, peer));
    auto iter_unreg = m_unreg_peers.find(id);
    
    if(iter_unreg == m_unreg_peers.end())
    {
        LOG_DEBUG_ERROR("init_verify unreg peer doesn't exist");
        connection->close();
        
        return;
    }
    
    std::shared_ptr<Peer> peer_unreg = iter_unreg->second;
    lock.unlock();
    rapidjson::Document doc;
    doc.SetObject();
    rapidjson::Document::AllocatorType &allocator = doc.GetAllocator();
    doc.AddMember("msg_type", MSG_REG, allocator);
    doc.AddMember("msg_cmd", REG_VERIFY_REQ, allocator);
    doc.AddMember("id", peer_unreg->m_reg_conn_id, allocator);
    doc.AddMember("key", peer_unreg->m_remote_key, allocator);
    connection->send(doc);
    peer_unreg->m_state = 4;
}

void Node::init(std::shared_ptr<fly::net::Connection<Json>> connection)
{
    uint64 conn_id = connection->id();
    std::shared_ptr<Peer> peer = std::make_shared<Peer>();
    peer->m_timer_id = m_timer_ctl.add_timer([=]() {
            connection->close();
        }, 10, true);
    peer->m_connection = connection;
    {
        std::lock_guard<std::mutex> guard(m_peer_mutex);
        m_unreg_peers.insert(std::make_pair(conn_id, peer));
    }
    
    if(!connection->is_passive())
    {
        peer->m_addr = connection->peer_addr();
        peer->m_state = 1;
        peer->m_local_key = fly::base::random_32();
        rapidjson::Document doc;
        doc.SetObject();
        rapidjson::Document::AllocatorType &allocator = doc.GetAllocator();
        doc.AddMember("msg_type", MSG_REG, allocator);
        doc.AddMember("msg_cmd", REG_REQ, allocator);
        doc.AddMember("host", rapidjson::StringRef(m_host.c_str()), allocator);
        doc.AddMember("port", m_port, allocator);
        doc.AddMember("id", conn_id, allocator);
        doc.AddMember("key", peer->m_local_key, allocator);
        doc.AddMember("version", ASKCOIN_VERSION, allocator);
        connection->send(doc);
    }
}

void Node::dispatch(std::unique_ptr<fly::net::Message<Json>> message)
{
    std::shared_ptr<fly::net::Connection<Json>> connection = message->get_connection();
    uint64 conn_id = connection->id();
    std::unique_lock<std::mutex> lock(m_peer_mutex);
    auto iter_reg = m_peers.find(conn_id);
    uint32 type = message->type();
    uint32 cmd = message->cmd();
    uint32 msg_length = message->length(); // todo, the following cmd need check length 
    
    if(iter_reg != m_peers.end())
    {
        std::shared_ptr<Peer> peer = iter_reg->second;
        lock.unlock();

        if(type != MSG_SYS)
        {
            Blockchain::instance()->dispatch_peer_message(std::move(message));
        }
        else
        {
            if(cmd == SYS_PING)
            {
                if(!connection->is_passive())
                {
                    connection->close();
                }
                else
                {
                    static bool pong_doc = false;
                    static rapidjson::Document doc;

                    if(!pong_doc)
                    {
                        doc.SetObject();
                        rapidjson::Document::AllocatorType &allocator = doc.GetAllocator();
                        doc.AddMember("msg_type", MSG_SYS, allocator);
                        doc.AddMember("msg_cmd", SYS_PONG, allocator);
                        pong_doc = true;
                    }
                    
                    connection->send(doc);
                    m_timer_ctl.reset_timer(peer->m_timer_id);
                }
            }
            else if(cmd == SYS_PONG)
            {
                if(connection->is_passive())
                {
                    connection->close();
                }
                else
                {
                    m_timer_ctl.reset_timer(peer->m_timer_id);
                }
            }
            else
            {
                connection->close();
            }
        }
        
        ASKCOIN_RETURN;
    }

    auto iter_unreg = m_unreg_peers.find(conn_id);
    
    if(iter_unreg == m_unreg_peers.end())
    {
        LOG_DEBUG_ERROR("unreg peer doesn't exist");
        connection->close();
        
        return;
    }
    
    std::shared_ptr<Peer> peer = iter_unreg->second;
    lock.unlock();
    
    if(type != MSG_REG)
    {
        LOG_DEBUG_ERROR("unreg peer recv message type: %u not MSG_REG", type);
        connection->close();
        
        return;
    }

    rapidjson::Document& doc = message->doc();

    if(!connection->is_passive())
    {
        if(cmd == REG_RSP)
        {
            if(peer->m_state != 1)
            {
                LOG_DEBUG_ERROR("unreg peer recv message REG_RSP, but m_state is not 1");
                connection->close();
                
                return;
            }
            
            if(!doc.HasMember("version"))
            {
                connection->close();
                
                ASKCOIN_RETURN;
            }
            
            const rapidjson::Value &version = doc["version"];

            if(!version.IsUint())
            {
                connection->close();
                
                ASKCOIN_RETURN;
            }

            if(!doc.HasMember("id"))
            {
                connection->close();

                ASKCOIN_RETURN;
            }
            
            const rapidjson::Value &id = doc["id"];

            if(!id.IsUint64())
            {
                connection->close();
                
                ASKCOIN_RETURN;
            }

            if(!doc.HasMember("key"))
            {
                connection->close();

                ASKCOIN_RETURN;
            }
            
            const rapidjson::Value &key = doc["key"];

            if(!key.IsUint())
            {
                connection->close();
                
                ASKCOIN_RETURN;
            }
            
            uint32 version_u32 = version.GetUint();
            uint64 id_u64 = id.GetUint64();
            uint32 key_u32 = key.GetUint();
            LOG_DEBUG_INFO("unreg peer (m_state:1) recv message cmd REG_RSP, version:%u, id:%lu, key:%u from %s:%u", version_u32, id_u64, key_u32, \
                     connection->peer_addr().m_host.c_str(), connection->peer_addr().m_port);
            
            if(!version_compatible(version_u32, ASKCOIN_VERSION))
            {
                LOG_DEBUG_ERROR("unreg peer (m_state:1) !version_compatible(%u,%u), addr: %s", version_u32, ASKCOIN_VERSION, peer->key().c_str());
                connection->close();

                return;
            }

            peer->m_remote_key = key_u32;
            peer->m_reg_conn_id = id_u64;
            peer->m_state = 3;
        }
        else if(cmd == REG_VERIFY_RSP)
        {
            if(peer->m_state != 0)
            {
                LOG_DEBUG_ERROR("verify unreg peer recv message REG_VERIFY_RSP, but m_state is not 0");
                connection->close();
                
                return;
            }

            if(!doc.HasMember("id"))
            {
                connection->close();

                ASKCOIN_RETURN;
            }
            
            const rapidjson::Value &id = doc["id"];

            if(!id.IsUint64())
            {
                connection->close();
                
                ASKCOIN_RETURN;
            }

            if(!doc.HasMember("key"))
            {
                connection->close();

                ASKCOIN_RETURN;
            }
            
            const rapidjson::Value &key = doc["key"];

            if(!key.IsUint())
            {
                connection->close();
                
                ASKCOIN_RETURN;
            }

            uint64 id_u64 = id.GetUint64();
            uint32 key_u32 = key.GetUint();
            LOG_DEBUG_INFO("verify unreg peer (m_state:0) recv message cmd REG_VERIFY_RSP, id:%lu, key:%u", id_u64, key_u32);
            std::unique_lock<std::mutex> lock(m_peer_mutex);
            auto iter_unreg = m_unreg_peers.find(id_u64);

            if(iter_unreg == m_unreg_peers.end())
            {
                LOG_DEBUG_ERROR("after recv message cmd REG_VERIFY_RSP, unreg peer doesn't exist");
                connection->close();
        
                return;
            }

            std::shared_ptr<Peer> peer_unreg = iter_unreg->second;

            if(peer_unreg->m_state != 4)
            {
                LOG_DEBUG_ERROR("after recv message cmd REG_VERIFY_RSP, unreg peer m_state != 4");
                connection->close();
            
                return;
            }
        
            if(key_u32 != peer_unreg->m_local_key)
            {
                LOG_DEBUG_ERROR("after recv message cmd REG_VERIFY_RSP, unreg peer m_local_key != key_u32");
                connection->close();

                return;
            }
        
            m_unreg_peers.erase(id_u64);
            m_peers.insert(std::make_pair(id_u64, peer_unreg));
            m_timer_ctl.reset_timer(peer_unreg->m_timer_id);
            lock.unlock();
            connection->close();
        }
        else
        {
            connection->close();
        }

        return;
    }
    
    if(cmd == REG_REQ)
    {
        if(peer->m_state != 0)
        {
            LOG_DEBUG_ERROR("unreg peer recv message REG_REQ, but m_state is not 0");
            connection->close();

            return;
        }
        
        if(!doc.HasMember("version"))
        {
            connection->close();
                
            ASKCOIN_RETURN;
        }
            
        const rapidjson::Value &version = doc["version"];

        if(!version.IsUint())
        {
            connection->close();
                
            ASKCOIN_RETURN;
        }

        if(!doc.HasMember("id"))
        {
            connection->close();

            ASKCOIN_RETURN;
        }
            
        const rapidjson::Value &id = doc["id"];

        if(!id.IsUint64())
        {
            connection->close();
                
            ASKCOIN_RETURN;
        }

        if(!doc.HasMember("key"))
        {
            connection->close();

            ASKCOIN_RETURN;
        }
            
        const rapidjson::Value &key = doc["key"];

        if(!key.IsUint())
        {
            connection->close();
                
            ASKCOIN_RETURN;
        }

        if(!doc.HasMember("host"))
        {
            connection->close();

            ASKCOIN_RETURN;
        }
            
        const rapidjson::Value &host = doc["host"];

        if(!host.IsString())
        {
            connection->close();
                
            ASKCOIN_RETURN;
        }

        if(!doc.HasMember("port"))
        {
            connection->close();

            ASKCOIN_RETURN;
        }
            
        const rapidjson::Value &port = doc["port"];

        if(!port.IsUint())
        {
            connection->close();
            
            ASKCOIN_RETURN;
        }
        
        uint32 version_u32 = version.GetUint();
        uint64 id_u64 = id.GetUint64();
        std::string host_str = host.GetString();
        uint16 port_u16 = port.GetUint();
        uint32 key_u32 = key.GetUint();
        LOG_DEBUG_INFO("unreg peer (m_state:0) recv message cmd REG_REQ, version:%u, id:%lu, key:%u, host:%s, port:%u", version_u32, id_u64, key_u32, host_str.c_str(), port_u16);
        if(!version_compatible(version_u32, ASKCOIN_VERSION))
        {
            LOG_DEBUG_ERROR("unreg peer (m_state:0) !version_compatible(%u,%u), addr: %s:%u", version_u32, ASKCOIN_VERSION, host_str.c_str(), port_u16);
            connection->close();
            
            return;
        }
        
        peer->m_local_key = fly::base::random_32();
        peer->m_remote_key = key_u32;
        peer->m_reg_conn_id = id_u64;
        peer->m_addr = fly::net::Addr(host_str, port_u16);
        std::shared_ptr<Peer_Score> peer_score = std::make_shared<Peer_Score>(peer->m_addr);
        std::unique_lock<std::mutex> lock(m_score_mutex);
        
        if(m_banned_peers.find(peer_score->key()) != m_banned_peers.end())
        {
            LOG_DEBUG_ERROR("unreg peer (m_state:0) is banned, addr: %s:%u", host_str.c_str(), port_u16);
            connection->close();
            
            return;
        }
        
        auto iter = m_peer_score_map.find(peer_score->key());
        
        if(iter != m_peer_score_map.end())
        {
            peer_score = iter->second;
        }
        else
        {
            add_peer_score(peer_score);
        }
        
        lock.unlock();
        uint8 expect = 0;

        if(peer_score->m_state.compare_exchange_strong(expect, 1))
        {
            peer->m_state = 2;
            rapidjson::Document doc;
            doc.SetObject();
            rapidjson::Document::AllocatorType &allocator = doc.GetAllocator();
            doc.AddMember("msg_type", MSG_REG, allocator);
            doc.AddMember("msg_cmd", REG_RSP, allocator);
            doc.AddMember("id", conn_id, allocator);
            doc.AddMember("key", peer->m_local_key, allocator);
            doc.AddMember("version", ASKCOIN_VERSION, allocator);
            connection->send(doc);

            std::thread tmp_thread([=, &lock]() {
                    std::unique_ptr<fly::net::Client<Json>> client(new fly::net::Client<Json>(peer->m_addr,
                                                                                              std::bind(&Node::init_verify, this, _1, conn_id),
                                                                                              std::bind(&Node::dispatch, this, _1),
                                                                                              std::bind(&Node::close, this, _1),
                                                                                              std::bind(&Node::be_closed, this, _1),
                                                                                              m_poller));
                    if(client->connect(1000))
                    {
                        LOG_DEBUG_INFO("unreg peer (m_state:2) connect to peer (%s:%u) success", peer->m_addr.m_host.c_str(), peer->m_addr.m_port);
                    }
                    else
                    {
                        LOG_DEBUG_ERROR("unreg peer (m_state:2) connect to peer (%s:%u) failed", peer->m_addr.m_host.c_str(), peer->m_addr.m_port);
                        connection->close();
                        lock.lock();
                        peer_score->sub_score(100);
                    }
                });
            tmp_thread.detach();
        }
        else
        {
            LOG_DEBUG_ERROR("peer (%s) already registered, so close request connection", peer_score->key().c_str());
            connection->close();
        }
    }
    else if(cmd == REG_VERIFY_REQ)
    {
        if(peer->m_state != 0)
        {
            LOG_DEBUG_ERROR("verify unreg peer recv message REG_VERIFY_REQ, but m_state is not 0");
            connection->close();

            return;
        }

        if(!doc.HasMember("id"))
        {
            connection->close();

            ASKCOIN_RETURN;
        }
            
        const rapidjson::Value &id = doc["id"];

        if(!id.IsUint64())
        {
            connection->close();
                
            ASKCOIN_RETURN;
        }

        if(!doc.HasMember("key"))
        {
            connection->close();

            ASKCOIN_RETURN;
        }
            
        const rapidjson::Value &key = doc["key"];

        if(!key.IsUint())
        {
            connection->close();
                
            ASKCOIN_RETURN;
        }

        uint64 id_u64 = id.GetUint64();
        uint32 key_u32 = key.GetUint();
        LOG_DEBUG_INFO("verify unreg peer (m_state:0) recv message cmd REG_VERIFY_REQ, id:%lu, key:%u", id_u64, key_u32);
        std::unique_lock<std::mutex> lock(m_peer_mutex);
        auto iter_unreg = m_unreg_peers.find(id_u64);

        if(iter_unreg == m_unreg_peers.end())
        {
            LOG_DEBUG_ERROR("after recv message cmd REG_VERIFY_REQ, unreg peer doesn't exist");
            connection->close();
        
            return;
        }

        std::shared_ptr<Peer> peer_unreg = iter_unreg->second;

        if(peer_unreg->m_state != 3)
        {
            LOG_DEBUG_ERROR("after recv message cmd REG_VERIFY_REQ, unreg peer m_state != 3");
            connection->close();
            
            return;
        }

        if(key_u32 != peer_unreg->m_local_key)
        {
            LOG_DEBUG_ERROR("after recv message cmd REG_VERIFY_REQ, unreg peer m_local_key != key_u32");
            connection->close();

            return;
        }

        m_unreg_peers.erase(id_u64);
        m_peers.insert(std::make_pair(id_u64, peer_unreg));
        m_timer_ctl.reset_timer(peer_unreg->m_timer_id);
        std::shared_ptr<fly::net::Connection<Json>> reg_connection = peer_unreg->m_connection;
        peer_unreg->m_ping_timer_id = m_timer_ctl.add_timer([=]() {
                static bool ping_doc = false;
                static rapidjson::Document doc;

                if(!ping_doc)
                {
                    doc.SetObject();
                    rapidjson::Document::AllocatorType &allocator = doc.GetAllocator();
                    doc.AddMember("msg_type", MSG_SYS, allocator);
                    doc.AddMember("msg_cmd", SYS_PING, allocator);
                    ping_doc = true;
                }

                reg_connection->send(doc);
            }, 5);
        lock.unlock();
        rapidjson::Document doc;
        doc.SetObject();
        rapidjson::Document::AllocatorType &allocator = doc.GetAllocator();
        doc.AddMember("msg_type", MSG_REG, allocator);
        doc.AddMember("msg_cmd", REG_VERIFY_RSP, allocator);
        doc.AddMember("key", peer_unreg->m_remote_key, allocator);
        doc.AddMember("id", peer_unreg->m_reg_conn_id, allocator);
        connection->send(doc);
    }
    else
    {
        connection->close();
    }
}

void Node::close(std::shared_ptr<fly::net::Connection<Json>> connection)
{
    uint64 conn_id = connection->id();
    std::unique_lock<std::mutex> lock(m_peer_mutex);
    auto iter_reg = m_peers.find(conn_id);
    auto iter_unreg = m_unreg_peers.find(conn_id);
    std::shared_ptr<Peer> peer;
    LOG_DEBUG_INFO("close connection from %s:%d", connection->peer_addr().m_host.c_str(), connection->peer_addr().m_port);
    
    if(iter_reg == m_peers.end())
    {
        peer = iter_unreg->second;
        m_unreg_peers.erase(conn_id);
        
        if(peer->m_state == 0)
        {
            LOG_DEBUG_INFO("unreg peer (m_state:0) close");

            return;
        }
        
        LOG_DEBUG_INFO("unreg peer (%s) close", peer->key().c_str());
    }
    else
    {
        peer = iter_reg->second;
        LOG_DEBUG_INFO("reg peer (%s) close", peer->key().c_str());
        m_peers.erase(conn_id);
    }

    lock.unlock();
    m_timer_ctl.del_timer(peer->m_timer_id);
    m_timer_ctl.del_timer(peer->m_ping_timer_id);
    std::lock_guard<std::mutex> guard(m_score_mutex);
    auto iter_score = m_peer_score_map.find(peer->key());
    
    if(iter_score == m_peer_score_map.end())
    {
        ASKCOIN_RETURN;
    }

    std::shared_ptr<Peer_Score> peer_score = iter_score->second;
    peer_score->sub_score(1);
    peer_score->m_state.store(0, std::memory_order_relaxed);
}

void Node::be_closed(std::shared_ptr<fly::net::Connection<Json>> connection)
{
    uint64 conn_id = connection->id();
    std::unique_lock<std::mutex> lock(m_peer_mutex);
    auto iter_reg = m_peers.find(conn_id);
    auto iter_unreg = m_unreg_peers.find(conn_id);
    std::shared_ptr<Peer> peer;
    LOG_DEBUG_INFO("close connection from %s:%d be closed", connection->peer_addr().m_host.c_str(), connection->peer_addr().m_port);
    
    if(iter_reg == m_peers.end())
    {
        peer = iter_unreg->second;
        m_unreg_peers.erase(conn_id);
        
        if(peer->m_state == 0)
        {
            LOG_DEBUG_INFO("unreg peer (m_state:0) be closed");

            return;
        }
        
        LOG_DEBUG_INFO("unreg peer (%s) be closed", peer->key().c_str());
    }
    else
    {
        peer = iter_reg->second;
        LOG_DEBUG_INFO("reg peer (%s) be closed", peer->key().c_str());
        m_peers.erase(conn_id);
    }

    lock.unlock();
    m_timer_ctl.del_timer(peer->m_timer_id);
    m_timer_ctl.del_timer(peer->m_ping_timer_id);
    std::lock_guard<std::mutex> guard(m_score_mutex);
    auto iter_score = m_peer_score_map.find(peer->key());
    
    if(iter_score == m_peer_score_map.end())
    {
        ASKCOIN_RETURN;
    }
    
    std::shared_ptr<Peer_Score> peer_score = iter_score->second;
    peer_score->sub_score(1);
    peer_score->m_state.store(0, std::memory_order_relaxed);
}

bool Node::insert_peer_score(const std::shared_ptr<Peer_Score> &peer_score)
{
    m_peer_scores.insert(peer_score);
}

bool Node::erase_peer_score(const std::shared_ptr<Peer_Score> &peer_score)
{
    auto iter_end = m_peer_scores.upper_bound(peer_score);
    
    for(auto iter = m_peer_scores.lower_bound(peer_score); iter != iter_end; ++iter)
    {
        if(*iter == peer_score)
        {
            m_peer_scores.erase(iter);
            
            return true;
        }
    }
    
    return false;
}

bool Node::add_peer_score(const std::shared_ptr<Peer_Score> &peer_score)
{
    std::string key = peer_score->key();
    
    if(m_peer_score_map.find(key) != m_peer_score_map.end())
    {
        return false;
    }
    
    m_peer_scores.insert(peer_score);
    m_peer_score_map.insert(std::make_pair(key, peer_score));
    
    return true;
}

// todo, when del_peer_score ?
bool Node::del_peer_score(const std::shared_ptr<Peer_Score> &peer_score)
{
    std::string key = peer_score->key();
    
    if(m_peer_score_map.find(key) == m_peer_score_map.end())
    {
        return false;
    }
    
    m_peer_score_map.erase(key);
    auto iter_end = m_peer_scores.upper_bound(peer_score);
    
    for(auto iter = m_peer_scores.lower_bound(peer_score); iter != iter_end; ++iter)
    {
        if(*iter == peer_score)
        {
            m_peer_scores.erase(iter);
            
            return true;
        }
    }

    return false;
}

}
}

void Blockchain::punish_peer(std::shared_ptr<net::p2p::Peer> peer)
{
    net::p2p::Node *p2p_node = net::p2p::Node::instance();
    std::unordered_map<std::string, std::shared_ptr<net::p2p::Peer_Score>> &peer_score_map = p2p_node->m_peer_score_map;
    peer->m_connection->close();
    {
        std::lock_guard<std::mutex> guard(p2p_node->m_score_mutex);
        auto iter_score = peer_score_map.find(peer->key());
        
        if(iter_score == peer_score_map.end())
        {
            ASKCOIN_RETURN;
        }

        if(peer->m_punish_timer_id > 0)
        {
            p2p_node->m_timer_ctl.del_timer(peer->m_punish_timer_id);
        }
        
        LOG_DEBUG_INFO("punish_peer + banned, peer: %s", peer->key().c_str());
        std::shared_ptr<net::p2p::Peer_Score> peer_score = iter_score->second;
        peer_score->sub_score(1000);
        p2p_node->m_banned_peers.insert(peer->key());
        peer->m_punish_timer_id = p2p_node->m_timer_ctl.add_timer([=]() {
                std::lock_guard<std::mutex> guard(p2p_node->m_score_mutex);
                p2p_node->m_banned_peers.erase(peer->key());
                LOG_DEBUG_INFO("unbanned peer: %s", peer->key().c_str());
            }, 600, true);
    }
}

void Blockchain::punish_brief_req(std::shared_ptr<Pending_Brief_Request> request)
{
    for(auto iter = m_pending_brief_chains.begin(); iter != m_pending_brief_chains.end();)
    {
        std::shared_ptr<Pending_Chain> pending_chain = *iter;

        if(!pending_chain->m_brief_attached)
        {
            ++iter;
            continue;
        }
        
        std::shared_ptr<net::p2p::Peer> peer = pending_chain->m_peer;
        iter = m_pending_brief_chains.erase(iter);
        punish_peer(peer);
        m_pending_peer_keys.erase(peer->key());
        LOG_DEBUG_INFO("punish_brief_req, peer key: %s, block_hash: %s", peer->key().c_str(), request->m_hash.c_str());
    }
    
    m_timer_ctl.del_timer(request->m_timer_id);
    m_pending_brief_reqs.erase(request->m_hash);
}

void Blockchain::punish_detail_req(std::shared_ptr<Pending_Detail_Request> request)
{
    for(auto iter = m_brief_chains.begin(); iter != m_brief_chains.end();)
    {
        std::shared_ptr<Pending_Chain> pending_chain = *iter;
        
        if(!pending_chain->m_detail_attached)
        {
            ++iter;
            continue;
        }

        std::shared_ptr<net::p2p::Peer> peer = pending_chain->m_peer;
        iter = m_brief_chains.erase(iter);
        punish_peer(peer);
        m_pending_peer_keys.erase(peer->key());
        m_is_switching = false;
        std::string block_hash = request->m_owner_chain->m_req_blocks[request->m_owner_chain->m_start]->m_hash;
        LOG_DEBUG_INFO("punish_detail_req, peer key: %s, block_hash: %s", peer->key().c_str(), block_hash.c_str());
    }
    
    m_timer_ctl.del_timer(request->m_timer_id);
}

void Blockchain::do_peer_message(std::unique_ptr<fly::net::Message<Json>> &message)
{
    std::shared_ptr<fly::net::Connection<Json>> connection = message->get_connection();
    uint64 conn_id = connection->id();
    uint32 type = message->type();
    uint32 cmd = message->cmd();
    net::p2p::Node *p2p_node = net::p2p::Node::instance();
    std::unordered_map<uint64, std::shared_ptr<net::p2p::Peer>> &peers = p2p_node->m_peers;
    std::unique_lock<std::mutex> lock(p2p_node->m_peer_mutex);
    auto iter_reg = peers.find(conn_id);

    if(iter_reg == peers.end())
    {
        ASKCOIN_RETURN;
    }

    std::shared_ptr<net::p2p::Peer> peer = iter_reg->second;
    lock.unlock();

    if(peer->m_connection != connection)
    {
        LOG_FATAL("do_peer_message, peer->m_connection != connection, peer key: %s", peer->key().c_str());

        return;
    }
    
    rapidjson::Document& doc = message->doc();
    uint32 msg_length = message->length(); // todo, the following need check length
    LOG_DEBUG_INFO("peer msg: %s, length: %u, peer key: %s", message->raw_data().c_str(), msg_length, peer->key().c_str());
    
    if(type == net::p2p::MSG_BLOCK)
    {
        if(cmd == net::p2p::BLOCK_BROADCAST)
        {
            if(m_pending_peer_keys.find(peer->key()) != m_pending_peer_keys.end())
            {
                ASKCOIN_RETURN;
            }

            if(!doc.HasMember("hash"))
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }

            if(!doc.HasMember("sign"))
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }

            if(!doc["hash"].IsString())
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }

            if(!doc["sign"].IsString())
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }

            std::string block_hash = doc["hash"].GetString();
            std::string block_sign = doc["sign"].GetString();

            if(block_hash.length() != 44)
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }

            if(m_blocks.find(block_hash) != m_blocks.end())
            {
                ASKCOIN_RETURN;
            }

            if(!doc.HasMember("data"))
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }
            
            const rapidjson::Value &data = doc["data"];

            if(!data.IsObject())
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }

            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            data.Accept(writer);
            std::string data_str(buffer.GetString(), buffer.GetSize());
            std::string block_hash_verify = coin_hash_b64(buffer.GetString(), buffer.GetSize());
            
            if(block_hash != block_hash_verify)
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }

            if(!doc.HasMember("pow"))
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }

            const rapidjson::Value &pow_array = doc["pow"];

            if(!pow_array.IsArray())
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }
            
            uint32 pow_num = pow_array.Size();

            if(pow_num != 9)
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }

            for(uint32 i = 0; i < 9; ++i)
            {
                if(!pow_array[i].IsUint())
                {
                    punish_peer(peer);
                    ASKCOIN_RETURN;
                }
            }
            
            Accum_Pow declared_pow(pow_array[0].GetUint(), pow_array[1].GetUint(), pow_array[2].GetUint(), pow_array[3].GetUint(), pow_array[4].GetUint(), \
                                pow_array[5].GetUint(), pow_array[6].GetUint(), pow_array[7].GetUint(), pow_array[8].GetUint());

            // todo, what if is switching?
            if(!m_most_difficult_block->difficult_than_me(declared_pow))
            {
                ASKCOIN_RETURN;
            }

            if(!data.HasMember("id"))
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }

            if(!data["id"].IsUint64())
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }

            uint64 block_id = data["id"].GetUint64();

            if(block_id == 0)
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }

            if(!data.HasMember("utc"))
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }

            if(!data["utc"].IsUint64())
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }

            uint64 utc = data["utc"].GetUint64();

            if(!data.HasMember("version"))
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }

            if(!data["version"].IsUint())
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }

            // todo, version compatible?
            uint32 version = data["version"].GetUint();
            
            if(!data.HasMember("zero_bits"))
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }

            if(!data["zero_bits"].IsUint())
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }

            uint32 zero_bits = data["zero_bits"].GetUint();

            if(zero_bits == 0 || zero_bits > 256)
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }
            
            if(!data.HasMember("pre_hash"))
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }

            if(!data["pre_hash"].IsString())
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }

            std::string pre_hash = data["pre_hash"].GetString();

            if(pre_hash.length() != 44)
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }
            
            if(!data.HasMember("miner"))
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }

            if(!data["miner"].IsString())
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }

            std::string miner_pubkey = data["miner"].GetString();

            if(miner_pubkey.length() != 88)
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }

            if(!data.HasMember("nonce"))
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }

            const rapidjson::Value &nonce = data["nonce"];

            if(!nonce.IsArray())
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }

            if(nonce.Size() != 4)
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }
            
            for(uint32 i = 0; i < 4; ++i)
            {
                if(!nonce[i].IsUint64())
                {
                    punish_peer(peer);
                    ASKCOIN_RETURN;
                }
            }
            
            if(!data.HasMember("tx_ids"))
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }
            
            const rapidjson::Value &tx_ids = data["tx_ids"];

            if(!tx_ids.IsArray())
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }
            
            uint32 tx_num = tx_ids.Size();
            
            if(tx_num > 2000)
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }
            
            for(rapidjson::Value::ConstValueIterator iter = tx_ids.Begin(); iter != tx_ids.End(); ++iter)
            {
                std::string tx_id = iter->GetString();
                
                if(tx_id.length() != 44)
                {
                    punish_peer(peer);
                    ASKCOIN_RETURN;
                }
            }
            
            if(!verify_sign(miner_pubkey, block_hash, block_sign))
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }
            
            if(!verify_hash(block_hash, data_str, zero_bits))
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }
            
            std::shared_ptr<Pending_Block> pending_block;
            auto iter_pending_block = m_pending_blocks.find(block_hash);
            bool is_new_pending_block = false;
            
            if(iter_pending_block != m_pending_blocks.end())
            {
                pending_block = iter_pending_block->second;
            }
            else
            {
                pending_block = std::make_shared<Pending_Block>(block_id, utc, version, zero_bits, block_hash, pre_hash);
                is_new_pending_block = true;
            }
            
            std::shared_ptr<Pending_Chain> pending_chain(new Pending_Chain(peer, pending_block, declared_pow));
            
            if(!pending_chain->m_remain_pow.sub_pow(pending_block->m_zero_bits))
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }
            
            if(is_new_pending_block)
            {
                auto iter_brief_req = m_pending_brief_reqs.find(block_hash);
                
                if(iter_brief_req != m_pending_brief_reqs.end())
                {
                    std::shared_ptr<Pending_Brief_Request> request = iter_brief_req->second;
                    m_timer_ctl.del_timer(request->m_timer_id);
                    m_pending_brief_reqs.erase(block_hash);
                }
                
                m_pending_blocks.insert(std::make_pair(block_hash, pending_block));
                m_pending_block_hashes.push_back(block_hash);
                
                if(m_pending_block_hashes.size() > 1000000)
                {
                    m_pending_blocks.erase(m_pending_block_hashes.front());
                    m_pending_block_hashes.pop_front();
                }
            }
            
            m_pending_peer_keys.insert(peer->key());
            uint64 now = time(NULL);
            
            if(utc > now)
            {
                uint32 diff = utc - now;
                
                if(diff > 3600)
                {
                    LOG_DEBUG_WARN("block time too future, diff: %u > 3600, hash: %s, peer key: %s", diff, block_hash.c_str(), peer->key().c_str());
                }
                
                m_timer_ctl.add_timer([=]() {
                        m_pending_brief_chains.push_back(pending_chain);
                    }, diff, true);
            }
            else
            {
                m_pending_brief_chains.push_back(pending_chain);
            }
        }
        else if(cmd == net::p2p::BLOCK_BRIEF_REQ)
        {
            if(!doc.HasMember("hash"))
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }
            
            if(!doc["hash"].IsString())
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }
            
            std::string block_hash = doc["hash"].GetString();
            
            if(block_hash.length() != 44)
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }
            
            auto iter = m_blocks.find(block_hash);
            
            if(iter == m_blocks.end())
            {
                ASKCOIN_RETURN;
            }

            std::string block_data;
            leveldb::Status s = m_db->Get(leveldb::ReadOptions(), block_hash, &block_data);
            
            if(!s.ok())
            {
                ASKCOIN_EXIT(EXIT_FAILURE);
            }
            
            rapidjson::Document doc;
            const char *block_data_str = block_data.c_str();
            doc.Parse(block_data_str);
            
            if(doc.HasParseError())
            {
                ASKCOIN_EXIT(EXIT_FAILURE);
            }

            rapidjson::Value &hash_node = doc["hash"];
            std::string block_hash_db = hash_node.GetString();
            
            if(block_hash != block_hash_db)
            {
                ASKCOIN_EXIT(EXIT_FAILURE);
            }

            rapidjson::Value &sign_node = doc["sign"];
            std::string block_sign = sign_node.GetString();
            rapidjson::Value &data = doc["data"];
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            data.Accept(writer);
            std::string block_hash_verify = coin_hash_b64(buffer.GetString(), buffer.GetSize());

            if(block_hash != block_hash_verify)
            {
                ASKCOIN_EXIT(EXIT_FAILURE);
            }
            
            std::string miner_pubkey = data["miner"].GetString();
            
            if(miner_pubkey.length() != 88)
            {
                ASKCOIN_EXIT(EXIT_FAILURE);
            }
            
            if(!verify_sign(miner_pubkey, block_hash, block_sign))
            {
                ASKCOIN_EXIT(EXIT_FAILURE);
            }
            
            {
                rapidjson::Document doc;
                doc.SetObject();
                rapidjson::Document::AllocatorType &allocator = doc.GetAllocator();
                doc.AddMember("msg_type", net::p2p::MSG_BLOCK, allocator);
                doc.AddMember("msg_cmd", net::p2p::BLOCK_BRIEF_RSP, allocator);
                doc.AddMember("hash", hash_node, allocator);
                doc.AddMember("sign", sign_node, allocator);
                doc.AddMember("data", data, allocator);
                connection->send(doc);
            }
        }
        else if(cmd == net::p2p::BLOCK_BRIEF_RSP)
        {
            if(!doc.HasMember("hash"))
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }

            if(!doc.HasMember("sign"))
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }

            if(!doc["hash"].IsString())
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }

            if(!doc["sign"].IsString())
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }
            
            std::string block_hash = doc["hash"].GetString();
            std::string block_sign = doc["sign"].GetString();

            if(!is_base64_char(block_hash))
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }
            
            if(!is_base64_char(block_sign))
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }

            if(block_hash.length() != 44)
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }
            
            if(m_blocks.find(block_hash) != m_blocks.end())
            {
                ASKCOIN_RETURN;
            }

            if(m_pending_blocks.find(block_hash) != m_pending_blocks.end())
            {
                ASKCOIN_RETURN;
            }

            auto iter_brief_req = m_pending_brief_reqs.find(block_hash);
            
            if(iter_brief_req == m_pending_brief_reqs.end())
            {
                ASKCOIN_RETURN;
            }

            std::shared_ptr<Pending_Brief_Request> request = iter_brief_req->second;

            if(!doc.HasMember("data"))
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }
            
            const rapidjson::Value &data = doc["data"];

            if(!data.IsObject())
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }

            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            data.Accept(writer);
            std::string data_str(buffer.GetString(), buffer.GetSize());
            std::string block_hash_verify = coin_hash_b64(buffer.GetString(), buffer.GetSize());
            
            if(block_hash != block_hash_verify)
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }
            
            if(!data.HasMember("id"))
            {
                punish_brief_req(request);
                ASKCOIN_RETURN;
            }

            if(!data["id"].IsUint64())
            {
                punish_brief_req(request);
                ASKCOIN_RETURN;
            }

            uint64 block_id = data["id"].GetUint64();

            if(block_id == 0)
            {
                punish_brief_req(request);
                ASKCOIN_RETURN;
            }

            if(!data.HasMember("utc"))
            {
                punish_brief_req(request);
                ASKCOIN_RETURN;
            }

            if(!data["utc"].IsUint64())
            {
                punish_brief_req(request);
                ASKCOIN_RETURN;
            }

            uint64 utc = data["utc"].GetUint64();

            if(!data.HasMember("version"))
            {
                punish_brief_req(request);
                ASKCOIN_RETURN;
            }

            if(!data["version"].IsUint())
            {
                punish_brief_req(request);
                ASKCOIN_RETURN;
            }
            
            // todo, version compatible?
            uint32 version = data["version"].GetUint();
            
            if(!data.HasMember("zero_bits"))
            {
                punish_brief_req(request);
                ASKCOIN_RETURN;
            }

            if(!data["zero_bits"].IsUint())
            {
                punish_brief_req(request);
                ASKCOIN_RETURN;
            }

            uint32 zero_bits = data["zero_bits"].GetUint();

            if(zero_bits == 0 || zero_bits > 256)
            {
                punish_brief_req(request);
                ASKCOIN_RETURN;
            }
            
            if(!data.HasMember("pre_hash"))
            {
                punish_brief_req(request);
                ASKCOIN_RETURN;
            }

            if(!data["pre_hash"].IsString())
            {
                punish_brief_req(request);
                ASKCOIN_RETURN;
            }

            std::string pre_hash = data["pre_hash"].GetString();

            if(pre_hash.length() != 44)
            {
                punish_brief_req(request);
                ASKCOIN_RETURN;
            }
            
            if(!data.HasMember("miner"))
            {
                punish_brief_req(request);
                ASKCOIN_RETURN;
            }

            if(!data["miner"].IsString())
            {
                punish_brief_req(request);
                ASKCOIN_RETURN;
            }

            std::string miner_pubkey = data["miner"].GetString();

            if(miner_pubkey.length() != 88)
            {
                punish_brief_req(request);
                ASKCOIN_RETURN;
            }

            if(!data.HasMember("nonce"))
            {
                punish_brief_req(request);
                ASKCOIN_RETURN;
            }

            const rapidjson::Value &nonce = data["nonce"];

            if(!nonce.IsArray())
            {
                punish_brief_req(request);
                ASKCOIN_RETURN;
            }

            if(nonce.Size() != 4)
            {
                punish_brief_req(request);
                ASKCOIN_RETURN;
            }
            
            for(uint32 i = 0; i < 4; ++i)
            {
                if(!nonce[i].IsUint64())
                {
                    punish_brief_req(request);
                    ASKCOIN_RETURN;
                }
            }
            
            if(!data.HasMember("tx_ids"))
            {
                punish_brief_req(request);
                ASKCOIN_RETURN;
            }
            
            const rapidjson::Value &tx_ids = data["tx_ids"];

            if(!tx_ids.IsArray())
            {
                punish_brief_req(request);
                ASKCOIN_RETURN;
            }
            
            uint32 tx_num = tx_ids.Size();
            
            if(tx_num > 2000)
            {
                punish_brief_req(request);
                ASKCOIN_RETURN;
            }

            for(rapidjson::Value::ConstValueIterator iter = tx_ids.Begin(); iter != tx_ids.End(); ++iter)
            {
                std::string tx_id = iter->GetString();
                
                if(tx_id.length() != 44)
                {
                    punish_brief_req(request);
                    ASKCOIN_RETURN;
                }
            }
            
            if(!verify_sign(miner_pubkey, block_hash, block_sign))
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }
            
            if(!verify_hash(block_hash, data_str, zero_bits))
            {
                punish_brief_req(request);
                ASKCOIN_RETURN;
            }
            
            auto pending_block = std::make_shared<Pending_Block>(block_id, utc, version, zero_bits, block_hash, pre_hash);
            m_pending_blocks.insert(std::make_pair(block_hash, pending_block));
            m_pending_block_hashes.push_back(block_hash);

            if(m_pending_block_hashes.size() > 1000000)
            {
                m_pending_blocks.erase(m_pending_block_hashes.front());
                m_pending_block_hashes.pop_front();
            }
            
            m_timer_ctl.del_timer(request->m_timer_id);
            m_pending_brief_reqs.erase(block_hash);
        }
        else if(cmd == net::p2p::BLOCK_DETAIL_REQ)
        {
            if(!doc.HasMember("hash"))
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }
            
            if(!doc["hash"].IsString())
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }
            
            std::string block_hash = doc["hash"].GetString();
            
            if(block_hash.length() != 44)
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }
            
            auto iter = m_blocks.find(block_hash);
            
            if(iter == m_blocks.end())
            {
                ASKCOIN_RETURN;
            }

            std::string block_data;
            leveldb::Status s = m_db->Get(leveldb::ReadOptions(), block_hash, &block_data);
            
            if(!s.ok())
            {
                ASKCOIN_EXIT(EXIT_FAILURE);
            }
            
            rapidjson::Document doc;
            const char *block_data_str = block_data.c_str();
            doc.Parse(block_data_str);
            
            if(doc.HasParseError())
            {
                ASKCOIN_EXIT(EXIT_FAILURE);
            }

            rapidjson::Value &hash_node = doc["hash"];
            std::string block_hash_db = hash_node.GetString();
            
            if(block_hash != block_hash_db)
            {
                ASKCOIN_EXIT(EXIT_FAILURE);
            }

            rapidjson::Value &sign_node = doc["sign"];
            std::string block_sign = sign_node.GetString();
            rapidjson::Value &data = doc["data"];
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            data.Accept(writer);
            std::string block_hash_verify = coin_hash_b64(buffer.GetString(), buffer.GetSize());

            if(block_hash != block_hash_verify)
            {
                ASKCOIN_EXIT(EXIT_FAILURE);
            }
            
            std::string miner_pubkey = data["miner"].GetString();
            
            if(miner_pubkey.length() != 88)
            {
                ASKCOIN_EXIT(EXIT_FAILURE);
            }
            
            if(!verify_sign(miner_pubkey, block_hash, block_sign))
            {
                ASKCOIN_EXIT(EXIT_FAILURE);
            }
            
            {
                rapidjson::Document doc;
                doc.SetObject();
                rapidjson::Document::AllocatorType &allocator = doc.GetAllocator();
                doc.AddMember("msg_type", net::p2p::MSG_BLOCK, allocator);
                doc.AddMember("msg_cmd", net::p2p::BLOCK_DETAIL_RSP, allocator);
                doc.AddMember("hash", hash_node, allocator);
                doc.AddMember("sign", sign_node, allocator);
                doc.AddMember("data", data, allocator);
                doc.AddMember("tx", doc["tx"], allocator);
                connection->send(doc);
            }
        }
        else if(cmd == net::p2p::BLOCK_DETAIL_RSP)
        {
            if(!doc.HasMember("hash"))
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }

            if(!doc.HasMember("sign"))
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }

            if(!doc["hash"].IsString())
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }

            if(!doc["sign"].IsString())
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }
            
            std::string block_hash = doc["hash"].GetString();
            std::string block_sign = doc["sign"].GetString();

            if(!is_base64_char(block_hash))
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }
            
            if(!is_base64_char(block_sign))
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }

            if(block_hash.length() != 44)
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }

            if(m_blocks.find(block_hash) != m_blocks.end())
            {
                ASKCOIN_RETURN;
            }

            if(!m_is_switching)
            {
                ASKCOIN_RETURN;
            }

            if(!doc.HasMember("data"))
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }
            
            const rapidjson::Value &data = doc["data"];

            if(!data.IsObject())
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }

            if(!doc.HasMember("tx"))
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }

            const rapidjson::Value &tx = doc["tx"];
            
            if(!tx.IsArray())
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }

            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            data.Accept(writer);
            std::string data_str(buffer.GetString(), buffer.GetSize());
            std::string block_hash_verify = coin_hash_b64(buffer.GetString(), buffer.GetSize());
            
            if(block_hash != block_hash_verify)
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }

            auto request = m_detail_request;

            if(!data.HasMember("id"))
            {
                punish_detail_req(request);
                ASKCOIN_RETURN;
            }

            if(!data["id"].IsUint64())
            {
                punish_detail_req(request);
                ASKCOIN_RETURN;
            }

            uint64 block_id = data["id"].GetUint64();

            if(block_id == 0)
            {
                punish_detail_req(request);
                ASKCOIN_RETURN;
            }

            if(!data.HasMember("utc"))
            {
                punish_detail_req(request);
                ASKCOIN_RETURN;
            }

            if(!data["utc"].IsUint64())
            {
                punish_detail_req(request);
                ASKCOIN_RETURN;
            }

            uint64 utc = data["utc"].GetUint64();

            if(!data.HasMember("version"))
            {
                punish_detail_req(request);
                ASKCOIN_RETURN;
            }

            if(!data["version"].IsUint())
            {
                punish_detail_req(request);
                ASKCOIN_RETURN;
            }
            
            // todo, version compatible?
            uint32 version = data["version"].GetUint();
            
            if(!version_compatible(version, ASKCOIN_VERSION))
            {
                LOG_ERROR("recv BLOCK_DETAIL_RSP, but !version_compatible(%u, %u)", version, ASKCOIN_VERSION);
                punish_detail_req(request);
                ASKCOIN_RETURN;
            }
            
            if(!data.HasMember("zero_bits"))
            {
                punish_detail_req(request);
                ASKCOIN_RETURN;
            }

            if(!data["zero_bits"].IsUint())
            {
                punish_detail_req(request);
                ASKCOIN_RETURN;
            }

            uint32 zero_bits = data["zero_bits"].GetUint();

            if(zero_bits == 0 || zero_bits > 256)
            {
                punish_detail_req(request);
                ASKCOIN_RETURN;
            }
            
            if(!data.HasMember("pre_hash"))
            {
                punish_detail_req(request);
                ASKCOIN_RETURN;
            }

            if(!data["pre_hash"].IsString())
            {
                punish_detail_req(request);
                ASKCOIN_RETURN;
            }
            
            std::string pre_hash = data["pre_hash"].GetString();

            if(pre_hash.length() != 44)
            {
                punish_detail_req(request);
                ASKCOIN_RETURN;
            }

            if(!is_base64_char(pre_hash))
            {
                punish_detail_req(request);
                ASKCOIN_RETURN;
            }
            
            if(!data.HasMember("miner"))
            {
                punish_detail_req(request);
                ASKCOIN_RETURN;
            }

            if(!data["miner"].IsString())
            {
                punish_detail_req(request);
                ASKCOIN_RETURN;
            }

            std::string miner_pubkey = data["miner"].GetString();

            if(miner_pubkey.length() != 88)
            {
                punish_detail_req(request);
                ASKCOIN_RETURN;
            }

            if(!is_base64_char(miner_pubkey))
            {
                punish_detail_req(request);
                ASKCOIN_RETURN;
            }
            
            std::shared_ptr<Account> miner_account;
            
            if(!get_account(miner_pubkey, miner_account))
            {
                punish_detail_req(request);
                ASKCOIN_RETURN;
            }
            
            if(!data.HasMember("nonce"))
            {
                punish_detail_req(request);
                ASKCOIN_RETURN;
            }
            
            const rapidjson::Value &nonce = data["nonce"];

            if(!nonce.IsArray())
            {
                punish_detail_req(request);
                ASKCOIN_RETURN;
            }

            if(nonce.Size() != 4)
            {
                punish_detail_req(request);
                ASKCOIN_RETURN;
            }
            
            for(uint32 i = 0; i < 4; ++i)
            {
                if(!nonce[i].IsUint64())
                {
                    punish_detail_req(request);
                    ASKCOIN_RETURN;
                }
            }
            
            if(!data.HasMember("tx_ids"))
            {
                punish_detail_req(request);
                ASKCOIN_RETURN;
            }
            
            const rapidjson::Value &tx_ids = data["tx_ids"];

            if(!tx_ids.IsArray())
            {
                punish_detail_req(request);
                ASKCOIN_RETURN;
            }
            
            uint32 tx_num = tx_ids.Size();
            
            if(tx_num > 2000)
            {
                punish_detail_req(request);
                ASKCOIN_RETURN;
            }

            if(tx_num != tx.Size())
            {
                punish_detail_req(request);
                ASKCOIN_RETURN;
            }
            
            std::shared_ptr<Pending_Chain> owner_chain = request->m_owner_chain;
            std::shared_ptr<Block> parent = m_blocks[pre_hash];
            uint64 parent_block_id = parent->id();
            uint64 parent_utc = parent->utc();
            std::string parent_hash = parent->hash();
            uint32 parent_zero_bits = parent->zero_bits();
            uint64 utc_diff = parent->utc_diff();
            
            if(block_id != parent_block_id + 1)
            {
                punish_detail_req(request);
                ASKCOIN_RETURN;
            }

            if(pre_hash != parent_hash)
            {
                punish_detail_req(request);
                ASKCOIN_RETURN;
            }
            
            if(utc_diff < 15)
            {
                if(zero_bits != parent_zero_bits + 1)
                {
                    punish_detail_req(request);
                    ASKCOIN_RETURN;
                }
            }
            else if(utc_diff > 35)
            {
                if(parent_zero_bits > 1)
                {
                    if(zero_bits != parent_zero_bits - 1)
                    {
                        punish_detail_req(request);
                        ASKCOIN_RETURN;
                    }
                }
                else if(zero_bits != 1)
                {
                    punish_detail_req(request);
                    ASKCOIN_RETURN;
                }
            }
            else if(zero_bits != parent_zero_bits)
            {
                punish_detail_req(request);
                ASKCOIN_RETURN;
            }
        
            if(utc < parent_utc)
            {
                punish_detail_req(request);
                ASKCOIN_RETURN;
            }

            uint64 now = time(NULL);
        
            if(utc > now)
            {
                LOG_ERROR("recv BLOCK_DETAIL_RSP, verify utc failed, id: %lu, hash: %s, please check your system time", \
                          block_id, block_hash.c_str());
                return;
            }
            
            if(!verify_sign(miner_pubkey, block_hash, block_sign))
            {
                punish_peer(peer);
                ASKCOIN_RETURN;
            }
            
            if(!verify_hash(block_hash, data_str, zero_bits))
            {
                punish_detail_req(request);
                LOG_ERROR("recv BLOCK_DETAIL_RSP, verify_hash failed, id: %lu, hash: %s, zero_bits: %u", \
                          block_id, block_hash.c_str(), zero_bits);
                return;
            }
            
            // 695 is the max size of the other fields from the block,
            // refer to max_size_fields in data_structure.example file
            uint32 max_msg_size = 695 + tx_num * 47;
            
            for(uint32 i = 0; i < tx_num; ++i)
            {
                if(!tx_ids[i].IsString())
                {
                    punish_detail_req(request);
                    ASKCOIN_RETURN;
                }
                
                std::string tx_id = tx_ids[i].GetString();
                
                if(tx_id.length() != 44)
                {
                    punish_detail_req(request);
                    ASKCOIN_RETURN;
                }
                
                if(!is_base64_char(tx_id))
                {
                    punish_detail_req(request);
                    ASKCOIN_RETURN;
                }

                if(m_tx_map.find(tx_id) != m_tx_map.end())
                {
                    punish_detail_req(request);
                    ASKCOIN_RETURN;
                }
            
                const rapidjson::Value &tx_node = tx[i];

                if(!tx_node.IsObject())
                {
                    punish_detail_req(request);
                    ASKCOIN_RETURN;
                }
            
                if(!tx_node.HasMember("sign"))
                {
                    punish_detail_req(request);
                    ASKCOIN_RETURN;
                }

                if(!tx_node.HasMember("data"))
                {
                    punish_detail_req(request);
                    ASKCOIN_RETURN;
                }
                
                if(!tx_node["sign"].IsString())
                {
                    punish_detail_req(request);
                    ASKCOIN_RETURN;
                }

                std::string tx_sign = tx_node["sign"].GetString();
                const rapidjson::Value &data = tx_node["data"];

                if(!data.IsObject())
                {
                    punish_detail_req(request);
                    ASKCOIN_RETURN;
                }
                
                if(!is_base64_char(tx_sign))
                {
                    punish_detail_req(request);
                    ASKCOIN_RETURN;
                }
            
                if(!data.HasMember("pubkey"))
                {
                    punish_detail_req(request);
                    ASKCOIN_RETURN;
                }

                if(!data.HasMember("type"))
                {
                    punish_detail_req(request);
                    ASKCOIN_RETURN;
                }
            
                if(!data.HasMember("utc"))
                {
                    punish_detail_req(request);
                    ASKCOIN_RETURN;
                }
            
                rapidjson::StringBuffer buffer;
                rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
                data.Accept(writer);
                std::string tx_id_verify = coin_hash_b64(buffer.GetString(), buffer.GetSize());
            
                if(tx_id != tx_id_verify)
                {
                    punish_detail_req(request);
                    ASKCOIN_RETURN;
                }

                if(!data["pubkey"].IsString())
                {
                    punish_detail_req(request);
                    ASKCOIN_RETURN;
                }
                
                std::string pubkey = data["pubkey"].GetString();
                
                if(pubkey.length() != 88)
                {
                    punish_detail_req(request);
                    ASKCOIN_RETURN;
                }

                if(!is_base64_char(pubkey))
                {
                    punish_detail_req(request);
                    ASKCOIN_RETURN;
                }

                if(!verify_sign(pubkey, tx_id, tx_sign))
                {
                    punish_detail_req(request);
                    ASKCOIN_RETURN;
                }

                if(!data["type"].IsUint())
                {
                    punish_detail_req(request);
                    ASKCOIN_RETURN;
                }

                if(!data["utc"].IsUint64())
                {
                    punish_detail_req(request);
                    ASKCOIN_RETURN;
                }

                uint32 tx_type = data["type"].GetUint();
                uint64 utc = data["utc"].GetUint64();
            }
            
            for(uint32 i = 0; i < tx_num; ++i)
            {
                std::string tx_id = tx_ids[i].GetString();
                const rapidjson::Value &tx_node = tx[i];
                const rapidjson::Value &data = tx_node["data"];
                std::string pubkey = data["pubkey"].GetString();
                uint32 tx_type = data["type"].GetUint();
            }

            std::shared_ptr<Block> cur_block(new Block(block_id, utc, version, zero_bits, block_hash));
            cur_block->set_parent(parent);
            cur_block->set_miner(miner_account);
            parent->add_my_difficulty_to(cur_block);
        
            if(m_blocks.find(block_hash) != m_blocks.end())
            {
                ASKCOIN_RETURN;
            }
        
            m_blocks.insert(std::make_pair(block_hash, cur_block));
        }
        else
        {
            punish_peer(peer);
        }
    }
    else if(type == net::p2p::MSG_TX)
    {
    }
    else if(type == net::p2p::MSG_PROBE)
    {
    }
    else
    {
        punish_peer(peer);
    }
}

void Blockchain::do_brief_chain()
{
    for(auto iter = m_pending_brief_chains.begin(); iter != m_pending_brief_chains.end();)
    {
        std::shared_ptr<Pending_Chain> pending_chain = *iter;
        std::shared_ptr<net::p2p::Peer> peer = pending_chain->m_peer;
        bool continue_if = false;

        if(!m_most_difficult_block->difficult_than_me(pending_chain->m_declared_pow))
        {
            iter = m_pending_brief_chains.erase(iter);
            m_pending_peer_keys.erase(peer->key());

            continue;
        }
        
        while(true)
        {
            std::shared_ptr<Pending_Block> pending_block = pending_chain->m_req_blocks.front();
            std::string pre_hash = pending_block->m_pre_hash;
            auto iter_1 = m_blocks.find(pre_hash);
            
            if(iter_1 != m_blocks.end())
            {
                std::shared_ptr<Block> pre_block = iter_1->second;
                iter = m_pending_brief_chains.erase(iter);
                continue_if = true;

                if(pending_block->m_id != pre_block->id() + 1)
                {
                    punish_peer(peer);
                    m_pending_peer_keys.erase(peer->key());

                    break;
                }
                
                if(!pre_block->difficult_equal(pending_chain->m_remain_pow))
                {
                    punish_peer(peer);
                    m_pending_peer_keys.erase(peer->key());

                    break;
                }
                
                m_brief_chains.push_back(pending_chain);
                
                if(m_is_switching)
                {
                    std::shared_ptr<Pending_Chain> owner_chain = m_detail_request->m_owner_chain;
                    auto pending_block = owner_chain->m_req_blocks[owner_chain->m_start];
                    auto pending_id = pending_block->m_id;
                    auto pending_hash = pending_block->m_hash;
                    auto num = pending_chain->m_req_blocks.size();
                    auto start_id = pending_chain->m_req_blocks[0]->m_id;
                    auto end_id = pending_chain->m_req_blocks[num - 1]->m_id;
        
                    if(pending_id > end_id || pending_id < start_id)
                    {
                        break;
                    }
                    
                    auto idx = pending_id - start_id;
                    
                    if(pending_chain->m_req_blocks[idx]->m_hash != pending_hash)
                    {
                        break;
                    }
                    
                    pending_chain->m_start = idx;
                    pending_chain->m_detail_attached = true;
                    m_detail_request->m_attached_chains.push_back(pending_chain);
                }
                
                break;
            }
            
            // pre_hash(gensis block) should be in m_blocks
            if(pending_block->m_id <= 1)
            {
                punish_peer(peer);
                m_pending_peer_keys.erase(peer->key());
                iter = m_pending_brief_chains.erase(iter);
                continue_if = true;
                
                break;
            }
            
            auto iter_2 = m_pending_blocks.find(pre_hash);
            
            if(iter_2 != m_pending_blocks.end())
            {
                std::shared_ptr<Pending_Block> pre_pending_block = iter_2->second;

                if(pending_block->m_id != pre_pending_block->m_id + 1)
                {
                    punish_peer(peer);
                    m_pending_peer_keys.erase(peer->key());
                    iter = m_pending_brief_chains.erase(iter);
                    continue_if = true;
                    
                    break;
                }
                
                if(!pending_chain->m_remain_pow.sub_pow(pre_pending_block->m_zero_bits))
                {
                    punish_peer(peer);
                    m_pending_peer_keys.erase(peer->key());
                    iter = m_pending_brief_chains.erase(iter);
                    continue_if = true;
                    
                    break;
                }

                pending_chain->m_req_blocks.push_front(pre_pending_block);
                pending_chain->m_brief_attached = false;
            }
            else
            {
                std::shared_ptr<Pending_Brief_Request> request;
                auto iter_3 = m_pending_brief_reqs.find(pre_hash);
                
                if(iter_3 == m_pending_brief_reqs.end())
                {
                    request = std::make_shared<Pending_Brief_Request>();
                    request->m_attached_chains.push_back(pending_chain);
                    request->m_hash = pre_hash;
                    m_pending_brief_reqs.insert(std::make_pair(pre_hash, request));
                    pending_chain->m_brief_attached = true;
                    rapidjson::Document doc;
                    doc.SetObject();
                    rapidjson::Document::AllocatorType &allocator = doc.GetAllocator();
                    doc.AddMember("msg_type", net::p2p::MSG_BLOCK, allocator);
                    doc.AddMember("msg_cmd", net::p2p::BLOCK_BRIEF_REQ, allocator);
                    doc.AddMember("hash", rapidjson::StringRef(pre_hash.c_str()), allocator);
                    request->m_attached_chains.back()->m_peer->m_connection->send(doc);
                    ++request->m_try_num;
                    LOG_DEBUG_INFO("pending_brief_request, id: %lu, hash: %s", pending_block->m_id - 1, pre_hash.c_str());
                    request->m_timer_id = m_timer_ctl.add_timer([=]() {
                            if(request->m_try_num >= request->m_attached_chains.size() * 2)
                            {
                                punish_brief_req(request);
                            }
                            else
                            {
                                auto last_peer = request->m_attached_chains.back()->m_peer;
                                
                                if(last_peer->m_connection->closed())
                                {
                                    request->m_attached_chains.pop_back();

                                    if(request->m_attached_chains.empty())
                                    {
                                        punish_brief_req(request);

                                        return;
                                    }
                                }
                                
                                while(true)
                                {
                                    std::random_shuffle(request->m_attached_chains.begin(), request->m_attached_chains.end());
                                    auto last_peer = request->m_attached_chains.back()->m_peer;
                                
                                    if(last_peer->m_connection->closed())
                                    {
                                        request->m_attached_chains.pop_back();

                                        if(request->m_attached_chains.empty())
                                        {
                                            punish_brief_req(request);

                                            return;
                                        }
                                    }
                                    else
                                    {
                                        break;
                                    }
                                }
                                
                                rapidjson::Document doc;
                                doc.SetObject();
                                rapidjson::Document::AllocatorType &allocator = doc.GetAllocator();
                                doc.AddMember("msg_type", net::p2p::MSG_BLOCK, allocator);
                                doc.AddMember("msg_cmd", net::p2p::BLOCK_BRIEF_REQ, allocator);
                                doc.AddMember("hash", rapidjson::StringRef(pre_hash.c_str()), allocator);
                                request->m_attached_chains.back()->m_peer->m_connection->send(doc);
                                ++request->m_try_num;
                            }
                        }, 1);
                }
                else
                {
                    request = iter_3->second;
                }

                if(!pending_chain->m_brief_attached)
                {
                    request->m_attached_chains.push_back(pending_chain);
                    pending_chain->m_brief_attached = true;
                }
                
                break;
            }
        }
        
        if(continue_if)
        {
            continue;
        }
        else
        {
            ++iter;
        }
    }

    if(m_is_switching)
    {
        ASKCOIN_RETURN;
    }
    
    std::shared_ptr<Pending_Chain> most_difficult_chain = std::make_shared<Pending_Chain>();
    Accum_Pow zero_pow;
    
    for(auto iter = m_brief_chains.begin(); iter != m_brief_chains.end();)
    {
        auto &pending_chain = *iter;

        if(!m_most_difficult_block->difficult_than_me(pending_chain->m_declared_pow))
        {
            iter = m_brief_chains.erase(iter);
            m_pending_peer_keys.erase(pending_chain->m_peer->key());
            
            continue;
        }

        if(pending_chain->m_declared_pow > most_difficult_chain->m_declared_pow)
        {
            most_difficult_chain = pending_chain;
        }

        ++iter;
    }
    
    if(most_difficult_chain->m_declared_pow > zero_pow)
    {
        switch_chain(most_difficult_chain);
    }
}
