#ifndef BALLANCEMMOSERVER_COUNTDOWN_HPP
#define BALLANCEMMOSERVER_COUNTDOWN_HPP
#include "message.hpp"
#include <cstdint>

namespace bmmo {
    enum countdown_type: uint8_t {
        CountdownType_Go,
        CountdownType_1,
        CountdownType_2,
        CountdownType_3,
        CountdownType_Unknown = 255
    };

    struct countdown_msg: public serializable_message {
        countdown_msg(): serializable_message(bmmo::Countdown) {}

        countdown_type type = CountdownType_Unknown;
        HSteamNetConnection sender = k_HSteamNetConnection_Invalid;
        struct map map;
        uint8_t restart_level = 0;
        uint8_t force_restart = 0;

        bool serialize() override {
            if (!serializable_message::serialize()) return false;
            raw.write(reinterpret_cast<const char*>(&type), sizeof(type));
            raw.write(reinterpret_cast<const char*>(&sender), sizeof(sender));
            map.serialize(raw);
            raw.write(reinterpret_cast<const char*>(&restart_level), sizeof(restart_level));
            raw.write(reinterpret_cast<const char*>(&force_restart), sizeof(force_restart));
            return (raw.good());
        }

        bool deserialize() override {
            if (!serializable_message::deserialize())
                return false;

            raw.read(reinterpret_cast<char*>(&type), sizeof(type));
            if (!raw.good() || raw.gcount() != sizeof(type)) return false;

            raw.read(reinterpret_cast<char*>(&sender), sizeof(sender));
            if (!raw.good() || raw.gcount() != sizeof(sender)) return false;

            if (!map.deserialize(raw)) return false;
            raw.read(reinterpret_cast<char*>(&restart_level), sizeof(restart_level));
            if (!raw.good() || raw.gcount() != sizeof(restart_level)) return false;
            raw.read(reinterpret_cast<char*>(&force_restart), sizeof(force_restart));
            if (!raw.good() || raw.gcount() != sizeof(force_restart)) return false;
            return (raw.good());
        }
    };
}

#endif //BALLANCEMMOSERVER_COUNTDOWN_HPP