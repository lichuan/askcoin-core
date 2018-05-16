#ifndef TOPIC
#define TOPIC

#include <list>
#include <memory>
#include <unordered_map>
#include "fly/base/common.hpp"
#include "reply.hpp"

class Account;

class Topic
{
public:
    Topic(std::string key, std::string data, uint64 block_id, uint64 balance);
    ~Topic();
    void set_owner(std::shared_ptr<Account> owner);
    std::shared_ptr<Account> get_owner();
    uint64 block_id();    
    std::string key();
    bool get_reply(std::string key, std::shared_ptr<Reply> &reply);
    void add_reply(std::string key, std::shared_ptr<Reply> reply);
    void sub_balance(uint64 value);
    uint64 get_balance();
    std::list<std::shared_ptr<Reply>> m_replie_list;
    
private:
    std::string m_data;
    std::string m_key;
    uint64 m_block_id;
    uint64 m_balance;
    std::shared_ptr<Account> m_owner;
    std::list<std::shared_ptr<Account>> m_replier;
    std::unordered_map<std::string, std::shared_ptr<Reply>> m_replies;
};

#endif
