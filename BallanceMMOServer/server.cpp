#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
#include <steam/steam_api.h>
#endif

#include "../BallanceMMOCommon/role/role.hpp"
#include <vector>
#include <unordered_set>
#include <condition_variable>
#include "../BallanceMMOCommon/common.hpp"
#include "console.hpp"

// #include <iomanip>
#include <mutex>
// #include <shared_mutex>
#include <fstream>

#include <ya_getopt.h>
#include <yaml-cpp/yaml.h>

struct client_data {
    std::string name;
    bool cheated = false;
    bool state_updated = true;
    bool timestamp_updated = true;
    bool ready = false;
    uint8_t uuid[16]{};
    bmmo::timed_ball_state state{};
    bmmo::map current_map{};
    int32_t current_sector = 0;
};

struct map_data {
    int rank = 0;
    SteamNetworkingMicroseconds start_time = 0;
};

class server: public role {
public:
    explicit server(uint16_t port) {
        port_ = port;
    }

    void run() override {
        while (running_) {
            auto update_begin = std::chrono::steady_clock::now();
            update();
            if (ticking_) {
                std::this_thread::sleep_until(update_begin + TICK_DELAY);
                tick();
            }
            std::this_thread::sleep_until(update_begin + UPDATE_INTERVAL);
        }

//        while (running_) {
//            poll_local_state_changes();
//        }
    }

    void poll_local_state_changes() override {
        std::string cmd;
        std::cin >> cmd;
        if (cmd == "stop") {
            shutdown();
        }
    }

    EResult send(const HSteamNetConnection destination, void* buffer, size_t size, int send_flags, int64* out_message_number = nullptr) {
        return interface_->SendMessageToConnection(destination,
                                                   buffer,
                                                   size,
                                                   send_flags,
                                                   out_message_number);

    }

    template<bmmo::trivially_copyable_msg T>
    EResult send(const HSteamNetConnection destination, T msg, int send_flags, int64* out_message_number = nullptr) {
        static_assert(std::is_trivially_copyable<T>());
        return send(destination,
                    &msg,
                    sizeof(msg),
                    send_flags,
                    out_message_number);
    }

    void broadcast_message(void* buffer, size_t size, int send_flags, const HSteamNetConnection ignored_client = k_HSteamNetConnection_Invalid) {
        for (auto& i: clients_)
            if (ignored_client != i.first)
                send(i.first, buffer, size,
                                                    send_flags,
                                                    nullptr);
    }

    template<bmmo::trivially_copyable_msg T>
    void broadcast_message(T msg, int send_flags, const HSteamNetConnection ignored_client = k_HSteamNetConnection_Invalid) {
        static_assert(std::is_trivially_copyable<T>());

        broadcast_message(&msg, sizeof(msg), send_flags, ignored_client);
    }

    void receive(void* data, size_t size, HSteamNetConnection client = k_HSteamNetConnection_Invalid) {
        if (get_client_count() == 0) { Printf("Error: no online players found."); return; }
        auto* networking_msg = SteamNetworkingUtils()->AllocateMessage(0);
        if (client == k_HSteamNetConnection_Invalid) // random player
            client = std::next(clients_.begin(), rand() % clients_.size())->first;
        networking_msg->m_conn = client;
        networking_msg->m_pData = data;
        networking_msg->m_cbSize = size;
        on_message(networking_msg);
        networking_msg->Release();
    }

    auto& get_bulletin() { return permanent_notification_; }

    HSteamNetConnection get_client_id(std::string username, bool suppress_error = false) {
        auto username_it = username_.find(bmmo::message_utils::to_lower(username));
        if (username_it == username_.end()) {
            if (!suppress_error)
                Printf("Error: client \"%s\" not found.", username);
            return k_HSteamNetConnection_Invalid;
        }
        return username_it->second;
    };

    inline int get_client_count() const noexcept { return clients_.size(); }

    bool kick_client(HSteamNetConnection client, std::string reason = "", HSteamNetConnection executor = k_HSteamNetConnection_Invalid, bmmo::crash_type crash = bmmo::crash_type::None) {
        if (!client_exists(client))
            return false;
        bmmo::player_kicked_msg msg{};
        msg.kicked_player_name = clients_[client].name;

        std::string kick_notice = "Kicked by ";
        if (executor != k_HSteamNetConnection_Invalid) {
            if (!client_exists(executor))
                return false;
            kick_notice += clients_[executor].name;
            msg.executor_name = clients_[executor].name;
        } else {
            kick_notice += "the server";
        }

        if (crash == bmmo::crash_type::FatalError) {
            // Send a message without serialization; this effectively kills the client
            // at deserialization. A dirty hack, but it works and we don't need to
            // worry about the outcome; the client will just terminate immediately.
            send(client, nullptr, 0, k_nSteamNetworkingSend_Reliable);
            reason = "fatal error";
        }

        if (!reason.empty()) {
            kick_notice.append(" (" + reason + ")");
            msg.reason = reason;
        }
        kick_notice.append(".");

        msg.crashed = (crash >= bmmo::crash_type::Crash);

        interface_->CloseConnection(client, k_ESteamNetConnectionEnd_App_Min + static_cast<int>(crash), kick_notice.c_str(), true);
        msg.serialize();
        broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);

