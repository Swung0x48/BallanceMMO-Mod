#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
#include <steam/steam_api.h>
#endif

#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <cassert>
#include <cstdarg>
#include <sstream>
#include <chrono>
#include <mutex>
#include <list>
#include <fstream>
#include <filesystem>
#include <condition_variable>
#include <random>

#include <asio/io_service.hpp>
#include <asio/ip/tcp.hpp>
#include <ya_getopt.h>

#include "common.hpp"
#include "entity/record_entry.hpp"

using bmmo::Printf, bmmo::Sprintf, bmmo::LogFileOutput, bmmo::FatalError;

bool cheat = false;
struct client_data {
    std::string name;
    bool cheated;
    uint16_t ping = 0;
    bmmo::ball_state state{};
    bmmo::map current_map{};
    int32_t current_sector = 0;
};

static struct option_t {
    std::string server_addr = "127.0.0.1:26676", username = "MockClient",
                uuid = "00010002-0003-0004-0005-000600070008", log_path;
    bool print_states = false, recorder_mode = false, individual_packets = false, save_sound_files = true;
    ESteamNetworkingSocketsDebugOutputType detail = k_ESteamNetworkingSocketsDebugOutputType_Important;
} options;

class client: public role {
public:
    bool connect(const std::string& connection_string) {
        bmmo::hostname_parser hp(connection_string);
        using asio::ip::tcp; // tired of writing it out
        asio::io_context io_context;
        tcp::resolver resolver(io_context);
        tcp::resolver::query query(hp.get_address(), hp.get_port());
        for (tcp::resolver::iterator iter = resolver.resolve(query), end; iter != end; iter++) {
            tcp::endpoint ep = *iter;
            std::string resolved_addr = ep.address().to_string() + ":" + std::to_string(ep.port());
            Printf("Trying %s...", resolved_addr);
            SteamNetworkingIPAddr server_address{};
            if (!server_address.ParseString(resolved_addr.c_str())) {
                return false;
            }
            SteamNetworkingConfigValue_t opt = generate_opt();
            connection_ = interface_->ConnectByIPAddress(server_address, 1, &opt);
            if (connection_ == k_HSteamNetConnection_Invalid)
                continue;
            io_context.stop();
            return true;
        }
        return false;
    }

    bool setup_recorder() {
        recorder_mode_ = true;
        auto current_time = std::time(nullptr);
        char record_name[40];
        std::strftime(record_name, sizeof(record_name), "records/record_%Y%m%d%H%M.bin", std::localtime(&current_time));
        std::filesystem::create_directory("records");
        Printf("Flight recorder started (saving at \"%s\").", record_name);
        if (options.individual_packets) {
            std::filesystem::create_directories(packet_save_path);
            Printf("Individual packets will be saved at \"%s\".", packet_save_path);
        }
        record_stream_.open(record_name, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!record_stream_.is_open())
            return false;
        record_stream_ << bmmo::RECORD_HEADER;
        record_stream_.put('\0');
        int64_t init_time = init_time_t_; // be specific about time_t type
        record_stream_.write(reinterpret_cast<const char*>(&bmmo::current_version), sizeof(bmmo::current_version));
        record_stream_.write(reinterpret_cast<const char*>(&init_time), sizeof(init_time));
        record_stream_.write(reinterpret_cast<const char*>(&init_timestamp_), sizeof(init_timestamp_));
        return true;
    }

    void run() override {
        bmmo::console::set_completion_callback([this](const std::vector<std::string>& args) -> std::vector<std::string> {
            switch (args.size()) {
                case 0: return {};
                case 1: return bmmo::console::instance->get_command_hints(false, args[0].c_str());
            }
            std::vector<std::string> player_hints;
            player_hints.reserve(clients_.size());
            bool hint_client_id = (args[args.size() - 1].starts_with('#'));
            for (const auto& [id, data]: clients_) {
                if (hint_client_id)
                    player_hints.emplace_back('#' + std::to_string(id));
                else
                    player_hints.emplace_back(data.name);
            }
            return player_hints;
        });

        running_ = true;
        startup_cv_.notify_all();
        while (running_) {
            auto update_begin = std::chrono::steady_clock::now();
            update();
            std::this_thread::sleep_until(update_begin + bmmo::CLIENT_RECEIVE_INTERVAL);
        }

//        while (running_) {
//            poll_local_state_changes();
//        }
    }

    EResult send(void* buffer, size_t size, int send_flags, int64* out_message_number = nullptr) {
        return interface_->SendMessageToConnection(connection_,
                                                   buffer,
                                                   size,
                                                   send_flags,
                                                   out_message_number);

    }

    template<typename T>
    EResult send(T msg, int send_flags, int64* out_message_number = nullptr) {
        static_assert(std::is_trivially_copyable<T>());
        return send(&msg,
                    sizeof(msg),
                    send_flags,
                    out_message_number);
    }

    void receive(void* data, size_t size) {
        auto* networking_msg = SteamNetworkingUtils()->AllocateMessage(0);
        networking_msg->m_conn = connection_;
        networking_msg->m_pData = data;
        networking_msg->m_cbSize = size;
        on_message(networking_msg);
        networking_msg->Release();
    }

    std::string get_detailed_info() {
        char info[2048];
        interface_->GetDetailedConnectionStatus(connection_, info, 2048);
        return {info};
    }

    SteamNetConnectionRealTimeStatus_t get_info() {
        SteamNetConnectionRealTimeStatus_t status{};
        interface_->GetConnectionRealTimeStatus(connection_, &status, 0, nullptr);
        return status;
    }

    SteamNetConnectionRealTimeLaneStatus_t get_lane_info() {
        SteamNetConnectionRealTimeLaneStatus_t status{};
        interface_->GetConnectionRealTimeStatus(connection_, nullptr, 0, &status);
        return status;
    }

    const bmmo::map& get_last_countdown_map() { return last_countdown_map_; }
    const bmmo::ranking_entry::map_rankings& get_local_rankings() { return local_rankings_; }

    bmmo::timed_ball_state_msg& get_local_state_msg() {
        return local_state_msg_;
    }

    std::string get_player_name(HSteamNetConnection player_id) const {
        if (player_id == k_HSteamNetConnection_Invalid) return "[Server]";
        if (auto it = clients_.find(player_id); it != clients_.end())
            return it->second.name;
        else
            return "#" + std::to_string(player_id);
    }

