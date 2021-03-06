#ifndef NET__P2P__PEER_SCORE
#define NET__P2P__PEER_SCORE

#include <atomic>
#include <memory>
#include "fly/net/addr.hpp"

namespace net {
namespace p2p {

class Peer_Score : public std::enable_shared_from_this<Peer_Score>
{
public:
    struct Score_Comp
    {
        bool operator()(const std::shared_ptr<Peer_Score> &a, const std::shared_ptr<Peer_Score> &b)
        {
            return a->m_score > b->m_score;
        }
    };
    
    Peer_Score(const fly::net::Addr &addr, uint64 score = 1000000000);
    ~Peer_Score();
    std::string key() const;
    const fly::net::Addr& addr();
    void add_score(uint64 score);
    void sub_score(uint64 score);
    std::atomic<uint8> m_state{0};
    fly::net::Addr m_addr;
    uint64 m_score;
    
private:
    std::string m_key;
};

}
}

#endif
