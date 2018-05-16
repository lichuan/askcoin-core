#include "reply.hpp"

Reply::Reply(std::string key, uint32 type, std::string data)
{
    m_key = key;
    m_type = type;
    m_data = data;
    m_balance = 0;
}

Reply::~Reply()
{
}

void Reply::set_owner(std::shared_ptr<Account> owner)
{
    m_owner = owner;
}

std::shared_ptr<Account> Reply::get_owner()
{
    return m_owner;
}

void Reply::set_reply_to(std::shared_ptr<Reply> to)
{
    m_to = to;
}

std::shared_ptr<Reply> Reply::get_reply_to()
{
    return m_to;
}

void Reply::add_balance(uint64 value)
{
    m_balance += value;
}

uint64 Reply::get_balance()
{
    return m_balance;
}