        return true;
    }

    bool load_config() {
        std::string logging_level_string = "important";
        std::ifstream ifile("config.yml");
        if (ifile.is_open() && ifile.peek() != std::ifstream::traits_type::eof()) {
            try {
                config_ = YAML::Load(ifile);
                if (config_["op_list"])
                    op_players_ = config_["op_list"].as<std::unordered_map<std::string, std::string>>();
                if (config_["enable_op_privileges"])
                    op_mode_ = config_["enable_op_privileges"].as<bool>();
                if (config_["ban_list"]) {
                    banned_players_ = config_["ban_list"].as<std::unordered_map<std::string, std::string>>();
                }
                if (config_["mute_list"]) {
                    auto muted_vector = config_["mute_list"].as<std::vector<std::string>>();
                    muted_players_ = std::unordered_set(muted_vector.begin(), muted_vector.end());
                }
                if (config_["restart_level_after_countdown"])
                    restart_level_ = config_["restart_level_after_countdown"].as<bool>();
                if (config_["force_restart_after_countdown"])
                    force_restart_level_ = config_["force_restart_after_countdown"].as<bool>();
                if (config_["logging_level"])
                    logging_level_string = config_["logging_level"].as<std::string>();
            } catch (const std::exception& e) {
                Printf("Error: failed to parse config: %s", e.what());
                return false;
            }
        } else {
            Printf("Config is empty. Generating default config...");
            op_players_ = {{"example_player", "00000001-0002-0003-0004-000000000005"}};
            banned_players_ = {{"00000001-0002-0003-0004-000000000005", "You're banned from this server."}};
            muted_players_ = {"00000001-0002-0003-0004-000000000005"};
        }

        config_["enable_op_privileges"] = op_mode_;
        config_["restart_level_after_countdown"] = restart_level_;
        config_["force_restart_after_countdown"] = force_restart_level_;
        if (logging_level_string == "msg")
            logging_level_ = k_ESteamNetworkingSocketsDebugOutputType_Msg;
        else if (logging_level_string == "warning")
            logging_level_ = k_ESteamNetworkingSocketsDebugOutputType_Warning;
        else {
            logging_level_ = k_ESteamNetworkingSocketsDebugOutputType_Important;
            config_["logging_level"] = "important";
        }
        set_logging_level(logging_level_);

        ifile.close();
        save_config_to_file();
        Printf("Config loaded successfully.");
        return true;
    }

    void print_clients(bool print_uuid = false) {
        std::map<decltype(username_)::key_type, decltype(username_)::mapped_type> spectators;
        static const auto print_client = [&](auto id, auto data) {
            Printf("%u: %s%s%s%s",
                    id, data.name,
                    print_uuid ? (" (" + get_uuid_string(data.uuid) + ")") : "",
                    data.cheated ? " [CHEAT]" : "", is_op(id) ? " [OP]" : "");
        };
        for (const auto& i: std::map(username_.begin(), username_.end())) {
            if (bmmo::name_validator::is_spectator(i.first))
                spectators.insert(i);
            else
                print_client(i.second, clients_[i.second]);
        }
        for (const auto& i: spectators)
            print_client(i.second, clients_[i.second]);
        Printf("%d client(s) online: %d player(s), %d spectator(s).",
            clients_.size(), clients_.size() - spectators.size(), spectators.size());
    }

    void print_maps() {
        std::multimap<decltype(map_names_)::mapped_type, decltype(map_names_)::key_type> map_names_inverted;
        for (const auto& [hash, name]: map_names_) map_names_inverted.emplace(name, hash);
        for (const auto& [name, hash]: map_names_inverted) {
            std::string hash_string;
            bmmo::string_from_hex_chars(hash_string, reinterpret_cast<const uint8_t*>(hash.c_str()), sizeof(bmmo::map::md5));
            Printf("%s: %s", hash_string, name);
        }
    }

    void print_player_maps() {
        for (const auto& [id, data]: clients_)
            Printf("%s(#%u, %s) is at the %d%s sector of %s.",
                data.cheated ? "[CHEAT] " : "", id, data.name,
                data.current_sector, bmmo::get_ordinal_rank(data.current_sector),
                data.current_map.get_display_name(map_names_));
    }

    void print_positions() {
        for (const auto& [id, data]: clients_) {
            std::string type = std::unordered_map<int, std::string>{{0, "paper"}, {1, "stone"}, {2, "wood"}}[data.state.type];
            if (type.empty()) type = "unknown (id #" + std::to_string(data.state.type) + ")";
            Printf("(%u, %s) is at %.2f, %.2f, %.2f with %s ball.",
                    id, data.name,
                    data.state.position.x, data.state.position.y, data.state.position.z,
                    type
            );
        }
    }

    void print_version_info() {
        Printf("Server version: %s; minimum accepted client version: %s.",
                        bmmo::version_t{}.to_string(),
                        bmmo::minimum_client_version.to_string());
        auto uptime = SteamNetworkingUtils()->GetLocalTimestamp() - init_timestamp_;
        std::string time_str(20, 0);
        time_str.resize(std::strftime(&time_str[0], time_str.size(),
            "%F %T", std::localtime(&init_time_t_)));
        Printf("Server uptime: %.2f seconds since %s.",
                        uptime * 1e-6, time_str);
    }

    inline void pull_ball_states(std::vector<bmmo::owned_timed_ball_state>& balls) {
        for (auto& i: clients_) {
            std::unique_lock<std::mutex> lock(client_data_mutex_);
            if (i.second.state.timestamp.is_zero())
                continue;
            balls.push_back({i.second.state, i.first});
        }
    }

    inline void pull_unupdated_ball_states(std::vector<bmmo::owned_timed_ball_state>& balls, std::vector<bmmo::owned_timestamp>& unchanged_balls) {
        for (auto& i: clients_) {
            std::unique_lock<std::mutex> lock(client_data_mutex_);
            if (!i.second.state_updated) {
                balls.push_back({i.second.state, i.first});
                i.second.state_updated = true;
            }
            if (!i.second.timestamp_updated) {
                unchanged_balls.push_back({i.second.state.timestamp, i.first});
                i.second.timestamp_updated = true;
            }
        }
    }

    void set_ban(HSteamNetConnection client, const std::string& reason) {
        if (!client_exists(client))
            return;
        banned_players_[get_uuid_string(clients_[client].uuid)] = reason;
        Printf("Banned %s%s.", clients_[client].name, reason.empty() ? "" : ": " + reason);
        save_config_to_file();
    }

    void set_mute(HSteamNetConnection client, bool action) {
        if (!client_exists(client))
            return;
        if (action) {
            Printf("Muted %s%s.", clients_[client].name,
                muted_players_.insert(get_uuid_string(clients_[client].uuid)).second
                ? "" : " (client was already muted previously)");
        } else {
            Printf("Unmuted %s%s.", clients_[client].name,
                muted_players_.erase(get_uuid_string(clients_[client].uuid)) != 0
                ? "" : " (client was already unmuted previously)");
        }
        save_config_to_file();
    }

    void set_op(HSteamNetConnection client, bool action) {
        if (!client_exists(client))
            return;
        std::string name = bmmo::name_validator::get_real_nickname(clients_[client].name);
        if (action) {
            if (auto it = op_players_.find(name); it != op_players_.end()) {
                if (it->second == get_uuid_string(clients_[client].uuid)) {
                    Printf("Error: client \"%s\" already has OP privileges.", name);
                    return;
                }
            }
            op_players_[name] = get_uuid_string(clients_[client].uuid);
            Printf("%s is now an operator.", name);
        } else {
            if (!op_players_.erase(name))
                return;
            Printf("%s is no longer an operator.", name);
        }
        save_config_to_file();
        bmmo::op_state_msg msg{};
        msg.content.op = action;
        send(client, msg, k_nSteamNetworkingSend_Reliable);
    }

    void toggle_cheat(bool cheat) {
        bmmo::cheat_toggle_msg msg;
        msg.content.cheated = (uint8_t)cheat;
        broadcast_message(msg, k_nSteamNetworkingSend_Reliable);
    }

    void shutdown(int reconnection_delay = 0) {
        Printf("Shutting down...");
        int nReason = reconnection_delay == 0 ? 0 : k_ESteamNetConnectionEnd_App_Min + 150 + reconnection_delay;
        for (auto& i: clients_) {
            interface_->CloseConnection(i.first, nReason, "Server closed", true);
        }
        if (ticking_)
            stop_ticking();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        running_ = false;
//        if (server_thread_.joinable())
//            server_thread_.join();
    }

    void wait_till_started() {
        while (!running()) {
            std::unique_lock<std::mutex> lk(startup_mutex_);
            startup_cv_.wait(lk);
        }
    }

    bool setup() override {
        Printf("Loading config from config.yml...");
        if (!load_config()) {
            Printf("Error: failed to load config. Please try fixing or emptying it first.");
            return false;
        }

        SteamNetworkingIPAddr local_address{};
        local_address.Clear();
        local_address.m_port = port_;
        SteamNetworkingConfigValue_t opt = generate_opt();
        listen_socket_ = interface_->CreateListenSocketIP(local_address, 1, &opt);
        if (listen_socket_ == k_HSteamListenSocket_Invalid) {
            return false;
        }

        poll_group_ = interface_->CreatePollGroup();
        if (poll_group_ == k_HSteamNetPollGroup_Invalid) {
            return false;
        }

        running_ = true;
        startup_cv_.notify_all();

        Printf("Server (v%s; client min. v%s) started at port %u.\n",
                bmmo::version_t{}.to_string(),
                bmmo::minimum_client_version.to_string(), port_);

        return true;
    }