    void print_bulletin() {
        Printf(bmmo::color_code(bmmo::PermanentNotification), "[Bulletin] %s", permanent_notification_text_);
    }

    void print_clients() {
        decltype(clients_) spectators;
        std::vector<std::pair<HSteamNetConnection, client_data>> players;
        players.reserve(clients_.size());
        auto print_client = [&](auto i) {
            Printf("%u: %s%s  %4dms",
                    i.first, i.second.name, i.second.cheated ? " [CHEAT]" : "", i.second.ping);
        };
        for (const auto& i: clients_) {
            if (bmmo::name_validator::is_spectator(i.second.name)) {
                spectators.insert(i);
                continue;
            }
            players.emplace_back(i);
        }
        std::sort(players.begin(), players.end(), [](const auto& i1, const auto& i2)
            { return bmmo::message_utils::to_lower(i1.second.name) < bmmo::message_utils::to_lower(i2.second.name); });
        for (const auto& i: spectators) print_client(i);
        for (const auto& i: players) print_client(i);
        Printf("%d client(s) online: %d player(s), %d spectator(s).",
            clients_.size(), players.size(), spectators.size());
    }

    void print_player_maps() {
        std::vector<std::pair<HSteamNetConnection, client_data>> players;
        players.reserve(clients_.size());
        for (const auto& i: clients_)
            players.emplace_back(i);
        std::sort(players.begin(), players.end(), [](const auto& i1, const auto& i2)
            { return bmmo::message_utils::to_lower(i1.second.name) < bmmo::message_utils::to_lower(i2.second.name); });
        for (const auto& [id, data]: players)
            Printf("%s(#%u, %s) is at the %d%s sector of %s.",
                data.cheated ? "[CHEAT] " : "", id, data.name,
                data.current_sector, bmmo::string_utils::get_ordinal_suffix(data.current_sector),
                data.current_map.get_display_name(map_names_));
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

    void print_positions() {
        std::vector<std::pair<HSteamNetConnection, client_data>> players;
        players.reserve(clients_.size());
        for (const auto& i: clients_)
            players.emplace_back(i);
        std::sort(players.begin(), players.end(), [](const auto& i1, const auto& i2)
            { return bmmo::message_utils::to_lower(i1.second.name) < bmmo::message_utils::to_lower(i2.second.name); });
        for (const auto& [id, data]: players) {
            Printf("(%u, %s) is at %.2f, %.2f, %.2f with %s ball.",
                    id, data.name,
                    data.state.position.x, data.state.position.y, data.state.position.z,
                    data.state.get_type_name()
            );
        }
    }

    void set_nickname(const std::string& name) { nickname_ = name; };
    void set_own_map(const bmmo::map& current_map, const std::string& map_name = "") {
        clients_[own_id_].current_map = current_map;
        if (!map_name.empty()) map_names_.try_emplace(current_map.get_hash_bytes_string(), map_name);
    }
    void set_print_states(bool print_states) { print_states_ = print_states; }
    void set_own_sector(const int32_t sector) { clients_[own_id_].current_sector = sector; }

    void set_uuid(std::string uuid) {
        size_t pos;
        while ((pos = uuid.find('-')) != std::string::npos)
            uuid.erase(pos, 1);
        bmmo::hex_chars_from_string(uuid_, uuid);
    };

    void shutdown() {
        running_ = false;
        interface_->CloseConnection(connection_, 0, "Goodbye", true);
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!recorder_mode_)
            return;
        while (!message_queue_.empty()) {
            Printf("Waiting for %d messages to be processed...", message_queue_.size());
            bmmo::record_entry entry = std::move(message_queue_.front());
            record_stream_.write(reinterpret_cast<const char*>(entry.data), entry.size);
            message_queue_.pop_front();
        }
        message_available_cv_.notify_all();
    }

    void teleport_to(const HSteamNetConnection player_id) {
        if (auto it = clients_.find(player_id); it != clients_.end()) {
            auto& client = it->second;
            local_state_msg_.content.position = client.state.position;
            local_state_msg_.content.rotation = client.state.rotation;
            // local_state_msg_.content.timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count(); // wtf is this from github copilot
            local_state_msg_.content.timestamp = SteamNetworkingUtils()->GetLocalTimestamp();
            send(local_state_msg_, k_nSteamNetworkingSend_Reliable);
            Printf(bmmo::ansi::WhiteInverse, "Teleported to %s at (%.3f, %.3f, %.3f).",
                    client.name, client.state.position.x, client.state.position.y, client.state.position.z);
        }
        else {
            Printf("Player not found.");
        }
    }

    void wait_till_started() {
        while (!running()) {
            std::unique_lock<std::mutex> lk(startup_mutex_);
            startup_cv_.wait(lk);
        }
    }

