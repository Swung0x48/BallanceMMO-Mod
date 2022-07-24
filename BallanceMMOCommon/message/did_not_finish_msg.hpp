#ifndef BALLANCEMMO_SERVER_DID_NOT_FINISH_MSG_HPP
#define BALLANCEMMO_SERVER_DID_NOT_FINISH_MSG_HPP
#include "message.hpp"

namespace bmmo {
    struct current_map_state {
        HSteamNetConnection player_id = k_HSteamNetConnection_Invalid;
        uint8_t cheated = false;
        struct map map;
        int32_t sector = 0;
    };

    typedef struct message<current_map_state, DidNotFinish> did_not_finish_msg;
};

#endif // BALLANCEMMO_SERVER_DID_NOT_FINISH_MSG_HPP