protected:
    void save_login_data(HSteamNetConnection client) {
        SteamNetConnectionInfo_t pInfo;
        interface_->GetConnectionInfo(client, &pInfo);
        SteamNetworkingIPAddr ip = pInfo.m_addrRemote;
        char ip_str[SteamNetworkingIPAddr::k_cchMaxString]{};
        std::string uuid_str = get_uuid_string(clients_[client].uuid),
                    name = clients_[client].name;
        ip.ToString(ip_str, SteamNetworkingIPAddr::k_cchMaxString, false);
        YAML::Node login_data;
        std::ifstream ifile("login_data.yml");
        if (ifile.is_open() && ifile.peek() != std::ifstream::traits_type::eof()) {
            try {
                login_data = YAML::Load(ifile);
            } catch (const std::exception& e) {
                Printf("Error: failed to parse config: %s", e.what());
                return;
            }
        }
        ifile.close();
        auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::string time_str(20, 0);
        time_str.resize(std::strftime(&time_str[0], time_str.size(),
            "%F %X", std::localtime(&time)));
        if (!login_data[uuid_str])
            login_data[uuid_str] = YAML::Node(YAML::NodeType::Map);
        if (!login_data[uuid_str][name])
            login_data[uuid_str][name] = YAML::Node(YAML::NodeType::Map);
        login_data[uuid_str][name][time_str] = ip_str;
        std::ofstream ofile("login_data.yml");
        if (ofile.is_open()) {
            ofile << login_data;
            ofile.close();
        }
    }

    void save_config_to_file() {
        config_["op_list"] = op_players_;
        config_["ban_list"] = banned_players_;
        config_["mute_list"] = std::vector<std::string>(muted_players_.begin(), muted_players_.end());
        std::ofstream config_file("config.yml");
        if (!config_file.is_open()) {
            Printf("Error: failed to open config file for writing.");
            return;
        }
        auto current_time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        config_file << "# Config file for Ballance MMO Server v" << bmmo::version_t{}.to_string() << " - "
                        << std::put_time(std::localtime(&current_time), "%F %T") << "\n"
                    << "# Notes:\n"
                    << "# - Op list player data style: \"playername: uuid\".\n"
                    << "# - Ban list style: \"uuid: reason\".\n"
                    << "# - Mute list style: \"- uuid\".\n"
                    << "# - Level restart: whether to restart on clients' sides after \"Go!\". If not forced, only for clients on the same map.\n"
                    << "# - Options for log levels: important, warning, msg.\n"
                    << std::endl;
        config_file << config_;
        config_file << std::endl;
        config_file.close();
    }

    // Fail silently if the client doesn't exist.
    void cleanup_disconnected_client(HSteamNetConnection client) {
        // Locate the client.  Note that it should have been found, because this
        // is the only codepath where we remove clients (except on shutdown),
        // and connection change callbacks are dispatched in queue order.
        auto itClient = clients_.find(client);
        //assert(itClient != clients_.end()); // It might in limbo state...So may not yet to be found
        if (itClient == clients_.end())
            return;

        bmmo::player_disconnected_msg msg;
        msg.content.connection_id = client;
        broadcast_message(msg, k_nSteamNetworkingSend_Reliable, client);
        std::string name = itClient->second.name;
        username_.erase(bmmo::message_utils::to_lower(name));
        clients_.erase(itClient);
        Printf("%s (#%u) disconnected.", name, client);
        
        switch (get_client_count()) {
            case 0:
                maps_.clear();
                map_names_.clear();
                permanent_notification_ = {};
                [[fallthrough]];
            case 1:
                if (ticking_)
                    stop_ticking();
                break;
            default:
                break;
        }
    }

    bool client_exists(HSteamNetConnection client, bool suppress_error = false) {
        if (client == k_HSteamNetConnection_Invalid)
            return false;
        if (!clients_.contains(client)) {
            if (!suppress_error)
                Printf("Error: client #%u not found.", client);
            return false;
        }
        return true;
    }

    bool deny_action(HSteamNetConnection client) {
        if (op_mode_ && op_online() && !is_op(client)) {
            bmmo::action_denied_msg denied_msg{.content = {bmmo::deny_reason::NoPermission}};
            send(client, denied_msg, k_nSteamNetworkingSend_Reliable);
            return true;
        }
        return false;
    }

    std::string get_uuid_string(uint8_t* uuid) {
        // std::stringstream ss;
        // for (int i = 0; i < 16; i++) {
        //     ss << std::hex << std::setfill('0') << std::setw(2) << (int)uuid[i];
        //     if (i == 3 || i == 5 || i == 7 || i == 9)
        //         ss << '-';
        // }
        // return ss.str();
        char str[37] = {};
        sprintf(str,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7],
            uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]
        );
        return {str};
    }

    bool is_op(HSteamNetConnection client) {
        if (!client_exists(client))
            return false;
        std::string name = bmmo::name_validator::get_real_nickname(clients_[client].name);
        auto op_it = op_players_.find(name);
        if (op_it == op_players_.end())
            return false;
        if (op_it->second == get_uuid_string(clients_[client].uuid))
            return true;
        return false;
    }

    bool op_online() {
        for (auto& client : clients_) {
            if (is_op(client.first))
                return true;
        }
        return false;
    }

    // nReason:
    // - 0~99: denied at joining
    // -- 0~49: denied from incorrect configuration
    // -- 50~99: banned
    // - 100~199: kicked when online
    // -- 150+n: auto reconnect in n seconds
    bool validate_client(HSteamNetConnection client, bmmo::login_request_v3_msg& msg) {
        int nReason = k_ESteamNetConnectionEnd_Invalid;
        std::stringstream reason;
        std::string real_nickname = bmmo::name_validator::get_real_nickname(msg.nickname);

        // check if client is banned
        if (auto it = banned_players_.find(get_uuid_string(msg.uuid)); it != banned_players_.end()) {
            reason << "You are banned from this server";
            if (!it->second.empty())
                reason << ": " << it->second;
            reason << ".";
            nReason = k_ESteamNetConnectionEnd_App_Min + 50;
        }
        // verify client version
        else if (msg.version < bmmo::minimum_client_version) {
            reason << "Outdated client (client: " << msg.version.to_string()
                    << "; minimum: " << bmmo::minimum_client_version.to_string() << ").";
            nReason = k_ESteamNetConnectionEnd_App_Min + 1;
        }
        // check if name exists
        else if (username_.contains(bmmo::string_utils::to_lower(msg.nickname))) {
            reason << "A player with the same username \"" << msg.nickname << "\" already exists on this serer.";
            nReason = k_ESteamNetConnectionEnd_App_Min + 2;
        }
        // validate nickname length
        else if (!bmmo::name_validator::is_of_valid_length(real_nickname)) {
            reason << "Nickname must be between "
                    << bmmo::name_validator::min_length << " and "
                    << bmmo::name_validator::max_length << " characters in length.";
            nReason = k_ESteamNetConnectionEnd_App_Min + 3;
        }
        // validate nickname characters
        else if (size_t invalid_pos = bmmo::name_validator::get_invalid_char_pos(real_nickname);
                invalid_pos != std::string::npos) {
            reason << "Invalid character '" << real_nickname[invalid_pos] << "' at position "
                    << invalid_pos << "; nicknames can only contain alphanumeric characters and underscores.";
            nReason = k_ESteamNetConnectionEnd_App_Min + 4;
        }

        if (nReason != k_ESteamNetConnectionEnd_Invalid) {
            bmmo::simple_action_msg new_msg{.content = {bmmo::action_type::LoginDenied}};
            send(client, new_msg, k_nSteamNetworkingSend_Reliable);
            interface_->CloseConnection(client, nReason, reason.str().c_str(), true);
            return false;
        }

        return true;
    }

    void on_connection_status_changed(SteamNetConnectionStatusChangedCallback_t* pInfo) override {
        // Printf("Connection status changed: %d", pInfo->m_info.m_eState);
        switch (pInfo->m_info.m_eState) {
            case k_ESteamNetworkingConnectionState_None:
                // NOTE: We will get callbacks here when we destroy connections.  You can ignore these.
            case k_ESteamNetworkingConnectionState_ClosedByPeer:
            case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
                // Ignore if they were not previously connected.  (If they disconnected
                // before we accepted the connection.)
                if (pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connected) {
                    // Select appropriate log messages
                    if (logging_level_ < k_ESteamNetworkingSocketsDebugOutputType_Msg) {
                        const char* pszDebugLogAction;
                        if (pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
                            pszDebugLogAction = "problem detected locally";
                        } else if (pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer) {
                            // Note that here we could check the reason code to see if
                            // it was a "usual" connection or an "unusual" one.
                            pszDebugLogAction = "closed by peer";
                        } else {
                            pszDebugLogAction = "closed by app";
                        }

                        // Spew something to our own log.  Note that because we put their nick
                        // as the connection description, it will show up, along with their
                        // transport-specific data (e.g. their IP address)
                        Printf( "[%s] %s (%d): %s\n",
                                pInfo->m_info.m_szConnectionDescription,
                                pszDebugLogAction,
                                pInfo->m_info.m_eEndReason,
                                pInfo->m_info.m_szEndDebug
                        );
                    }

                    cleanup_disconnected_client(pInfo->m_hConn);
                } else {
                    assert(pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connecting
                           || pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_None);
                }

                // Clean up the connection.  This is important!
                // The connection is "closed" in the network sense, but
                // it has not been destroyed.  We must close it on our end, too
                // to finish up.  The reason information do not matter in this case,
                // and we cannot linger because it's already closed on the other end,
                // so we just pass 0's.

                interface_->CloseConnection(pInfo->m_hConn, 0, nullptr, false);

                break;
            }

            case k_ESteamNetworkingConnectionState_Connecting: {
                // This must be a new connection
                assert(clients_.find(pInfo->m_hConn) == clients_.end());

                Printf("Connection request from %s\n", pInfo->m_info.m_szConnectionDescription);

                // A client is attempting to connect
                // Try to accept the connection.
                if (interface_->AcceptConnection(pInfo->m_hConn) != k_EResultOK) {
                    // This could fail.  If the remote host tried to connect, but then
                    // disconnected, the connection may already be half closed.  Just
                    // destroy whatever we have on our side.
                    interface_->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
                    Printf("Can't accept connection.  (It was already closed?)\n");
                    break;
                }

                // Assign the poll group
                if (!interface_->SetConnectionPollGroup(pInfo->m_hConn, poll_group_)) {
                    interface_->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
                    Printf("Failed to set poll group?");
                    break;
                }

                // Generate a random nick.  A random temporary nick
                // is really dumb and not how you would write a real chat server.
                // You would want them to have some sort of signon message,
                // and you would keep their client in a state of limbo (connected,
                // but not logged on) until them.  I'm trying to keep this example
                // code really simple.
                char nick[32];
                sprintf(nick, "Unidentified%05d", rand() % 100000);

                // DO NOT add client here.
                //clients_[pInfo->m_hConn] = {nick};
                interface_->SetConnectionName(pInfo->m_hConn, nick);
//                SetClientNick( pInfo->m_hConn, nick );
                break;
            }

            case k_ESteamNetworkingConnectionState_Connected:
                // We will get a callback immediately after accepting the connection.
                // Since we are the server, we can ignore this, it's not news to us.
                break;

            default:
                // Silences -Wswitch
                break;
        }
    }

    void on_message(ISteamNetworkingMessage* networking_msg) override {
        auto client_it = clients_.find(networking_msg->m_conn);

        auto* raw_msg = reinterpret_cast<bmmo::general_message*>(networking_msg->m_pData);
        if (!(client_it != clients_.end() || raw_msg->code == bmmo::LoginRequest || raw_msg->code == bmmo::LoginRequestV2 || raw_msg->code == bmmo::LoginRequestV3)) { // ignore limbo clients message
            interface_->CloseConnection(networking_msg->m_conn, k_ESteamNetConnectionEnd_AppException_Min, "Invalid client", true);
            return;
        }

        switch (raw_msg->code) {
            case bmmo::LoginRequest: {
                bmmo::simple_action_msg msg{.content = {bmmo::action_type::LoginDenied}};
                send(networking_msg->m_conn, msg, k_nSteamNetworkingSend_Reliable);
                interface_->CloseConnection(networking_msg->m_conn, k_ESteamNetConnectionEnd_App_Min + 1, "Outdated client", true);
                break;
            }
            case bmmo::LoginRequestV2: {
                bmmo::login_request_v2_msg msg;
                msg.raw.write(static_cast<const char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();

                interface_->SetConnectionName(networking_msg->m_conn, msg.nickname.c_str());

                std::string reason = "Outdated client (client: " + msg.version.to_string()
                        + "; minimum: " + bmmo::minimum_client_version.to_string() + ")";
                bmmo::simple_action_msg new_msg{.content = {bmmo::action_type::LoginDenied}};
                send(networking_msg->m_conn, new_msg, k_nSteamNetworkingSend_Reliable);
                interface_->CloseConnection(networking_msg->m_conn, k_ESteamNetConnectionEnd_App_Min + 1, reason.c_str(), true);
                break;
            }
            case bmmo::LoginRequestV3: {
                bmmo::login_request_v3_msg msg;
                msg.raw.write(static_cast<const char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();

                interface_->SetConnectionName(networking_msg->m_conn, msg.nickname.c_str());

                if (!validate_client(networking_msg->m_conn, msg))
                    break;

                // accepting client
                clients_.insert({networking_msg->m_conn, {msg.nickname, (bool)msg.cheated}});  // add the client here
                memcpy(clients_[networking_msg->m_conn].uuid, msg.uuid, sizeof(msg.uuid));
                username_[bmmo::message_utils::to_lower(msg.nickname)] = networking_msg->m_conn;
                Printf("%s (%s; v%s) logged in with cheat mode %s!\n",
                        msg.nickname,
                        get_uuid_string(msg.uuid).substr(0, 8),
                        msg.version.to_string(),
                        msg.cheated ? "on" : "off");
                
                if (!map_names_.empty()) { // do this before login_accepted_msg since the latter contains map info
                    bmmo::map_names_msg name_msg;
                    name_msg.maps = map_names_;
                    name_msg.serialize();
                    send(networking_msg->m_conn, name_msg.raw.str().data(), name_msg.size(), k_nSteamNetworkingSend_Reliable);
                }

                // notify this client of other online players
                bmmo::login_accepted_v3_msg accepted_msg;
                for (const auto& [id, data]: clients_) {
                    //if (client_it != it)
                    accepted_msg.online_players.reserve(clients_.size());
                    accepted_msg.online_players.insert({id, {data.name, data.cheated, data.current_map, data.current_sector}});
                }
                accepted_msg.serialize();
                send(networking_msg->m_conn, accepted_msg.raw.str().data(), accepted_msg.size(), k_nSteamNetworkingSend_Reliable);

                save_login_data(networking_msg->m_conn);

                // notify other client of the fact that this client goes online
                bmmo::player_connected_v2_msg connected_msg;
                connected_msg.connection_id = networking_msg->m_conn;
                connected_msg.name = msg.nickname;
                connected_msg.cheated = msg.cheated;
                connected_msg.serialize();
                broadcast_message(connected_msg.raw.str().data(), connected_msg.size(), k_nSteamNetworkingSend_Reliable, networking_msg->m_conn);

                bmmo::owned_timed_ball_state_msg state_msg{};
                pull_ball_states(state_msg.balls);
                state_msg.serialize();
                send(networking_msg->m_conn, state_msg.raw.str().data(), state_msg.size(), k_nSteamNetworkingSend_Reliable);

                if (!permanent_notification_.second.empty()) {
                    bmmo::permanent_notification_msg bulletin_msg{};
                    std::tie(bulletin_msg.title, bulletin_msg.text_content) = permanent_notification_;
                    bulletin_msg.serialize();
                    send(networking_msg->m_conn, bulletin_msg.raw.str().data(), bulletin_msg.size(), k_nSteamNetworkingSend_Reliable);
                }

                if (!ticking_ && get_client_count() > 1)
                    start_ticking();

                break;
            }
            case bmmo::LoginAccepted:
                break;
            case bmmo::PlayerDisconnected:
                break;
            case bmmo::PlayerConnected:
                break;
            case bmmo::Ping:
                break;
            case bmmo::BallState: {
                auto* state_msg = reinterpret_cast<bmmo::ball_state_msg*>(networking_msg->m_pData);

                std::unique_lock<std::mutex> lock(client_data_mutex_);
                clients_[networking_msg->m_conn].state = {state_msg->content, SteamNetworkingUtils()->GetLocalTimestamp()};
                client_it->second.state_updated = false;

                // Printf("%u: %d, (%f, %f, %f), (%f, %f, %f, %f)",
                //        networking_msg->m_conn,
                //        state_msg->content.type,
                //        state_msg->content.position.x,
                //        state_msg->content.position.y,
                //        state_msg->content.position.z,
                //        state_msg->content.rotation.x,
                //        state_msg->content.rotation.y,
                //        state_msg->content.rotation.z,
                //        state_msg->content.rotation.w);
//                std::cout << "(" <<
//                          state_msg->state.position.x << ", " <<
//                          state_msg->state.position.y << ", " <<
//                          state_msg->state.position.z << "), (" <<
//                          state_msg->state.quaternion.x << ", " <<
//                          state_msg->state.quaternion.y << ", " <<
//                          state_msg->state.quaternion.z << ", " <<
//                          state_msg->state.quaternion.w << ")" << std::endl;

//                 bmmo::owned_ball_state_msg new_msg;
//                 new_msg.content.state = state_msg->content;
// //                std::memcpy(&(new_msg.content), &(state_msg->content), sizeof(state_msg->content));
//                 new_msg.content.player_id = networking_msg->m_conn;
//                 broadcast_message(&new_msg, sizeof(new_msg), k_nSteamNetworkingSend_UnreliableNoDelay, &networking_msg->m_conn);

                break;
            }
            case bmmo::TimedBallState: {
                auto* state_msg = reinterpret_cast<bmmo::timed_ball_state_msg*>(networking_msg->m_pData);
                std::unique_lock<std::mutex> lock(client_data_mutex_);
                if (state_msg->content.timestamp < client_it->second.state.timestamp)
                    break;
                client_it->second.state = state_msg->content;
                client_it->second.state_updated = false;
                break;
            }
            case bmmo::Timestamp: {
                auto* timestamp_msg = reinterpret_cast<bmmo::timestamp_msg*>(networking_msg->m_pData);
                std::unique_lock<std::mutex> lock(client_data_mutex_);
                if (timestamp_msg->content < client_it->second.state.timestamp)
                    break;
                client_it->second.state.timestamp = timestamp_msg->content;
                client_it->second.timestamp_updated = false;
                break;
            }
            case bmmo::Chat: {
                bmmo::chat_msg msg{};
                msg.raw.write(static_cast<const char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();

                // sanitize chat message (remove control characters)
                std::replace_if(msg.chat_content.begin(), msg.chat_content.end(),
                    [](char c) { return std::iscntrl(c); }, ' ');

                bool muted = muted_players_.contains(get_uuid_string(client_it->second.uuid));

                // Print chat message to console
                const std::string& current_player_name = client_it->second.name;
                const HSteamNetConnection current_player_id  = networking_msg->m_conn;
                Printf("%s(%u, %s): %s", muted ? "[Muted] " : "", current_player_id, current_player_name, msg.chat_content);

                if (muted) {
                    bmmo::action_denied_msg msg{.content = {bmmo::deny_reason::PlayerMuted}};
                    send(networking_msg->m_conn, msg, k_nSteamNetworkingSend_Reliable);
                    break;
                }

                // Broatcast chat message to other player
                msg.player_id = current_player_id;
                msg.clear();
                msg.serialize();

                // No need to ignore the sender, 'cause we will send the message back
                broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);

                break;
            }
            case bmmo::PrivateChat: {
                bmmo::private_chat_msg msg{};
                msg.raw.write(static_cast<const char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();

                std::replace_if(msg.chat_content.begin(), msg.chat_content.end(),
                    [](char c) { return std::iscntrl(c); }, ' ');
                const HSteamNetConnection receiver = msg.player_id;
                msg.player_id = networking_msg->m_conn;

                if (client_exists(receiver, true)) {
                    Printf("(%u, %s) -> (%u, %s): %s",
                        msg.player_id, client_it->second.name, receiver, clients_[receiver].name, msg.chat_content);
                    msg.clear();
                    msg.serialize();
                    send(receiver, msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
                } else {
                    Printf("(%u, %s) -> (%u, %s): %s",
                        msg.player_id, client_it->second.name, receiver, "[Server]", msg.chat_content);
                    if (receiver != k_HSteamNetConnection_Invalid) {
                        bmmo::action_denied_msg denied_msg{.content = {bmmo::deny_reason::TargetNotFound}};
                        send(msg.player_id, denied_msg, k_nSteamNetworkingSend_Reliable);
                    }
                };
                break;
            }
            case bmmo::ImportantNotification: {
                if (deny_action(networking_msg->m_conn))
                    break;
                bmmo::important_notification_msg msg{};
                msg.raw.write(static_cast<const char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();

                std::replace_if(msg.chat_content.begin(), msg.chat_content.end(),
                    [](char c) { return std::iscntrl(c); }, ' ');
                msg.player_id = networking_msg->m_conn;

                Printf("[Announcement] (%u, %s): %s", msg.player_id, client_it->second.name, msg.chat_content);
                msg.clear();
                msg.serialize();
                broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
                break;
            }
            case bmmo::PlayerReady: {
                auto* msg = reinterpret_cast<bmmo::player_ready_msg*>(networking_msg->m_pData);
                msg->content.player_id = networking_msg->m_conn;
                client_it->second.ready = msg->content.ready;
                msg->content.count = std::count_if(clients_.begin(), clients_.end(),
                    [](const auto& i) { return i.second.ready; });
                Printf("(#%u, %s) is%s ready to start (%u player%s ready).",
                    networking_msg->m_conn, client_it->second.name,
                    msg->content.ready ? "" : " not",
                    msg->content.count, msg->content.count == 1 ? "" : "s");
                broadcast_message(*msg, k_nSteamNetworkingSend_Reliable);
                break;
            }
            case bmmo::Countdown: {
                if (deny_action(networking_msg->m_conn))
                    break;
                auto* msg = reinterpret_cast<bmmo::countdown_msg*>(networking_msg->m_pData);

                std::string map_name = msg->content.map.get_display_name(map_names_);
                switch (msg->content.type) {
                    case bmmo::countdown_type::Go: {
                        Printf("[%u, %s]: %s - Go!%s", networking_msg->m_conn, client_it->second.name, map_name, msg->content.force_restart ? " (rank reset)" : "");
                        if (force_restart_level_ || msg->content.force_restart) {
                            maps_.clear();
                            for (const auto& map: map_names_)
                                maps_[map.first] = {0, SteamNetworkingUtils()->GetLocalTimestamp()};
                        } else {
                            maps_[msg->content.map.get_hash_bytes_string()] = {0, SteamNetworkingUtils()->GetLocalTimestamp()};
                        }
                        msg->content.restart_level = restart_level_;
                        msg->content.force_restart = force_restart_level_;
                        for (auto& i: clients_)
                            i.second.ready = false;
                        break;
                    }
                    case bmmo::countdown_type::Countdown_1:
                    case bmmo::countdown_type::Countdown_2:
                    case bmmo::countdown_type::Countdown_3:
                        Printf("[%u, %s]: %s - %u", networking_msg->m_conn, client_it->second.name, map_name, msg->content.type);
                        break;
                    case bmmo::countdown_type::Ready:
                        Printf("[%u, %s]: %s - Get ready", networking_msg->m_conn, client_it->second.name, map_name);
                        break;
                    case bmmo::countdown_type::ConfirmReady:
                        Printf("[%u, %s]: %s - Please use \"/mmo ready\" to confirm if you are ready", networking_msg->m_conn, client_it->second.name, map_name);
                        break;
                    case bmmo::countdown_type::Unknown:
                    default:
                        return;
                }

                msg->content.sender = networking_msg->m_conn;
                broadcast_message(*msg, k_nSteamNetworkingSend_Reliable);
                break;
            }
            case bmmo::DidNotFinish: {
                auto* msg = reinterpret_cast<bmmo::did_not_finish_msg*>(networking_msg->m_pData);
                msg->content.player_id = networking_msg->m_conn;
                Printf(
                    "%s(#%u, %s) did not finish %s (aborted at sector %d).",
                    msg->content.cheated ? "[CHEAT] " : "",
                    msg->content.player_id, clients_[msg->content.player_id].name,
                    msg->content.map.get_display_name(map_names_),
                    msg->content.sector
                );
                broadcast_message(*msg, k_nSteamNetworkingSend_Reliable);
                break;
            }
            case bmmo::LevelFinish:
                break;
            case bmmo::LevelFinishV2: {
                auto* msg = reinterpret_cast<bmmo::level_finish_v2_msg*>(networking_msg->m_pData);
                msg->content.player_id = networking_msg->m_conn;

                // Cheat check
                if (msg->content.map.level * 100 != msg->content.levelBonus || msg->content.lifeBonus != 200) {
                    msg->content.cheated = true;
                }

                // Prepare data...
                int score = msg->content.levelBonus + msg->content.points + msg->content.lives * msg->content.lifeBonus;
                std::string md5_str = msg->content.map.get_hash_bytes_string();
                auto& current_map = maps_[md5_str];

                if (current_map.start_time != 0 && msg->content.timeElapsed - current_map.start_time / 1e6f < 9000)
                    msg->content.timeElapsed = (SteamNetworkingUtils()->GetLocalTimestamp() - current_map.start_time) / 1e6f;
                int total = int(msg->content.timeElapsed);
                int minutes = total / 60;
                int seconds = total % 60;
                int hours = minutes / 60;
                minutes = minutes % 60;
                int ms = int((msg->content.timeElapsed - total) * 1000);

                // Prepare message
                msg->content.rank = ++maps_[md5_str].rank;
                Printf("%s(#%u, %s) finished %s in %d%s place (score: %d; real time: %02d:%02d:%02d.%03d).",
                    msg->content.cheated ? "[CHEAT] " : "",
                    msg->content.player_id, clients_[msg->content.player_id].name,
                    msg->content.map.get_display_name(map_names_), maps_[md5_str].rank, bmmo::get_ordinal_rank(msg->content.rank),
                    score, hours, minutes, seconds, ms);

                broadcast_message(*msg, k_nSteamNetworkingSend_Reliable);

                break;
            }
            case bmmo::MapNames: {
                bmmo::map_names_msg msg{};
                msg.raw.write(static_cast<const char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();

                map_names_.insert(msg.maps.begin(), msg.maps.end());

                msg.clear();
                msg.serialize();
                broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable, networking_msg->m_conn);
                break;
            }
            case bmmo::CheatState: {
                auto* state_msg = reinterpret_cast<bmmo::cheat_state_msg*>(networking_msg->m_pData);
                client_it->second.cheated = state_msg->content.cheated;
                Printf("%s turned [%s] cheat!", client_it->second.name, state_msg->content.cheated ? "on" : "off");
                bmmo::owned_cheat_state_msg new_msg{};
                new_msg.content.player_id = networking_msg->m_conn;
                new_msg.content.state.cheated = state_msg->content.cheated;
                new_msg.content.state.notify = state_msg->content.notify;
                broadcast_message(&new_msg, sizeof(new_msg), k_nSteamNetworkingSend_Reliable);

                break;
            }
            case bmmo::CheatToggle: {
                if (deny_action(networking_msg->m_conn))
                    break;
                auto* state_msg = reinterpret_cast<bmmo::cheat_toggle_msg*>(networking_msg->m_pData);
                Printf("%s toggled cheat [%s] globally!", client_it->second.name, state_msg->content.cheated ? "on" : "off");
                bmmo::owned_cheat_toggle_msg new_msg{};
                new_msg.content.player_id = client_it->first;
                new_msg.content.state.cheated = state_msg->content.cheated;
                broadcast_message(&new_msg, sizeof(new_msg), k_nSteamNetworkingSend_Reliable);

                break;
            }
            case bmmo::KickRequest: {
                bmmo::kick_request_msg msg{};
                msg.raw.write(static_cast<const char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();

                HSteamNetConnection player_id = msg.player_id;
                if (!msg.player_name.empty()) {
                    Printf("%s requested to kick player \"%s\"!", client_it->second.name, msg.player_name);
                    player_id = get_client_id(msg.player_name);
                } else {
                    Printf("%s requested to kick player #%u!", client_it->second.name, msg.player_id);
                }
                if (deny_action(networking_msg->m_conn))
                    break;

                std::replace_if(msg.reason.begin(), msg.reason.end(),
                    [](char c) { return std::iscntrl(c); }, ' ');

                if (!kick_client(player_id, msg.reason, client_it->first)) {
                    bmmo::action_denied_msg new_msg{};
                    new_msg.content.reason = bmmo::deny_reason::TargetNotFound;
                    send(networking_msg->m_conn, new_msg, k_nSteamNetworkingSend_Reliable);
                };

                break;
            }
            case bmmo::CurrentMap: {
                auto* msg = reinterpret_cast<bmmo::current_map_msg*>(networking_msg->m_pData);
                msg->content.player_id = networking_msg->m_conn;
                switch (msg->content.type) {
                    case bmmo::current_map_state::Announcement: {
                        broadcast_message(*msg, k_nSteamNetworkingSend_Reliable);
                        Printf("%s(#%u, %s) is at the %d%s sector of %s.",
                            client_it->second.cheated ? "[CHEAT] " : "",
                            networking_msg->m_conn, client_it->second.name,
                            msg->content.sector, bmmo::get_ordinal_rank(msg->content.sector),
                            msg->content.map.get_display_name(map_names_));
                        break;
                    }
                    case bmmo::current_map_state::EnteringMap: {
                        client_it->second.current_map = msg->content.map;
                        client_it->second.current_sector = msg->content.sector;
                        broadcast_message(*msg, k_nSteamNetworkingSend_Reliable, networking_msg->m_conn);
                        break;
                    }
                    default: break;
                }
                break;
            }
            case bmmo::CurrentSector: {
                auto* msg = reinterpret_cast<bmmo::current_sector_msg*>(networking_msg->m_pData);
                msg->content.player_id = networking_msg->m_conn;
                client_it->second.current_sector = msg->content.sector;
                broadcast_message(*msg, k_nSteamNetworkingSend_Reliable, networking_msg->m_conn);
                break;
            }
            case bmmo::SimpleAction: {
                auto* msg = reinterpret_cast<bmmo::simple_action_msg*>(networking_msg->m_pData);
                switch (msg->content.action) {
                    case bmmo::action_type::CurrentMapQuery: {
                        break;
                    }
                    case bmmo::action_type::FatalError: {
                        Printf("(#%u, %s) has encountered a fatal error!",
                            networking_msg->m_conn, client_it->second.name);
                        // they already got their own fatal error, so we don't need to induce one here.
                        kick_client(networking_msg->m_conn, "fatal error", k_HSteamNetConnection_Invalid, bmmo::crash_type::SelfTriggeredFatalError);
                        break;
                    }
                    default:
                        break;
                }
                break;
            }
            case bmmo::PermanentNotification: {
                if (deny_action(networking_msg->m_conn))
                    break;
                auto msg = bmmo::message_utils::deserialize<bmmo::permanent_notification_msg>(networking_msg);
                msg.title = client_it->second.name;
                Printf("[Bulletin] %s%s", msg.title,
                        msg.text_content.empty() ? " - Content cleared" : ": " + msg.text_content);
                permanent_notification_ = {msg.title, msg.text_content};
                msg.clear();
                msg.serialize();
                broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
                break;
            }
            case bmmo::PlainText: {
                bmmo::plain_text_msg msg{};
                msg.raw.write(static_cast<const char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();
                Printf("[Plain] (%u, %s): %s", networking_msg->m_conn, client_it->second.name, msg.text_content);
                break;
            }
            case bmmo::HashData: {
                bmmo::hash_data_msg msg{};
                msg.raw.write(static_cast<const char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();

                if (msg.data_name == "Balls.nmo" && !msg.is_same_data("fb29d77e63aad08499ce38d36266ec33")) {
                    bmmo::plain_text_msg new_msg{};
                    std::string md5_string;
                    bmmo::string_from_hex_chars(md5_string, msg.md5, sizeof(msg.md5));
                    new_msg.text_content = "Warning: " + client_it->second.name + " has a modified Balls.nmo (MD5 " + md5_string.substr(0, 12) + "..)! This could be problematic.";
                    Printf("%s", new_msg.text_content);
                    new_msg.serialize();
                    broadcast_message(new_msg.raw.str().data(), new_msg.size(), k_nSteamNetworkingSend_Reliable);
                }
                break;
            }
            case bmmo::ModList: {
                break;
            }
            case bmmo::OwnedBallState:
            case bmmo::OwnedBallStateV2:
            case bmmo::OwnedTimedBallState:
            case bmmo::LoginAcceptedV2:
            case bmmo::LoginAcceptedV3:
            case bmmo::PlayerConnectedV2:
            case bmmo::OwnedCheatState:
            case bmmo::OwnedCheatToggle:
            case bmmo::PlayerKicked:
            case bmmo::ActionDenied:
            case bmmo::OpState:
            case bmmo::KeyboardInput:
                break;
            default:
                Printf("Error: invalid message with opcode %d received.", raw_msg->code);
        }

        // TODO: replace with actual message data structure handling
//        std::string str;
//        str.assign((const char*)networking_msg->m_pData, networking_msg->m_cbSize);
//
//        Printf("%s: %s", client_it->second.name.c_str(), str.c_str());
//        interface_->SendMessageToConnection(client_it->first, str.c_str(), str.length() + 1,
//                                            k_nSteamNetworkingSend_Reliable,
//                                            nullptr);
    }

    int poll_incoming_messages() override {
        int msg_count = interface_->ReceiveMessagesOnPollGroup(poll_group_, incoming_messages_, ONCE_RECV_MSG_COUNT);
        if (msg_count == 0)
            return 0;
        else if (msg_count < 0)
            FatalError("Error checking for messages.");
        assert(msg_count > 0);

        for (int i = 0; i < msg_count; ++i) {
            on_message(incoming_messages_[i]);
            incoming_messages_[i]->Release();
        }

        return msg_count;
    }

    inline void tick() {
        bmmo::owned_timed_ball_state_msg msg{};
        pull_unupdated_ball_states(msg.balls, msg.unchanged_balls);
        if (msg.balls.empty() && msg.unchanged_balls.empty())
            return;
        msg.serialize();
        broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_UnreliableNoDelay);
    };

    void start_ticking() {
        ticking_ = true;
        // ticking_thread_ = std::thread([&]() {
        Printf("Ticking started.");
        //     while (ticking_) {
        //         auto next_tick = std::chrono::steady_clock::now() + TICK_INTERVAL;
        //         tick();
        //         std::this_thread::sleep_until(next_tick);
    }
    void stop_ticking() {
        ticking_ = false;
        // ticking_thread_.join();
        Printf("Ticking stopped.");
    }

    uint16_t port_ = 0;
    HSteamListenSocket listen_socket_ = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup poll_group_ = k_HSteamNetPollGroup_Invalid;
//    std::thread server_thread_;
    std::unordered_map<HSteamNetConnection, client_data> clients_;
    std::unordered_map<std::string, HSteamNetConnection> username_; // Note: this stores names converted to all-lowercases
    std::mutex client_data_mutex_;
    std::pair<std::string, std::string> permanent_notification_; // <username (title), text>
    constexpr static inline std::chrono::nanoseconds TICK_DELAY{(int)1e9 / 198},
                                                     UPDATE_INTERVAL{(int)1e9 / 66};

    std::mutex startup_mutex_;
    std::condition_variable startup_cv_;

    // std::thread ticking_thread_;
    std::atomic_bool ticking_ = false;
    YAML::Node config_;
    std::unordered_map<std::string, std::string> op_players_, banned_players_, map_names_;
    std::unordered_set<std::string> muted_players_;
    std::unordered_map<std::string, map_data> maps_;
    bool op_mode_ = true, restart_level_ = true, force_restart_level_ = false;
    ESteamNetworkingSocketsDebugOutputType logging_level_ = k_ESteamNetworkingSocketsDebugOutputType_Important;
};

// parse arguments (optional port and help/version/log) with getopt
int parse_args(int argc, char** argv, uint16_t* port, std::string& log_path, bool* dry_run) {
    enum option_values { DryRun = UINT8_MAX + 1 };
    static struct option long_options[] = {
        {"port", required_argument, 0, 'p'},
        {"log", required_argument, 0, 'l'},
        {"help", no_argument, 0, 'h'},
        {"version", no_argument, 0, 'v'},
        {"dry-run", no_argument, 0, DryRun},
        {0, 0, 0, 0}
    };
    int opt, opt_index = 0;
    while ((opt = getopt_long(argc, argv, "p:l:hv", long_options, &opt_index)) != -1) {
        switch (opt) {
            case 'p':
                *port = atoi(optarg);
                break;
            case 'l':
                log_path = optarg;
                break;
            case 'h':
                printf("Usage: %s [OPTION]...\n", argv[0]);
                puts("Options:");
                puts("  -p, --port=PORT\t Use PORT as the server port instead (default: 26676).");
                puts("  -l, --log=PATH\t Write log to the file at PATH in addition to stdout.");
                puts("  -h, --help\t\t Display this help and exit.");
                puts("  -v, --version\t\t Display version information and exit.");
                puts("      --dry-run\t\t Test the server by starting it and exiting immediately.");
                return -1;
            case 'v':
                puts("Ballance MMO server by Swung0x48 and BallanceBug.");
                printf("Build time: \t%s %s.\n", __DATE__, __TIME__);
                printf("Version: \t%s.\n", bmmo::version_t{}.to_string().c_str());
                printf("Minimum accepted client version: %s.\n", bmmo::minimum_client_version.to_string().c_str());
                puts("GitHub repository: https://github.com/Swung0x48/BallanceMMO");
                return -1;
            case DryRun:
                *dry_run = true;
                break;
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    uint16_t port = 26676;
    bool dry_run = false;
    std::string log_path;
    if (parse_args(argc, argv, &port, log_path, &dry_run) < 0)
        return 0;

    if (port == 0) {
        std::cerr << "Fatal: invalid port number." << std::endl;
        return 1;
    };

    if (!log_path.empty()) {
        FILE* log_file = fopen(log_path.c_str(), "a");
        if (log_file == nullptr) {
            std::cerr << "Fatal: failed to open the log file." << std::endl;
            return 1;
        }
        server::set_log_file(log_file);
    }

    printf("Initializing sockets...\n");
    server::init_socket();

    printf("Starting server at port %u.\n", port);
    server server(port);

    printf("Bootstrapping server...\n");
    fflush(stdout);
    if (!server.setup())
        server::FatalError("Server failed on setup.");
    std::thread server_thread([&server]() { server.run(); });

    console console;
    console.register_command("stop", [&] { server.shutdown(atoi(console.get_next_word().c_str())); });
    console.register_command("list", [&] { server.print_clients(); });
    console.register_command("list-uuid", [&] { server.print_clients(true); });
    console.register_command("say", [&] {
        bmmo::chat_msg msg{};
        msg.chat_content = console.get_rest_of_line();
        msg.serialize();

        server.broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        server.Printf("([Server]): %s", msg.chat_content);
    });
    auto send_plain_text_msg = [&](bool broadcast = false) {
        HSteamNetConnection client{};
        if (!broadcast) {
            client = server.get_client_id(console.get_next_word());
            if (client == k_HSteamNetConnection_Invalid)
                return;
        }
        bmmo::plain_text_msg msg{};
        msg.text_content = console.get_rest_of_line();
        msg.serialize();
        if (broadcast) {
            server.broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
            server.Printf("[Plain]: %s", msg.text_content);
        } else {
            server.send(client, msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
            server.Printf("[Plain -> #%u]: %s", msg.text_content, client);
        }
    };
    console.register_command("plaintext", send_plain_text_msg);
    console.register_command("plaintext-broadcast", std::bind(send_plain_text_msg, true));
    auto send_popup_msg = [&](bool broadcast = false) {
        HSteamNetConnection client = k_HSteamNetConnection_Invalid;
        if (!broadcast) {
            client = server.get_client_id(console.get_next_word());
            if (client == k_HSteamNetConnection_Invalid)
                return;
        }
        std::string line = console.get_rest_of_line();
        bmmo::popup_box_msg msg{};
        auto pos = line.find("||");
        msg.title = pos == std::string::npos ? "BallanceMMO - Message" : line.substr(0, pos);
        msg.text_content = pos == std::string::npos ? line : line.substr(pos + 2);
        msg.serialize();
        if (!broadcast)
            server.send(client, msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        else
            server.broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        server.Printf("[Popup -> %s] {%s}: %s", client == k_HSteamNetConnection_Invalid ? "[all]" : std::to_string(client), msg.title, msg.text_content);
    };
    console.register_command("popup", send_popup_msg);
    console.register_command("popup-broadcast", std::bind(send_popup_msg, true));
    console.register_command("announce", [&] {
        bmmo::important_notification_msg msg{};
        msg.chat_content = console.get_rest_of_line();
        msg.serialize();
        server.broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        server.Printf("[Announcement] ([Server]): %s", msg.chat_content);
    });
    console.register_command("cheat", [&] {
        bool cheat_state = false;
        if (console.get_next_word() == "on")
            cheat_state = true;
        server.toggle_cheat(cheat_state);
    });
    console.register_command("version", [&] { server.print_version_info(); });
    console.register_aliases("version", {"ver"});
    console.register_command("getmap", [&] { server.print_player_maps(); });
    console.register_command("getpos", [&] { server.print_positions(); });
    console.register_command("kick", [&] {
        HSteamNetConnection client = k_HSteamNetConnection_Invalid;
        std::string client_input = console.get_next_word();
        if (client_input.length() > 0 && client_input[0] == '#') {
            client = atoll(client_input.substr(1).c_str());
            if (client == 0) {
                server.Printf("Error: invalid connection id.");
                return;
            }
        } else {
            client = server.get_client_id(client_input);
        }
        std::string text = console.get_rest_of_line();
        if (console.get_command_name() == "whisper") {
            bmmo::private_chat_msg msg{};
            msg.chat_content = text;
            msg.serialize();
            server.send(client, msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
            server.Printf("([Server]) -> #%u: %s", client, msg.chat_content);
        } else if (console.get_command_name() == "ban") {
            server.set_ban(client, text);
        } else {
            bmmo::crash_type crash = bmmo::crash_type::None;
            if (console.get_command_name() == "crash")
                crash = bmmo::crash_type::Crash;
            else if (console.get_command_name() == "fatalerror")
                crash = bmmo::crash_type::FatalError;
            server.kick_client(client, text, k_HSteamNetConnection_Invalid, crash);
        }
    });
    console.register_aliases("kick", {"crash", "fatalerror", "whisper", "ban"});
    console.register_command("op", [&] {
        HSteamNetConnection client = k_HSteamNetConnection_Invalid;
        std::string client_input = console.get_next_word(), cmd = console.get_command_name();
        if (client_input.length() > 0 && client_input[0] == '#') {
            client = atoll(client_input.substr(1).c_str());
            if (client == 0) {
                server.Printf("Error: invalid connection id.");
                return;
            }
        } else {
            client = server.get_client_id(client_input);
        }
        bool action = false;
        if (cmd == "op" || cmd == "mute")
            action = true;
        if (cmd == "op" || cmd == "deop")
            server.set_op(client, action);
        else
            server.set_mute(client, action);
    });
    console.register_aliases("op", {"deop", "mute", "unmute"});
    console.register_command("reload", [&] {
        if (!server.load_config())
            server.Printf("Error: failed to reload config.");
    });
    console.register_command("listmap", [&] { server.print_maps(); });
    console.register_command("countdown", [&] {
        auto print_hint = [] {
            role::Printf("Error: please specify the map to countdown (hint: use \"getmap\" and \"listmap\").");
            role::Printf("Usage: \"countdown <client id> level|<hash> <level number> [type]\".");
            role::Printf("<type>: {\"4\": \"Get ready\", \"5\": \"Confirm ready\", \"\": \"auto countdown\"}");
        };
        auto client = (HSteamNetConnection) atoll(console.get_next_word().c_str());
        if (console.empty()) { print_hint(); return; }
        std::string hash = console.get_next_word();
        if (console.empty()) { print_hint(); return; }
        bmmo::map map{.type = bmmo::map_type::OriginalLevel, .level = std::clamp(atoi(console.get_next_word().c_str()), 0, 13)};
        if (hash == "level")
            bmmo::hex_chars_from_string(map.md5, bmmo::map::original_map_hashes[map.level]);
        else
            bmmo::hex_chars_from_string(map.md5, hash);
        bmmo::countdown_msg msg{.content = {.map = map}};
        if (console.empty()) {
            for (int i = 3; i >= 0; --i) {
                msg.content.type = static_cast<bmmo::countdown_type>(i);
                server.receive(&msg, sizeof(msg), client);
                if (i != 0) std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        } else {
            msg.content.type = static_cast<bmmo::countdown_type>(atoi(console.get_next_word().c_str()));
            server.receive(&msg, sizeof(msg), client);
        }
    });
    console.register_command("countdown-forced", [&] {
        bmmo::countdown_msg msg{};
        msg.content.restart_level = msg.content.force_restart = true;
        msg.content.map.type = bmmo::map_type::OriginalLevel;
        for (int i = 3; i >= 0; --i) {
            msg.content.type = static_cast<bmmo::countdown_type>(i);
            server.broadcast_message(msg, k_nSteamNetworkingSend_Reliable);
            server.Printf("[[Server]]: Countdown - %s", i == 0 ? "Go!" : std::to_string(i));
            if (i != 0)
                std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    });
    console.register_command("bulletin", [&] {
        auto& bulletin = server.get_bulletin();
        if (console.get_command_name() == "bulletin") {
            bulletin = {"[Server]", console.get_rest_of_line()};
            bmmo::permanent_notification_msg msg{};
            std::tie(msg.title, msg.text_content) = bulletin;
            msg.serialize();
            server.broadcast_message(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
        }
        server.Printf("[Bulletin] %s%s", bulletin.first,
            bulletin.second.empty() ? " - Empty" : ": " + bulletin.second);
    });
    console.register_aliases("bulletin", {"getbulletin"});
    console.register_command("help", [&] { server.Printf(console.get_help_string().c_str()); });

    server.wait_till_started();

    if (dry_run)
        server.shutdown();

    while (server.running()) {
        std::cout << "\r> " << std::flush;
        std::string line;
        if (!console.read_input(line)) {
            puts("stop");
            server.shutdown();
        };
        server.LogFileOutput(("> " + line).c_str());

        if (!console.execute(line) && !console.get_command_name().empty()) {
            std::string extra_text;
            if (auto hints = console.get_command_hints(true); !hints.empty())
                extra_text = " Did you mean: " + bmmo::string_utils::join_strings(hints, 0, ", ") + "?";
            server.Printf("Error: unknown command \"%s\".%s", console.get_command_name(), extra_text);
        }
    }

    std::cout << "Stopping..." << std::endl;
    if (server_thread.joinable())
        server_thread.join();

    server::destroy();
    printf("\r");
}
