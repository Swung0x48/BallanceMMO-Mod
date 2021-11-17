#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
#include <steam/steam_api.h>
#endif

#include "role.hpp"
#include <vector>
#include "common.hpp"
#include "message/message_all.hpp"

struct client_data {
    std::string name;
};

class server : public role {
public:
    explicit server(uint16_t port) {
        port_ = port;
    }

    void run() override {
        if (!setup())
            FatalError("Server failed on setup.");

        running_ = true;
        while (running_) {
            update();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
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

    void shutdown() {
        running_ = false;
//        if (server_thread_.joinable())
//            server_thread_.join();
    }

protected:
    bool setup() {
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

        return true;
    }

    void on_connection_status_changed(SteamNetConnectionStatusChangedCallback_t* pInfo) override {
        switch (pInfo->m_info.m_eState) {
            case k_ESteamNetworkingConnectionState_None:
                // NOTE: We will get callbacks here when we destroy connections.  You can ignore these.
                break;

            case k_ESteamNetworkingConnectionState_ClosedByPeer:
            case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
                // Ignore if they were not previously connected.  (If they disconnected
                // before we accepted the connection.)
                if (pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connected) {

                    // Locate the client.  Note that it should have been found, because this
                    // is the only codepath where we remove clients (except on shutdown),
                    // and connection change callbacks are dispatched in queue order.
                    auto itClient = clients_.find(pInfo->m_hConn);
                    assert(itClient != clients_.end());

                    // Select appropriate log messages
                    const char* pszDebugLogAction;
                    if (pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
                        pszDebugLogAction = "problem detected locally";
                    } else {
                        // Note that here we could check the reason code to see if
                        // it was a "usual" connection or an "unusual" one.
                        pszDebugLogAction = "closed by peer";
                    }

                    // Spew something to our own log.  Note that because we put their nick
                    // as the connection description, it will show up, along with their
                    // transport-specific data (e.g. their IP address)


                    clients_.erase(itClient);
                } else {
                    assert(pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connecting);
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

                printf("Connection request from %s\n", pInfo->m_info.m_szConnectionDescription);

                // A client is attempting to connect
                // Try to accept the connection.
                if (interface_->AcceptConnection(pInfo->m_hConn) != k_EResultOK) {
                    // This could fail.  If the remote host tried to connect, but then
                    // disconnected, the connection may already be half closed.  Just
                    // destroy whatever we have on our side.
                    interface_->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
                    printf("Can't accept connection.  (It was already closed?)\n");
                    break;
                }

                // Assign the poll group
                if (!interface_->SetConnectionPollGroup(pInfo->m_hConn, poll_group_)) {
                    interface_->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
                    printf("Failed to set poll group?");
                    break;
                }

                // Generate a random nick.  A random temporary nick
                // is really dumb and not how you would write a real chat server.
                // You would want them to have some sort of signon message,
                // and you would keep their client in a state of limbo (connected,
                // but not logged on) until them.  I'm trying to keep this example
                // code really simple.
                char nick[64];
                sprintf(nick, "BraveWarrior%d", 10000 + (rand() % 100000));

                // Add them to the client list, using std::map wacky syntax
                clients_[pInfo->m_hConn] = {nick};
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
        assert(client_it != clients_.end());

        auto* raw_msg = reinterpret_cast<bmmo::general_message*>(networking_msg->m_pData);
        switch (raw_msg->opcode) {
            case bmmo::LoginRequest: {
//                assert(*(reinterpret_cast<uint32_t*>(raw_msg->content)) == strlen((const char*)(raw_msg->content) + sizeof(uint32_t)));
//                std::cout << (const char*)(raw_msg->content) + sizeof(uint32_t) << " logged in!" << std::endl;
//                clients_[msg->m_conn] = {(const char*)(raw_msg->content) + sizeof(uint32_t)};
//                interface_->SetConnectionName(msg->m_conn, (const char*)(raw_msg->content) + sizeof(uint32_t));
                bmmo::login_request_msg msg;
                msg.raw.write(static_cast<const char*>(networking_msg->m_pData), networking_msg->m_cbSize);
                msg.deserialize();
                std::cout << msg.nickname << " logged in!" << std::endl;
                break;
            }
            case bmmo::LoginAccepted:
                break;
            case bmmo::LoginDenied:
                break;
            case bmmo::PlayerDisconnect:
                break;
            case bmmo::PlayerConnected:
                break;
            case bmmo::Ping:
                break;
            case bmmo::BallState: {
                auto* state_msg = reinterpret_cast<bmmo::ball_state_msg*>(networking_msg->m_pData);

                std::cout << "(" <<
                          state_msg->state.position.x << ", " <<
                          state_msg->state.position.y << ", " <<
                          state_msg->state.position.z << "), (" <<
                          state_msg->state.quaternion.x << ", " <<
                          state_msg->state.quaternion.y << ", " <<
                          state_msg->state.quaternion.z << ", " <<
                          state_msg->state.quaternion.w << ")" << std::endl;
                break;
            }
            case bmmo::KeyboardInput:
                break;
        }

        // TODO: replace with actual message data structure handling
        std::string str;
        str.assign((const char*)networking_msg->m_pData, networking_msg->m_cbSize);

        std::cout << client_it->second.name << ": " << str << std::endl;
        interface_->SendMessageToConnection(client_it->first, str.c_str(), str.length() + 1,
                                            k_nSteamNetworkingSend_Reliable,
                                            nullptr);
    }

    void poll_incoming_messages() override {
        while (running_) {
            ISteamNetworkingMessage* incoming_message = nullptr;
            int msg_count = interface_->ReceiveMessagesOnPollGroup(poll_group_, &incoming_message, 1);
            if (msg_count == 0)
                break;
            if (msg_count < 0)
                FatalError("Error checking for messages.");
            assert(msg_count == 1 && incoming_message);

            on_message(incoming_message);
            incoming_message->Release();
        }
    }

    uint16_t port_ = 0;
    HSteamListenSocket listen_socket_ = k_HSteamListenSocket_Invalid;
    HSteamNetPollGroup poll_group_ = k_HSteamNetPollGroup_Invalid;
//    std::thread server_thread_;
    std::unordered_map<HSteamNetConnection, client_data> clients_;
};

int main() {
    std::cout << "Initializing sockets..." << std::endl;
    server::init_socket();

    uint16_t port = 26676;
    std::cout << "Starting server at port " << port << std::endl;
    server server(port);

    std::cout << "Bootstrapping server..." << std::endl;
    std::thread server_thread([&server]() { server.run(); });
    std::cout << "Server started!" << std::endl;


    do {
        std::cout << "\r>" << std::flush;
        std::string cmd;
        std::cin >> cmd;
        if (cmd == "stop") {
            server.shutdown();
        }
    } while (server.running());

    std::cout << "Stopping..." << std::endl;
    if (server_thread.joinable())
        server_thread.join();
    server::destroy();
}