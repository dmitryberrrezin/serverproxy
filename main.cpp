#include "helpfunctions.h"

#include <cstdint>
#include <list>
#include <vector>
#include <yaml-cpp/yaml.h>
#include <uv.h>

static int id = 0;

static uv_loop_t* loop;
static std::map<uint16_t, uint32_t> port_protocol; /* key - port, value - protocol */
std::map<uv_stream_t*, std::vector<uint8_t>> packet_storage;

static void alloc_buffer(uv_handle_t* /*handle*/, size_t suggested_size, uv_buf_t* buf)
{
    buf->base = reinterpret_cast<char*>(malloc(suggested_size));
    buf->len = suggested_size;
}

void close_client_cb(uv_handle_t *handle)
{
    MsgToServer::SendConnect(reinterpret_cast<uv_stream_t*>(handle), false);
    client_map.erase((reinterpret_cast<client_data*>(handle->data))->id);
    free(handle->data);
    free(handle);
}

void close_server_cb(uv_handle_t* handle) /*unsubscribe from everything */
{
    for(auto &node : protocol_map)
    {
        if(node.second == reinterpret_cast<uv_stream_t*>(handle))
            node.second = nullptr;
    }

    for(auto &node : observe_map)
    {
        if(node.second.find(reinterpret_cast<uv_stream_t*>(handle)) != node.second.end())
        {
            node.second.erase(reinterpret_cast<uv_stream_t*>(handle));
        }
    }

    for(int i = 0; i < 7; ++i)
    {
        auto it = log_map[i].find(reinterpret_cast<uv_stream_t*>(handle));
        if(it != log_map[i].end())
        {
            log_map[i].erase(it);
            break;
        }
    }

    auto it = std::find(absolute_observe_list.begin(), absolute_observe_list.end(), reinterpret_cast<uv_stream_t*>(handle));
    if(it != absolute_observe_list.end())
        absolute_observe_list.erase(it);

    packet_storage.erase(reinterpret_cast<uv_stream_t*>(handle));
    free(handle);
}

static void echo_read_client(uv_stream_t* client, ssize_t nread, const uv_buf_t* buf)
{
    if (nread > 0)
    {
        MsgToServer::SendData(client, reinterpret_cast<uint8_t*>(buf->base), nread);
        free(buf->base);
        return;
    }
    if (nread < 0)
    {
        if (nread != UV_EOF)
        {
            fprintf(stderr, "Read error %s\n", uv_err_name(static_cast<int>(nread)));
        }
        else
        {
            uv_close(reinterpret_cast<uv_handle_t*>(client), close_client_cb);
            printf("client disconnect\n");
        }
        free(buf->base);
    }
}

static void echo_read_server(uv_stream_t* server, ssize_t nread, const uv_buf_t* buf)
{
    if (nread > 0)
    {
        auto &this_server_storage = packet_storage.find(server)->second;
        std::copy(&buf->base[0], &buf->base[nread], std::back_inserter(this_server_storage));

        if(this_server_storage.size() >= HEADER_SIZE_FROM_SERVER)
        {
            uint32_t packet_size = 0;
            memcpy(&packet_size, &this_server_storage[4], sizeof(packet_size));

            while(packet_size <= this_server_storage.size())
            {
                std::vector<uint8_t> for_parse;
                std::copy(&this_server_storage[0], &this_server_storage[packet_size], std::back_inserter(for_parse));
                this_server_storage.erase(this_server_storage.begin(),this_server_storage.begin() + packet_size);
                ParsePacketFromServer(server, for_parse);

                if(this_server_storage.size() >= HEADER_SIZE_FROM_SERVER)
                    memcpy(&packet_size, &this_server_storage[4], sizeof(packet_size));
                else
                    break;
            }
        }

        free(buf->base);
        return;
    }
    if (nread < 0)
    {
        if (nread != UV_EOF)
            fprintf(stderr, "Read from server error %s\n", uv_err_name(static_cast<int>(nread)));
        uv_close(reinterpret_cast<uv_handle_t*>(server), close_server_cb);
        printf("server disconnect\n");
    }
    free(buf->base);
}

static void new_client_connection(uv_stream_t* listener, int status)
{
    if (status < 0)
    {
        fprintf(stderr, "New connection error %s\n", uv_strerror(status));
        return;
    }
    auto client = reinterpret_cast<uv_stream_t*>(malloc(sizeof(uv_stream_t)));
    uv_tcp_init(loop, reinterpret_cast<uv_tcp_t*>(client));

    id++;
    auto d = reinterpret_cast<client_data*>(malloc(sizeof(client_data)));
    d->id = id;
    d->port_server = reinterpret_cast<listener_data*>(listener->data)->listening_port;
    d->protocol = port_protocol.find(d->port_server)->second;
    client->data = d;

    int r = uv_accept(listener, client);
    if(r == 0)
    {
        d->ip = GetInt32IP(client, &d->port_client);
        client_map.emplace(id, client);
        uv_read_start(client, alloc_buffer, echo_read_client);
        MsgToServer::SendConnect(client, true);

        std::stringstream ss;
        ss << "New client connected, id: " << id << ", protocol: " << d->protocol;
        printf("%s\n", ss.str().c_str());
        MsgToServer::SendMessage(LEVEL_INFO, ss.str().c_str());
    }
    else
    {
        uv_close(reinterpret_cast<uv_handle_t*>(client), nullptr);
        d->ip = GetInt32IP(client, &d->port_client);
        MsgToServer::SendError(client, r);
        free(d);
        MsgToServer::SendMessage(LEVEL_CRIT, "Client accept failed");
    }
}