    void whisper_to(const HSteamNetConnection player_id, const std::string& message) {
        if (auto it = clients_.find(player_id); it != clients_.end() || player_id == k_HSteamNetConnection_Invalid) {
            bmmo::private_chat_msg msg{};
            msg.player_id = player_id;
            msg.chat_content = message;
            msg.serialize();
            send(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
            Printf(bmmo::color_code(msg.code), "Whispered to (%u, %s): %s",
                    player_id, get_player_name(player_id), message);
        }
        else {
            Printf("Player not found.");
        }
    }

    void write_record() {
        static uint32_t counter = 0;
        std::unique_lock<std::mutex> available_lk(message_available_mutex_);
        while (message_queue_.empty() && running())
            message_available_cv_.wait(available_lk);
        std::unique_lock<std::mutex> lk(message_queue_mutex_);

        while (!message_queue_.empty()) {
            bmmo::record_entry entry = std::move(message_queue_.front());
            if (options.individual_packets) {
                FILE* entry_record_file = std::fopen((packet_save_path + std::to_string(counter++) + ".entry").c_str(), "ab");
                if (entry_record_file) {
                    std::fwrite(entry.data, 1, entry.size, entry_record_file);
                    std::fclose(entry_record_file);
                }
            }
            record_stream_.write(reinterpret_cast<const char*>(entry.data), entry.size);
            message_queue_.pop_front();
        }
        lk.unlock();
        record_stream_.flush();
    }

private:
    void on_connection_status_changed(SteamNetConnectionStatusChangedCallback_t* pInfo) override {
        // What's the state of the connection?
        switch (pInfo->m_info.m_eState) {
            case k_ESteamNetworkingConnectionState_None:
                // NOTE: We will get callbacks here when we destroy connections.  You can ignore these.
                break;

            case k_ESteamNetworkingConnectionState_ClosedByPeer:
            case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
                running_ = false;

                // Print an appropriate message
                if (pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connecting) {
                    // Note: we could distinguish between a timeout, a rejected connection,
                    // or some other transport problem.
                    Printf("We sought the remote host, yet our efforts were met with defeat.  (%s)\n",
                           pInfo->m_info.m_szEndDebug);
                } else if (pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
                    Printf("Alas, troubles beset us; we have lost contact with the host.  (%s)\n",
                           pInfo->m_info.m_szEndDebug);
                } else {
                    // NOTE: We could check the reason code for a normal disconnection
                    Printf("The host hath bidden us farewell.  (%d: %s)", pInfo->m_info.m_eEndReason, pInfo->m_info.m_szEndDebug);
                }

                // Clean up the connection.  This is important!
                // The connection is "closed" in the network sense, but
                // it has not been destroyed.  We must close it on our end, too
                // to finish up.  The reason information do not matter in this case,
                // and we cannot linger because it's already closed on the other end,
                // so we just pass 0's.
                interface_->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
                connection_ = k_HSteamNetConnection_Invalid;
                permanent_notification_text_.clear();
                set_nickname(options.username);
                break;
            }

            case k_ESteamNetworkingConnectionState_Connecting:
                // We will get this callback when we start connecting.
                // We can ignore this.
                break;

            case k_ESteamNetworkingConnectionState_Connected: {
                Printf("Connected to server OK\n");
                //bmmo::login_request_msg msg;
                bmmo::login_request_v3_msg msg;
                msg.version = bmmo::current_version;
                msg.nickname = nickname_;
                msg.cheated = 0;
                memcpy(msg.uuid, uuid_, sizeof(uuid_));
                // msg.version = bmmo::version_t{1, 0, 0, bmmo::Alpha, 0};
                msg.serialize();
                send(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
                break;
            }
            default:
                // Silences -Wswitch
                break;
        }
    }

    void on_message(ISteamNetworkingMessage* networking_msg) override {
//        printf("\b");
//        fwrite(msg->m_pData, 1, msg->m_cbSize, stdout);
//        fputc('\n', stdout);

//        printf("\b> ");
//        Printf(reinterpret_cast<const char*>(msg->m_pData));
        auto* raw_msg = reinterpret_cast<bmmo::general_message*>(networking_msg->m_pData);
        if (networking_msg->m_cbSize < static_cast<decltype(networking_msg->m_cbSize)>(sizeof(bmmo::opcode))) {
            Printf("Error: invalid message with size %d received.", networking_msg->m_cbSize);
            return;
        }

        switch (raw_msg->code) {
            case bmmo::LoginAcceptedV3: {
                bmmo::login_accepted_v3_msg msg{};
                msg.raw.write(reinterpret_cast<char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();
                clients_.clear();
                Printf(bmmo::color_code(msg.code), "%d player(s) online:", msg.online_players.size());
                for (const auto& [id, data]: msg.online_players) {
                    if (data.name == nickname_) own_id_ = id;
                    Printf("%s (#%u)%s", data.name, id, (data.cheated ? " [CHEAT]" : ""));
                    clients_.insert({ id, { data.name, (bool) data.cheated, {}, {}, data.map, data.sector } });
                }
                bmmo::plain_text_msg text_msg{};
                text_msg.text_content = "Using Mock Client.";
                text_msg.serialize();
                send(text_msg.raw.str().data(), text_msg.size(), k_nSteamNetworkingSend_Reliable);
                break;
            }
            case bmmo::PlayerConnectedV2: {
                bmmo::player_connected_v2_msg msg{};
                msg.raw.write(reinterpret_cast<char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();
                Printf(bmmo::color_code(msg.code), "%s (#%u) logged in with cheat mode %s.",
                        msg.name.c_str(), msg.connection_id, (msg.cheated ? "on" : "off"));
                clients_[msg.connection_id] = { msg.name, (bool) msg.cheated };
                break;
            }
            case bmmo::PlayerDisconnected: {
                auto* msg = reinterpret_cast<bmmo::player_disconnected_msg*>(networking_msg->m_pData);
                if (auto it = clients_.find(msg->content.connection_id); it != clients_.end()) {
                    Printf(bmmo::color_code(msg->code), "%s (#%u) disconnected.", it->second.name, it->first);
                    clients_.erase(it);
                }
                break;
            }
            case bmmo::OwnedBallState: {
                if (!print_states_)
                    break;
                assert(networking_msg->m_cbSize == sizeof(bmmo::owned_ball_state_msg));
                auto* obs = reinterpret_cast<bmmo::owned_ball_state_msg*>(networking_msg->m_pData);
                const auto& state = obs->content.state;
                Printf("#%u: %d, (%.2lf, %.2lf, %.2lf), (%.2lf, %.2lf, %.2lf, %.2lf)",
                       obs->content.player_id,
                       state.type,
                       state.position.x, state.position.y, state.position.z,
                       state.rotation.x, state.rotation.y, state.rotation.z, state.rotation.w);
                break;
            }
            case bmmo::OwnedBallStateV2: {
                bmmo::owned_ball_state_v2_msg msg;
                msg.raw.write(reinterpret_cast<char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();

                for (auto& ball : msg.balls) {
                    if (print_states_)
                        Printf("#%u: %d, (%.2lf, %.2lf, %.2lf), (%.2lf, %.2lf, %.2lf, %.2lf)",
                            ball.player_id,
                            ball.state.type,
                            ball.state.position.x, ball.state.position.y, ball.state.position.z,
                            ball.state.rotation.x, ball.state.rotation.y, ball.state.rotation.z, ball.state.rotation.w);
                    clients_[ball.player_id].state = ball.state;
                }

                break;
            }
            case bmmo::OwnedTimedBallState:
                break;
            case bmmo::OwnedCompressedBallState: {
                bmmo::owned_compressed_ball_state_msg msg;
                msg.raw.write(reinterpret_cast<char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();

                for (auto& ball : msg.balls) {
                    if (print_states_)
                        Printf("%u: %d, (%.2f, %.2f, %.2f), (%.2f, %.2f, %.2f, %.2f), %ld",
                            ball.player_id,
                            ball.state.type,
                            ball.state.position.x, ball.state.position.y, ball.state.position.z,
                            ball.state.rotation.x, ball.state.rotation.y, ball.state.rotation.z, ball.state.rotation.w,
                            int64_t(ball.state.timestamp));
                    clients_[ball.player_id].state = ball.state;
                }

                break;
            }
            case bmmo::OwnedCheatState: {
                assert(networking_msg->m_cbSize == sizeof(bmmo::owned_cheat_state_msg));
                auto* ocs = reinterpret_cast<bmmo::owned_cheat_state_msg*>(networking_msg->m_pData);
                if (ocs->content.state.notify)
                    Printf("(%u, %s) turned cheat %s.", ocs->content.player_id, clients_[ocs->content.player_id].name, ocs->content.state.cheated ? "on" : "off");
                clients_[ocs->content.player_id].cheated = ocs->content.state.cheated;
                break;
            }
            case bmmo::Chat: {
                bmmo::chat_msg msg{};
                msg.raw.write(static_cast<const char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();

                Printf("(%u, %s): %s", msg.player_id, get_player_name(msg.player_id), msg.chat_content.c_str());
                break;
            }
            case bmmo::PrivateChat: {
                bmmo::private_chat_msg msg{};
                msg.raw.write(static_cast<const char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();

                Printf(bmmo::color_code(msg.code), "(%u, %s) whispers to you: %s",
                        msg.player_id, get_player_name(msg.player_id), msg.chat_content.c_str());
                break;
            }
            case bmmo::PlainText: {
                auto msg = bmmo::message_utils::deserialize<bmmo::plain_text_msg>(networking_msg);
                Printf(msg.text_content.c_str());
                break;
            }
            case bmmo::PublicNotification: {
                auto msg = bmmo::message_utils::deserialize<bmmo::public_notification_msg>(networking_msg);
                Printf(msg.get_ansi_color_code(), "[%s] %s", msg.get_type_name(), msg.text_content);
                break;
            }
            case bmmo::ImportantNotification: {
                bmmo::important_notification_msg msg{};
                msg.raw.write(static_cast<const char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();

                Printf(msg.get_ansi_color(), "[%s] (%u, %s): %s",
                        msg.get_type_name(), msg.player_id, get_player_name(msg.player_id), msg.chat_content);
                break;
            }
            case bmmo::PermanentNotification: {
                auto msg = bmmo::message_utils::deserialize<bmmo::permanent_notification_msg>(networking_msg);
                Printf(bmmo::color_code(msg.code), "[Bulletin] %s%s", msg.title,
                        msg.text_content.empty() ? " - Content cleared" : ": " + msg.text_content);
                permanent_notification_text_ = msg.text_content;
                break;
            }
            case bmmo::PopupBox: {
                auto msg = bmmo::message_utils::deserialize<bmmo::popup_box_msg>(networking_msg);
                Printf(bmmo::color_code(msg.code), "[Popup] {%s}: %s", msg.title, msg.text_content);
                break;
            }
            case bmmo::RestartRequest: {
                auto msg = bmmo::message_utils::deserialize<bmmo::restart_request_msg>(networking_msg);
                const bool restart = (msg.content.victim == own_id_);
                Printf(bmmo::color_code(msg.code), "(#%u, %s) requested to restart %s current level.",
                        msg.content.requester, get_player_name(msg.content.requester),
                        restart ? "your" : get_player_name(msg.content.victim) + "'s");
                if (restart)
                    send(bmmo::owned_simple_action_msg{.content = {
                        .type = bmmo::owned_simple_action_type::RestartRequestFailed,
                        .player_id = msg.content.requester,
                    }}, k_nSteamNetworkingSend_Reliable);
                break;
            }
            case bmmo::ScoreList: {
                auto msg = bmmo::message_utils::deserialize<bmmo::score_list_msg>(networking_msg);
                bool hs_mode = (msg.mode == bmmo::level_mode::Highscore);
                bmmo::ranking_entry::sort_rankings(msg.rankings, hs_mode);
                auto formatted_texts = bmmo::ranking_entry::get_formatted_rankings(
                        msg.rankings, msg.map.get_display_name(map_names_), hs_mode);
                for (const auto& [line, color]: formatted_texts)
                    Printf(color, line.c_str());
                break;
            }
            case bmmo::SoundData: {
                auto msg = bmmo::message_utils::deserialize<bmmo::sound_data_msg>(networking_msg);
                Printf(bmmo::color_code(msg.code), "Sound data received%s!", msg.caption.empty() ? "" : ": " + msg.caption);
                std::stringstream data_text;
                for (const auto& [frequency, duration]: msg.sounds)
                    data_text << ", (" << frequency << ", " << duration << ")";
                Printf("%s", data_text.str().erase(0, 2));
                break;
            }
            case bmmo::SoundStream: {
                bmmo::sound_stream_msg msg{};
                msg.raw.write(reinterpret_cast<char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.save_sound_file = options.save_sound_files;
                msg.save_to_pwd = true;
                if (!msg.deserialize()) {
                    Printf("Error receiving sound!");
                    break;
                }
                Printf(bmmo::color_code(msg.code), "Sound stream received%s!", msg.caption.empty() ? "" : ": " + msg.caption);
                Printf("Path: %s; Length: %d ms; Gain: %.2f; Pitch: %.2f.",
                    msg.path, msg.duration_ms, msg.gain, msg.pitch);
                break;
            }
            case bmmo::ActionDenied: {
                auto* msg = reinterpret_cast<bmmo::action_denied_msg*>(networking_msg->m_pData);
                Printf(bmmo::color_code(msg->code), "Action failed: %s", msg->content.to_string());
                break;
            }
            case bmmo::CheatToggle: {
                auto* msg = reinterpret_cast<bmmo::cheat_toggle_msg*>(networking_msg->m_pData);
                Printf(bmmo::color_code(msg->code), "[Server] toggled%s cheat %s%s!",
                    msg->content.notify ? "" : " your",
                    msg->content.cheated ? "on" : "off",
                    msg->content.notify ? " globally" : "");
                if (cheat == msg->content.cheated)
                    return;
                cheat = msg->content.cheated;
                bmmo::cheat_state_msg state_msg{};
                state_msg.content.cheated = cheat;
                state_msg.content.notify = false;
                send(state_msg, k_nSteamNetworkingSend_Reliable);
                break;
            }
            case bmmo::Countdown: {
                auto* msg = reinterpret_cast<bmmo::countdown_msg*>(networking_msg->m_pData);
                last_countdown_map_ = msg->content.map;
                using ct = bmmo::countdown_type;
                Printf(msg->content.type == ct::Go ? bmmo::color_code(msg->code) : bmmo::ansi::Default,
                    "[%u, %s]: %s%s - %s",
                    msg->content.sender,
                    clients_[msg->content.sender].name,
                    msg->content.map.get_display_name(map_names_),
                    msg->content.get_level_mode_label(),
                    std::map<ct, const char*>{
                        {ct::Ready, "Get Ready"}, {ct::ConfirmReady, "Confirm if you're ready"},
                        {ct::Go, "Go!"}, {ct::Countdown_1, "1"}, {ct::Countdown_2, "2"}, {ct::Countdown_3, "3"}
                    }[msg->content.type]);
                if (msg->content.type == ct::Go)
                    local_rankings_[last_countdown_map_.get_hash_bytes_string()] = {};
                break;
            }
            case bmmo::CurrentMap: {
                auto* msg = reinterpret_cast<bmmo::current_map_msg*>(networking_msg->m_pData);
                clients_[msg->content.player_id].current_map = msg->content.map;
                clients_[msg->content.player_id].current_sector = msg->content.sector;
                break;
            }
            case bmmo::CurrentSector: {
                auto* msg = reinterpret_cast<bmmo::current_sector_msg*>(networking_msg->m_pData);
                clients_[msg->content.player_id].current_sector = msg->content.sector;
                break;
            }
            case bmmo::DidNotFinish: {
                auto* msg = reinterpret_cast<bmmo::did_not_finish_msg*>(networking_msg->m_pData);
                std::string player_name = get_player_name(msg->content.player_id);
                Printf(
                    bmmo::color_code(msg->code),
                    "%s(#%u, %s) did not finish %s (furthest reach: sector %d).",
                    msg->content.cheated ? "[CHEAT] " : "",
                    msg->content.player_id, player_name,
                    msg->content.map.get_display_name(map_names_),
                    msg->content.sector
                );
                local_rankings_[msg->content.map.get_hash_bytes_string()].second.push_back({
                    {(bool)msg->content.cheated, player_name}, msg->content.sector});
                break;
            }
            case bmmo::LatencyData: {
                auto msg = bmmo::message_utils::deserialize<bmmo::latency_data_msg>(networking_msg);
                for (const auto& [id, ping]: msg.data) {
                    clients_[id].ping = ping;
                }
                break;
            }
            case bmmo::LevelFinishV2: {
                auto* msg = reinterpret_cast<bmmo::level_finish_v2_msg*>(networking_msg->m_pData);

                std::string player_name = get_player_name(msg->content.player_id),
                    formatted_score = msg->content.get_formatted_score();
                Printf(bmmo::color_code(msg->code),
                    "%s(#%u, %s) finished %s%s in %d%s place (score: %s; real time: %s).",
                    msg->content.cheated ? "[CHEAT] " : "",
                    msg->content.player_id, player_name,
                    msg->content.map.get_display_name(map_names_), get_level_mode_label(msg->content.mode),
                    msg->content.rank, bmmo::string_utils::get_ordinal_suffix(msg->content.rank),
                    formatted_score, msg->content.get_formatted_time());
                local_rankings_[msg->content.map.get_hash_bytes_string()].first.push_back({
                    {(bool)msg->content.cheated, player_name}, msg->content.mode,
                    msg->content.rank, msg->content.timeElapsed, formatted_score});
                break;
            }
            case bmmo::MapNames: {
                bmmo::map_names_msg msg{};
                msg.raw.write(static_cast<const char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();

                map_names_.insert(msg.maps.begin(), msg.maps.end());
                break;
            }
            case bmmo::NameUpdate: {
                auto msg = bmmo::message_utils::deserialize<bmmo::name_update_msg>(networking_msg);
                set_nickname(bmmo::name_validator::is_spectator(options.username)
                        ? bmmo::name_validator::get_spectator_nickname(msg.text_content) : msg.text_content);
                Printf(bmmo::ansi::WhiteInverse, "Your name has been changed to \"%s\" upon server request.",
                        nickname_);
                break;
            }
            case bmmo::OpState: {
                auto* msg = reinterpret_cast<bmmo::op_state_msg*>(networking_msg->m_pData);
                Printf("You have been %s Operator permission.", msg->content.op ? "granted" : "removed from",
                        bmmo::color_code(msg->code));
                break;
            }
            case bmmo::OwnedCheatToggle: {
                auto* msg = reinterpret_cast<bmmo::owned_cheat_toggle_msg*>(networking_msg->m_pData);
                Printf(bmmo::color_code(msg->code), "(#%u, %s) toggled cheat %s globally!",
                    msg->content.player_id, clients_[msg->content.player_id].name, msg->content.state.cheated ? "on" : "off");
                if (cheat == msg->content.state.cheated)
                    return;
                cheat = msg->content.state.cheated;
                bmmo::cheat_state_msg state_msg{};
                state_msg.content.cheated = cheat;
                state_msg.content.notify = false;
                send(state_msg, k_nSteamNetworkingSend_Reliable);
                break;
            }
            case bmmo::PlayerKicked: {
                bmmo::player_kicked_msg msg{};
                msg.raw.write(static_cast<const char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();

                Printf(bmmo::color_code(msg.code), "%s was kicked by %s%s%s.",
                    msg.kicked_player_name.c_str(),
                    (msg.executor_name == "")? "the server" : msg.executor_name.c_str(),
                    (msg.reason == "")? "" : (" (" + msg.reason + ")").c_str(),
                    msg.crashed ? " and crashed subsequently" : ""
                );
                break;
            }
            case bmmo::SimpleAction: {
                auto* msg = reinterpret_cast<bmmo::simple_action_msg*>(networking_msg->m_pData);
                switch (msg->content) {
                    using sa = bmmo::simple_action;
                    case sa::LoginDenied: {
                        Printf("Login denied.");
                        break;
                    }
                    case sa::CurrentMapQuery: {
                        bmmo::current_map_msg new_msg{};
                        send(new_msg, k_nSteamNetworkingSend_Reliable);
                        break;
                    }
                    case sa::TriggerFatalError: {
                        trigger_fatal_error();
                        break;
                    }
                    case sa::Unknown: {
                        Printf("Unknown action request received.");
                        break;
                    }
                    default:
                        break;
                }
                break;
            }
            case bmmo::OwnedSimpleAction: {
                auto* msg = reinterpret_cast<bmmo::owned_simple_action_msg*>(networking_msg->m_pData);
                switch (msg->content.type) {
                    using osa = bmmo::owned_simple_action_type;
                    case osa::RestartRequestFailed: {
                        Printf(bmmo::ansi::Italic, "(#%u, %s)'s restart request failed.",
                            msg->content.player_id, get_player_name(msg->content.player_id));
                        break;
                    }
                    default:
                        break;
                }
                break;
            }
            default:
                break;
        }
    }

    int poll_incoming_messages() override {
        int msg_count = interface_->ReceiveMessagesOnConnection(connection_, incoming_messages_, ONCE_RECV_MSG_COUNT);
        if (msg_count == 0)
            return 0;
        if (msg_count < 0)
            FatalError("Error checking for messages.");
        assert(msg_count > 0);

        for (int i = 0; i < msg_count; ++i) {
            if (recorder_mode_)
                enqueue_log_message(incoming_messages_[i]);
            on_message(incoming_messages_[i]);
            incoming_messages_[i]->Release();
        }

        return msg_count;
    }

    void enqueue_log_message(const ISteamNetworkingMessage* msg) {
        bmmo::record_entry entry(SteamNetworkingUtils()->GetLocalTimestamp(), msg->m_cbSize, reinterpret_cast<std::byte*>(msg->m_pData));

        std::unique_lock<std::mutex> lk(message_queue_mutex_);
        message_queue_.emplace_back(std::move(entry));
        message_available_cv_.notify_one();
    }

    std::mutex message_queue_mutex_;
    std::mutex message_available_mutex_;
    std::condition_variable message_available_cv_;
    std::list<bmmo::record_entry> message_queue_;
    std::ofstream record_stream_;
    const std::string packet_save_path = []() -> std::string {
        std::time_t t = std::time(nullptr);
        char path[32];
        std::strftime(path, sizeof(path), "packets/%Y%m%d%H%M/", std::localtime(&t));
        return path;
    }();

    void poll_connection_state_changes() override {
        this_instance_ = this;
        interface_->RunCallbacks();
    }

    void poll_local_state_changes() override {
        std::string input;
        std::cin >> input;
        if (input == "stop") {
            shutdown();
        } else if (input == "1") {
            bmmo::ball_state_msg msg;
            msg.content.position.x = 1;
            msg.content.rotation.y = 2;
            send(msg, k_nSteamNetworkingSend_UnreliableNoDelay);
        }
    }

    std::mutex startup_mutex_;
    std::condition_variable startup_cv_;
    HSteamNetConnection connection_ = k_HSteamNetConnection_Invalid;
    HSteamNetConnection own_id_{};
    std::string nickname_;
    uint8_t uuid_[16]{};
    std::unordered_map<HSteamNetConnection, client_data> clients_;
    std::unordered_map<std::string, std::string> map_names_;
    std::string permanent_notification_text_;
    bmmo::map last_countdown_map_{};
    bmmo::ranking_entry::map_rankings local_rankings_{};
    bmmo::timed_ball_state_msg local_state_msg_{};
    std::atomic_bool print_states_ = false, recorder_mode_ = false;
};

// parse command line arguments (server/name/uuid/help/version) with getopt
int parse_args(int argc, char** argv) {
    enum option_values { NoSoundFiles = UINT8_MAX + 1, AutoFlush, IndividualPackets };
    static struct option long_options[] = {
        {"recorder-mode", required_argument, 0, 'r'},
        {"server", required_argument, 0, 's'},
        {"name", required_argument, 0, 'n'},
        {"uuid", required_argument, 0, 'u'},
        {"log", required_argument, 0, 'l'},
        {"detail", required_argument, 0, 'd'},
        {"print", no_argument, 0, 'p'},
        {"help", no_argument, 0, 'h'},
        {"version", no_argument, 0, 'v'},
        {"no-sound-files", no_argument, 0, NoSoundFiles},
        {"auto-flush", no_argument, 0, AutoFlush},
        {"individual-packets", no_argument, 0, IndividualPackets},
        {0, 0, 0, 0}
    };
    int opt, opt_index = 0;
    while ((opt = getopt_long(argc, argv, "s:n:u:l:d:rphv", long_options, &opt_index))!= -1) {
        switch (opt) {
            case 's':
                options.server_addr = optarg; break;
            case 'n':
                options.username = optarg; break;
            case 'u':
                options.uuid = optarg; break;
            case 'l':
                options.log_path = optarg; break;
            case 'd':
                options.detail = (decltype(options.detail))(option_t{}.detail + std::clamp(atoi(optarg), 0, 2));
                break;
            case 'r':
                options.recorder_mode = true;
                if (options.username == option_t{}.username) options.username = "*FlightRecorder";
                if (options.uuid == option_t{}.uuid) options.uuid = "01020304-0506-0708-090a-0b0c0d0e0f00";
                break;
            case 'p':
                options.print_states = true; break;
            case NoSoundFiles:
                options.save_sound_files = false; break;
            case AutoFlush:
                bmmo::set_auto_flush_log(true); break;
            case IndividualPackets:
                options.individual_packets = true; break;
            case 'h':
                printf("Usage: %s [OPTION]...\n", argv[0]);
                puts("Options:");
                puts("  -s, --server=ADDRESS\t Connect to the server at ADDRESS instead (default: 127.0.0.1:26676).");
                puts("  -n, --name=NAME\t Set your name to NAME (default: \"MockClient\")");
                puts("  -u, --uuid=UUID\t Set your UUID to UUID (default: \"00010002-0003-0004-0005-000600070008\")");
                puts("  -l, --log=PATH\t Write log to the file at PATH in addition to stdout.");
                puts("      --auto-flush\t Automatically flush the log file after each output.");
                puts("  -d, --detail=LEVEL\t Set the detail level (0 to 2, from low to high) of output (default: 0).");
                puts("  -r, --recorder-mode\t Record data received from the server and save them to a binary file.");
                puts("      --individual-packets  Record and save each packet individually. Requires --recorder-mode.");
                puts("                            Use carefully as it may generate a large number of files.");
                puts("      --no-sound-files\t Discard sound files sent by the server.");
                puts("  -p, --print\t\t Print player state changes.");
                puts("  -h, --help\t\t Display this help and exit.");
                puts("  -v, --version\t\t Display version information and exit.");
                return -1;
            case 'v':
                puts("Ballance MMO mock client by Swung0x48 and BallanceBug.");
                printf("Build time: \t%s.\n", bmmo::string_utils::get_build_time_string().c_str());
                printf("Version: %s.\n", bmmo::current_version.to_string().c_str());
                return -1;
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    if (parse_args(argc, argv) != 0)
        return 0;

    if (!options.log_path.empty()) {
        FILE* log_file = fopen(options.log_path.c_str(), "a");
        if (log_file == nullptr) {
            std::cerr << "Fatal: failed to open log file." << std::endl;
            return 1;
        }
        bmmo::set_log_file(log_file);
    }

    std::cout << "Initializing sockets..." << std::endl;
    client::init_socket();

    std::cout << "Creating client instance..." << std::endl;
    client client;
    client.set_nickname(options.username);
    client.set_uuid(options.uuid);
    client.set_print_states(options.print_states);
    client.set_logging_level(options.detail);
    if (options.recorder_mode) client.setup_recorder();

    std::cout << "Connecting to server..." << std::endl;
    if (!client.connect(options.server_addr)) {
        std::cerr << "Cannot connect to server." << std::endl;
        return 1;
    }

    std::thread client_thread([&client]() { client.run(); });

    bmmo::console console;
    console.register_command("help", [&] { Printf(console.get_help_string().c_str()); });
    console.register_command("stop", [&] { client.shutdown(); });
    auto pos_function = [&](bool translate = false) {
        std::mt19937 random_gen(std::random_device{}());
        std::uniform_real_distribution<float> pos_dist(-10, 10), rot_dist(-1, 1);
        auto& msg = client.get_local_state_msg();
        float* const pos = msg.content.position.v;
        for (int i = 0; i < 3; ++i) {
            if (!console.empty())
                pos[i] = console.get_next_double() + ((translate) ? pos[i] : 0.0f);
            else
                pos[i] = pos_dist(random_gen) + ((translate) ? pos[i] : 0.0f);
        }
        float* const rot = msg.content.rotation.v;
        float last_squared = 1;
        for (int i = 0; i < 4; ++i) {
            if (!console.empty())
                rot[i] = console.get_next_double() + ((translate) ? rot[i] : 0.0f);
            else
                rot[i] = rot_dist(random_gen) + ((translate) ? rot[i] : 0.0f);
            if (i != 3) last_squared -= rot[i] * rot[i];
        }
        rot[3] = std::sqrt(last_squared);
        msg.content.timestamp = SteamNetworkingUtils()->GetLocalTimestamp();
        Printf("Sending ball state message: (%.2f, %.2f, %.2f), (%.3f, %.3f, %.3f, %.3f)",
            pos[0], pos[1], pos[2], rot[0], rot[1], rot[2], rot[3]);
//        for (int i = 0; i < 50; ++i)
        client.send(msg, k_nSteamNetworkingSend_UnreliableNoDelay);
    };
    console.register_command("move", pos_function);
    console.register_command("translate", std::bind(pos_function, true));
    console.register_command("getinfo-detailed", [&] {
        std::atomic_bool running = true;
        std::thread output_thread([&]() {
            while (running) {
                // bmmo::ball_state_msg msg;
                // msg.content.position.x = 1;
                // msg.content.rotation.y = 2;
                // for (int i = 0; i < 50; ++i)
                //     client.send(msg, k_nSteamNetworkingSend_UnreliableNoDelay);

                std::cout << client.get_detailed_info() << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds (500));
                std::cout << "\033[2J\033[H" << std::flush;
            }
        });
        while (running) {
            std::string in;
            std::cin >> in;
            if (in == "q") {
                running = false;
            }
        }
        if (output_thread.joinable())
            output_thread.join();
    });
    console.register_command("getinfo", [&] {
        auto status = client.get_info();
        Printf("Ping: %dms\n", status.m_nPing);
        Printf("ConnectionQualityRemote: %.2f%\n", status.m_flConnectionQualityRemote * 100.0f);
        auto l_status = client.get_lane_info();
        Printf("PendingReliable: %d", l_status.m_cbPendingReliable);
    });
    console.register_command("reconnect", [&] {
        if (client_thread.joinable())
            client_thread.join();

        if (!console.empty())
            options.server_addr = console.get_next_word();

        if (!client.connect(options.server_addr)) {
            std::cerr << "Cannot connect to server." << std::endl;
            return;
        }

        client_thread = std::thread([&client]() { client.run(); });
        client.wait_till_started();
    });
    console.register_command("cheat", [&] {
        bmmo::cheat_toggle_msg msg{};
        msg.content.cheated = (console.get_next_word(true) == "on") ? true : false;
        client.send(msg, k_nSteamNetworkingSend_Reliable);
    });
    console.register_command("cheat-self", [&] {
        if (!console.empty())
            cheat = (console.get_next_word(true) == "on") ? true : false;
        else
            cheat = !cheat;
        bmmo::cheat_state_msg msg;
        msg.content.cheated = cheat;
        client.send(msg, k_nSteamNetworkingSend_Reliable);
    });
    console.register_command("list", [&] { client.print_clients(); });
    console.register_aliases("list", {"l"});
    console.register_command("print", [&] {
        options.print_states = !options.print_states;
        client.set_print_states(options.print_states);
    });
    console.register_command("teleport", [&] { client.teleport_to(console.get_next_client_id()); });
    console.register_command("balltype", [&] {
        auto& msg = client.get_local_state_msg();
        msg.content.type = console.get_next_int();
        client.send(msg, k_nSteamNetworkingSend_Reliable);
    });
    console.register_command("whisper", [&] {
        HSteamNetConnection dest = console.get_next_client_id();
        client.whisper_to(dest, console.get_rest_of_line());
    });
    console.register_command("getmap", [&] { client.print_player_maps(); });
    console.register_command("listmap", [&] { client.print_maps(); });
    auto get_map_from_input = [&](bool with_name = false) {
        auto input_map = console.get_next_map(with_name);
        if (with_name && !input_map.name.empty()) {
            bmmo::map_names_msg name_msg{};
            name_msg.maps.emplace(input_map.get_hash_bytes_string(), input_map.name);
            name_msg.serialize();
            client.send(name_msg.raw.str().data(), name_msg.size(), k_nSteamNetworkingSend_Reliable);
        }
        return input_map;
    };
    console.register_command("countdown", [&] {
        if (console.empty()) {
            Printf(R"(Error: please specify the map to countdown (hint: use "getmap" and "listmap").)");
            Printf("Usage: \"countdown level|<hash> <level number> [mode] [type]\".");
            Printf(R"(<type>: {"4": "Get ready", "5": "Confirm ready", "": "auto countdown"})");
            return;
        }
        bmmo::countdown_msg msg{.content = {.map = get_map_from_input()}};
        if (!console.empty() && console.get_next_word(true) == "hs")
            msg.content.mode = bmmo::level_mode::Highscore;
        if (console.empty()) {
            for (int i = 3; i >= 0; --i) {
                msg.content.type = static_cast<bmmo::countdown_type>(i);
                client.send(msg, k_nSteamNetworkingSend_Reliable);
                if (i != 0) std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        } else {
            msg.content.type = static_cast<bmmo::countdown_type>(console.get_next_int());
            client.send(msg, k_nSteamNetworkingSend_Reliable);
        }
    });
    console.register_aliases("countdown", {"cd"});
    console.register_command("announce", [&] {
        using in_msg = bmmo::important_notification_msg;
        in_msg msg{};
        msg.chat_content = console.get_rest_of_line();
        msg.type = (console.get_command_name() == "notice") ? in_msg::Notice : in_msg::Announcement;
        msg.serialize();
        client.send(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
    });
    console.register_aliases("announce", {"notice"});
    console.register_command("kick", [&] {
        auto name = console.get_next_word();
        bmmo::kick_request_msg msg{};
        if (console.get_command_name() == "crash")
            msg.crash = true;
        if (name[0] == '#') msg.player_id = console.get_next_client_id();
        else msg.player_name = name;
        msg.reason = console.get_rest_of_line();
        msg.serialize();
        client.send(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
    });
    console.register_aliases("kick", {"crash"});
    console.register_command("say", [&] {
        bmmo::chat_msg msg{};
        msg.chat_content = console.get_rest_of_line();
        msg.serialize();
        client.send(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
    });
    console.register_aliases("say", {"s"});
    console.register_command("getpos", [&] { client.print_positions(); });
    console.register_command("setmap", [&] {
        if (console.empty()) {
            Printf(R"(Usage: "setmap level <level number>", "setmap <hash> [<level number> <name>]".)");
            return;
        }
        auto input_map = get_map_from_input(true);
        bmmo::current_map_msg msg{.content = {.map = input_map, .type = bmmo::current_map_state::EnteringMap}};
        client.set_own_map(msg.content.map, input_map.name);
        client.send(msg, k_nSteamNetworkingSend_Reliable);
    });
    console.register_command("setsector", [&] {
        bmmo::current_sector_msg msg{};
        msg.content.sector = console.get_next_int();
        client.set_own_sector(msg.content.sector);
        client.send(msg, k_nSteamNetworkingSend_Reliable);
    });
    console.register_command("dnf", [&] {
        if (console.empty()) return Printf("Usage: \"dnf <map> <sector>\".");
        client.send(bmmo::did_not_finish_msg{.content = {.cheated = cheat, .map = get_map_from_input(),
            .sector = console.get_next_int()}}, k_nSteamNetworkingSend_Reliable);
    });
    console.register_command("win", [&] {
        if (console.empty()) return Printf("Usage: \"win <map> <mode> <points> <lives> <time>\".");
        auto map = get_map_from_input();
        auto mode = console.get_next_word(true) == "hs" ? bmmo::level_mode::Highscore : bmmo::level_mode::Speedrun;
        client.send(bmmo::level_finish_v2_msg{.content = {
            .points = console.get_next_int(), .lives = console.get_next_int(),
            .lifeBonus = 200, .levelBonus = map.level * 100, .timeElapsed = (float) console.get_next_double(),
            .startPoints = 1000, .cheated = cheat, .mode = mode, .map = map
        }}, k_nSteamNetworkingSend_Reliable);
    });
    console.register_command("bulletin", [&] {
        bmmo::permanent_notification_msg msg{};
        msg.text_content = console.get_rest_of_line();
        msg.serialize();
        client.send(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
    });
    console.register_command("getbulletin", [&] { client.print_bulletin(); });
    console.register_command("scores", [&] {
        if (console.empty()) return Printf("Usage: \"scores [local] <hs|sr> [map]\"");
        auto mode = console.get_next_word(true);
        // bool hs_mode = (console.get_next_word(true) == "hs");
        bool use_local_data = false;
        if (mode == "local") {
            use_local_data = true;
            mode = console.get_next_word(true);
        }
        bmmo::score_list_msg msg{};
        msg.map = console.empty() ? client.get_last_countdown_map() : get_map_from_input();
        msg.mode = (mode == "hs") ? bmmo::level_mode::Highscore : bmmo::level_mode::Speedrun;
        if (!use_local_data) {
            msg.serialize();
            client.send(msg.raw.str().data(), msg.size(), k_nSteamNetworkingSend_Reliable);
            return;
        }
        auto& rankings = client.get_local_rankings();
        auto ranks_it = rankings.find(msg.map.get_hash_bytes_string());
        if (ranks_it == rankings.end() || (ranks_it->second.first.empty() && ranks_it->second.second.empty())) {
            Printf(bmmo::ansi::BrightRed, "Error: ranking info not found for the specified map.");
            return;
        }
        msg.serialize(ranks_it->second);
        client.receive(msg.raw.str().data(), msg.size());
    });
    console.register_command("restartlevel", [&] {
        bmmo::restart_request_msg msg{.content = {.victim = console.get_next_client_id()}};
        client.send(msg, k_nSteamNetworkingSend_Reliable);
    });
    console.register_command("flushlog", bmmo::flush_log);

    client.wait_till_started();
    std::thread record_thread;
    if (options.recorder_mode) {
        record_thread = std::thread([&client]() {
            while (client.running())
                client.write_record();
        });
    }
    while (client.running()) {
        std::string input;
        if (!console.read_input(input)) {
            puts("stop");
            client.shutdown();
            break;
        };
        LogFileOutput(("> " + input).c_str());

        // std::cin >> input;
        if (!console.execute(input) && !console.get_command_name().empty()) {
            std::string extra_text;
            if (auto hints = console.get_command_hints(true); !hints.empty())
                extra_text = " Did you mean: " + bmmo::message_utils::join_strings(hints, 0, ", ") + "?";
            Printf("Error: unknown command \"%s\".%s", console.get_command_name(), extra_text);
        }
    }

    std::cout << "Stopping..." << std::endl;
    if (client_thread.joinable())
        client_thread.join();
    if (record_thread.joinable())
        record_thread.join();
    client::destroy();
}
