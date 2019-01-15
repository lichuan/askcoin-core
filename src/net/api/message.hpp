#ifndef NET__API__MESSAGE
#define NET__API__MESSAGE

namespace net {
namespace api {

enum MSG_TYPE
{
    MSG_SYS,
    MSG_ACCOUNT,
    MSG_TX,
    MSG_BLOCK,
    MSG_TOPIC,
    MSG_EXPLORER,
    MSG_EXCHANGE
};

enum MSG_CMD
{
    SYS_PING,
    SYS_PONG,
    SYS_INFO,

    ACCOUNT_IMPORT = 0,
    ACCOUNT_TOP100,
    ACCOUNT_PROBE,
    ACCOUNT_QUERY,
    ACCOUNT_HISTORY,
    
    TX_CMD = 0,
    
    BLOCK_SYNC = 0,

    TOPIC_QUESTION_PROBE = 0,
    TOPIC_DETAIL_PROBE,
    TOPIC_LIST,
    TOPIC_ANSWER_LIST,

    EXPLORER_MAIN_PAGE = 0,
    EXPLORER_NEXT_PAGE,
    EXPLORER_BLOCK_PAGE,
    EXPLORER_TX_PAGE,
    EXPLORER_ACCOUNT_PAGE,
    EXPLORER_QUERY,

    EXCHANGE_LOGIN = 0,
    EXCHANGE_NOTIFY_DEPOSIT,
    EXCHANGE_DEPOSIT_TX_PROBE,
    EXCHANGE_WITHDRAW_TX_PROBE
};

}
}

#endif