static void new_server_connection(uv_stream_t* listener, int status)
{
    if (status < 0)
    {
        fprintf(stderr, "New connection error %s\n", uv_strerror(status));
        return;
    }
    auto server = reinterpret_cast<uv_tcp_t*>(malloc(sizeof(uv_tcp_t)));
    uv_tcp_init(loop, server);

    if (uv_accept(listener, reinterpret_cast<uv_stream_t*>(server)) == 0)
    {
        uv_read_start(reinterpret_cast<uv_stream_t*>(server), alloc_buffer, echo_read_server);

        packet_storage.emplace(reinterpret_cast<uv_stream_t*>(server), std::vector<uint8_t>());
        printf("New server connected\n");
        MsgToServer::SendMessage(LEVEL_INFO, "New server connected");
    }
    else
    {
        uv_close(reinterpret_cast<uv_handle_t*>(server), nullptr);
        fprintf(stderr, "Server connection failed\n");
        MsgToServer::SendMessage(LEVEL_CRIT, "Server connection failed");
    }
}

static void listeners_initialization(std::list<int> &ports,
                                     std::vector<uv_tcp_t*> &listeners,
                                     void(*connection_cb)(uv_stream_t* /*handle*/, int /*status*/))
{
    struct sockaddr_in addr{};
    for(int i = 0; !ports.empty(); ++i)
    {
        auto listener = reinterpret_cast<uv_tcp_t*>(malloc(sizeof(uv_tcp_t)));
        auto d = reinterpret_cast<listener_data*>(malloc(sizeof(listener_data)));
        d->listening_port = ports.front();
        listener->data = d;
        listeners.push_back(listener);
        uv_tcp_init(loop, listener);
        uv_ip4_addr(IP, ports.front(), &addr);
        uv_tcp_bind(listener, reinterpret_cast<const struct sockaddr*>(&addr), 0);
        int r = uv_listen(reinterpret_cast<uv_stream_t*>(listener), DEFAULT_BACKLOG, connection_cb);

        if(r != 0)
        {
            std::stringstream ss;
            ss << "Start listener " << i << " error: " << uv_strerror(r);
            MsgToServer::SendMessage(LEVEL_CRIT, ss.str().c_str());
            fprintf(stderr, "Start listener %d error: %s\n", i, uv_strerror(r));
        }
        ports.pop_front();
    }
}

static void ReadConfig(const char *path,
                       std::list<int> &client_ports,
                       std::list<int> &server_ports)
{
    YAML::Node config = YAML::LoadFile(path);

    YAML::Node protocol_types = config["protocol types"];
    for(size_t i = 0; i < protocol_types.size(); ++i)
    {
        auto prtcl = protocol_types[i].as<uint32_t>();
        if(prtcl <= 6)
            throw std::logic_error("an attempt to use reserved protocol id");
        protocol_map.emplace(prtcl, nullptr);
        observe_map.emplace(prtcl, std::set<uv_stream_t*>());
    }

    YAML::Node node_server_ports = config["server ports"];
    for(size_t i = 0; i < node_server_ports.size(); ++i)
    {
        server_ports.push_back(node_server_ports[i].as<int>());
    }

    YAML::Node client_ports_node = config["client ports"];
    for(auto it = client_ports_node.begin(); it != client_ports_node.end(); ++it)
    {
        int c_port = it->first.as<int>();
        auto c_protocol = it->second.as<uint32_t>();

        if(std::find(server_ports.begin(), server_ports.end(), c_port) != server_ports.end())
            throw std::logic_error("duplicate ports");
        if(protocol_map.find(c_protocol) == protocol_map.end())
            throw std::logic_error("binding to unknown protocol");

        client_ports.push_back(c_port);
        port_protocol.emplace(c_port, c_protocol);
    }
}

int main(int argc, char* argv[])
{
    const char* config_path;
    if (argc <= 1)
    {
        config_path = "config.yaml";
    }
    else
    {
        config_path = argv[1];
    }

    std::vector<uv_tcp_t*> server_listeners;
    std::vector<uv_tcp_t*> client_listeners;

    std::list<int> server_ports;
    std::list<int> client_ports;

    try
    {
        ReadConfig(config_path, client_ports, server_ports);
    }
    catch (std::exception &ex)
    {
        printf("%s\n", ex.what());
        return -1;
    }

    for(int i = 0; i < 7; ++i)
    {
        log_map.emplace(i, std::set<uv_stream_t*>());
    }

    loop = uv_default_loop();

    listeners_initialization(client_ports, client_listeners, new_client_connection);
    listeners_initialization(server_ports, server_listeners, new_server_connection);
    return uv_run(loop, UV_RUN_DEFAULT);
}