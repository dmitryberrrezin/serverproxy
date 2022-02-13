#include "helpfunctions.h"
#include <chrono>
#include <algorithm>
#include <sstream>

std::map<uint64_t, uv_stream_t*> client_map;
std::map<uint32_t, uv_stream_t*> protocol_map;
std::map<uint32_t, std::set<uv_stream_t*>> observe_map;
std::list<uv_stream_t*> absolute_observe_list;
std::map<uint8_t, std::set<uv_stream_t*>> log_map;

void close_client_cb(uv_handle_t *handle);
void close_server_cb(uv_handle_t *handle);

uint64_t CurrentTimestamp()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

uint32_t GetInt32IP (uv_stream_t* handle, uint16_t* port)
{
    struct sockaddr_in addr{};
    int len = sizeof(addr);
    uv_tcp_getpeername(reinterpret_cast<uv_tcp_t*>(handle),
                       reinterpret_cast<struct sockaddr*>(&addr),
                       &len);
    if(port != nullptr)
        *port = addr.sin_port;
    return addr.sin_addr.s_addr;  /*little-endian*/
}

uv_buf_t InitPacketToServer(uv_stream_t* client,
                            PACKET_TYPE_FOR p_type,
                            const uint8_t* add_data,
                            size_t add_len)
{
    uint32_t version = CURRENT_PROTOCOL_VERSION;
    uint32_t packet_size = HEADER_SIZE_TO_SERVER+add_len;
    uint64_t timestamp = CurrentTimestamp();

    client_data *c_data = nullptr;
    uint64_t c_id = 0;
    uint32_t c_protocol = 0;
    uint32_t c_ip = 0;
    uint16_t port_client = 0;
    uint16_t port_server = 0;

    if(client != nullptr)
    {
        c_data = reinterpret_cast<client_data*>(client->data);
        c_id = c_data->id;
        c_protocol = c_data->protocol;
        c_ip = c_data->ip;
        port_client = c_data->port_client;
        port_server = c_data->port_server;
    }
    auto str = reinterpret_cast<uint8_t*>(malloc(packet_size));

    DataToBytes(version, str);
    DataToBytes(packet_size, &str[4]);
    DataToBytes(timestamp, &str[8]);
    DataToBytes(c_id, &str[16]);
    DataToBytes(c_protocol, &str[24]);
    DataToBytes(__builtin_bswap32(c_ip), &str[28]);
    DataToBytes(port_server, &str[32]);
    DataToBytes(port_client, &str[34]);
    DataToBytes(p_type, &str[36]);

    if(add_len > 0)
    {
        memcpy(str + HEADER_SIZE_TO_SERVER, add_data, add_len);
    }
    return uv_buf_init(reinterpret_cast<char*>(str), packet_size);
}

int observe(uv_stream_t* server, const uint32_t protocol_id)
{
    if(protocol_id <= 6) /* если сервер уже подписан на любой из логов, то происходит отписка и подписка на текущий */
    {
        for(int i = 0; i < 7; ++i)
        {
            auto it = log_map[i].find(server);
            if(it != log_map[i].end())
            {
                if(i == static_cast<int>(protocol_id))
                {
                    return -3; //ошибка, сервер уже подписан на текущий лог
                }
                log_map[i].erase(it);
                break;
            }
        }
        log_map[protocol_id].emplace(server);
        return 0;
    }

    auto protocol_check = protocol_map.find(protocol_id);
    if(protocol_check == protocol_map.end())
    {
        return -1; //ошибка, неизвестный протокол
    }
    else if(protocol_check->second == server)
    {
        return -2;  //ошибка, сервер подписан на протокол как обработчик
    }

    auto isAbsoluteObserving = std::find(absolute_observe_list.begin(), absolute_observe_list.end(), server);
    if(isAbsoluteObserving != absolute_observe_list.end())
    {
        absolute_observe_list.erase(isAbsoluteObserving);
    }

    auto observe_node = observe_map.find(protocol_id);
    auto it = observe_node->second.find(server);
    if(it == observe_node->second.end())
    {
        observe_node->second.emplace(server);
        return 0; //успех, наблюдатель добавлен
    }
    else
    {
        return -3; //ошибка, сервер уже наблюдает
    }
}

int subscribe(uv_stream_t* server, const uint32_t protocol_id, bool force)
{
    if(protocol_id <= 6)
    {
        return observe(server, protocol_id);
    }

    /* проверка на то, является ли сервер наблюдателем этого протокола */
    auto observe_node = observe_map.find(protocol_id);
    if(observe_node == observe_map.end())
    {
        return -1; //ошибка, неизвестный протокол
    }
    auto observer = observe_node->second.find(server);
    if(observer != observe_node->second.end())
    {
        return -2; //ошибка, сервер подписан на протокол в режиме observe
    }

    auto it = protocol_map.find(protocol_id);
    if(it->second == server)
    {
        return -3; //ошибка, сервер уже подписан на протокол
    }
    else if(it->second != nullptr)
    {
        if(force == true)
        {
            it->second = server;
            return 0; //успех, существующий обработчик сброшен
        }
        else
        {
            return -4; //ошибка, уже есть обработчик на протокол
        }
    }
   /* else if(it->second == nullptr) */
    it->second = server;
    return 0; //успех, сервер подписан
}

int unsubscribe(uv_stream_t* server, const uint32_t protocol_id)
{
    if(protocol_id <= 6)
    {
        auto it = log_map[protocol_id].find(server);
        if(it == log_map[protocol_id].end())
        {
            return -2; //ошибка, сервер не подписан на протокол
        }
        log_map[protocol_id].erase(it);
        return 0; //успех, сервер отписан от лога
    }
    /* попытка отписки от протокола*/
    auto it = protocol_map.find(protocol_id);
    if(it == protocol_map.end())
    {
        return -1; //ошибка, неизвестный протокол
    }
    else if(it->second == server)
    {
        it->second = nullptr;
        return 0; //успех, сервер отписан
    }
    /*попытка отписки от наблюдения */
    auto observe_node = observe_map.find(protocol_id);
    auto iter = observe_node->second.find(server);
    if(iter == observe_node->second.end())
    {
        return -2; //ошибка, сервер не подписан
    }
    else
    {
        observe_node->second.erase(iter);
        return 0; //успех сервер отписан
    }
}

void ParsePacketFromServer(uv_stream_t* server, std::vector<uint8_t> &buf)
{

    uint32_t version = 0;
    memcpy(&version,&buf[0], sizeof(version));

    if(version != CURRENT_PROTOCOL_VERSION)
    {
        uv_close(reinterpret_cast<uv_handle_t*>(server), close_server_cb);
        MsgToServer::SendMessage(LEVEL_ERROR, "Unknown protocol version");
        printf("Unknown protocol version: %u, current version is %u\n", version, CURRENT_PROTOCOL_VERSION);
        return;
    }

    uint32_t packet_size = 0;
    memcpy(&packet_size,&buf[4],  sizeof(packet_size));

    uint64_t counter = 0;
    memcpy(&counter,&buf[8],  sizeof(counter));

    uint16_t packet_type = 0;
    memcpy(&packet_type, &buf[16], sizeof(packet_type));

    size_t add_len = packet_size - HEADER_SIZE_FROM_SERVER;

    if(packet_type == REGISTER
       || packet_type == REGISTER_FORCE
       || packet_type == OBSERVE
       || packet_type == UNREGISTER)
    {
        if(add_len < 4)  /*т.к protocol_id должен занимать 4 байта, если add_len < 4 будем считать, что в доп. данных ничего нет*/
        {
            if(packet_type == OBSERVE)
            {
                for(auto &observe_node : observe_map)
                {
                    auto server_iter = observe_node.second.find(server);
                    if(server_iter != observe_node.second.end())
                    {
                        observe_node.second.erase(server_iter);
                    }
                }
                //subscribe for all data
                absolute_observe_list.push_back(server);
                MsgToServer::SendErrorPersonality(server, 0);
                return;
            }

            std::stringstream ss;
            ss << "Invalid packet from server, counter: " << counter;
            MsgToServer::SendMessage(LEVEL_WARN, ss.str().c_str());
            printf("%s\n", ss.str().c_str());
            return;
        }
        uint32_t protocol_id = 0;
        memcpy(&protocol_id, &buf[HEADER_SIZE_FROM_SERVER], sizeof(protocol_id));

        if(packet_type == REGISTER || packet_type == REGISTER_FORCE)
        {
            int r = subscribe(server, protocol_id, packet_type == REGISTER_FORCE);
            MsgToServer::SendErrorPersonality(server, r);

            std::stringstream ss;
            MESSAGE_LEVEL msg_level = LEVEL_ERROR;
            switch(r)
            {
                case 0:
                    ss << "Handler added";
                    msg_level = LEVEL_INFO;
                    break;
                case -1:
                    ss << "Attempt to register unknown protocol: " << protocol_id;
                    break;
                case -2:
                    ss << "Server is already observer for this protocol: " << protocol_id;
                    break;
                case -3:
                    ss << "Server is already handler for this protocol: " << protocol_id;
                    break;
                case -4:
                    ss << "There is already a handler for this protocol: " << protocol_id;
                    break;
                default:
                    ss << "Unknown result";
                    msg_level = LEVEL_CRIT;
                    break;
            }
            MsgToServer::SendMessage(msg_level, ss.str().c_str());
            printf("%s\n", ss.str().c_str());
        }
        else if(packet_type == OBSERVE)
        {
            int r = observe(server, protocol_id);
            MsgToServer::SendErrorPersonality(server, r);

            std::stringstream ss;
            MESSAGE_LEVEL msg_level = LEVEL_ERROR;
            switch(r)
            {
                case 0:
                    ss << "Observer added, protocol: " << protocol_id;
                    msg_level = LEVEL_INFO;
                    break;
                case -1:
                    ss << "Attempt to observe unknown protocol: " << protocol_id;
                    break;
                case -2:
                    ss << "Server is already a handler for this protocol: " << protocol_id;
                    break;
                case -3:
                    ss << "Server is already observing, protocol: " << protocol_id;
                    break;
                default:
                    ss << "Unknown result";
                    msg_level = LEVEL_CRIT;
                    break;
            }
            MsgToServer::SendMessage(msg_level, ss.str().c_str());
            //printf("server observed, protocol: %d\n", protocol_id);
        }
        else   /*if type == UNREGISTER*/
        {
            int r = unsubscribe(server, protocol_id);
            MsgToServer::SendErrorPersonality(server, r);

            std::stringstream ss;
            MESSAGE_LEVEL msg_level = LEVEL_ERROR;
            switch(r)
            {
                case 0:
                    ss << "Server unregistered";
                    msg_level = LEVEL_INFO;
                    break;
                case -1:
                    ss << "Attempt to unregister from unknown protocol: " << protocol_id;
                    break;
                case -2:
                    ss << "Server is not registered to this protocol: " << protocol_id;
                    break;
                default:
                    ss << "Unknown result";
                    msg_level = LEVEL_CRIT;
                    break;
            }
            MsgToServer::SendMessage(msg_level, ss.str().c_str());
        }
    }
    else if(packet_type == DATA)
    {
        int data_size = static_cast<int>(packet_size) - HEADER_SIZE_FROM_SERVER - 8;
        if(data_size <= 0)
        {
            printf("invalid data\n");
            return;
        }

        uint64_t c_id = 0;
        memcpy(&c_id, &buf[HEADER_SIZE_FROM_SERVER], sizeof(c_id));

        auto data_to_client = reinterpret_cast<uint8_t*>(malloc(data_size));
        memcpy(data_to_client, &buf[0]+HEADER_SIZE_FROM_SERVER+8, data_size);

        uv_buf_t buf_client = uv_buf_init(reinterpret_cast<char*>(data_to_client), data_size);

        auto it = client_map.find(c_id);
        if(it == client_map.end())
        {
            MsgToServer::SendMessage(LEVEL_ERROR, "Unknown client id");
            printf("unknown client id\n");
        }
        else
        {
            auto request = reinterpret_cast<uv_write_t*>(malloc(sizeof(uv_write_t)));
            uv_write(request, it->second, &buf_client, 1,  [](uv_write_t* req, int /*status*/){
                free(req);
            });
        }
        //send to absolute_observers
        for(auto &iter : absolute_observe_list)
        {
            auto request = reinterpret_cast<uv_write_t*>(malloc(sizeof (uv_write_t)));
            uv_write(request, iter, &buf_client, 1,  [](uv_write_t* req, int /*status*/){
                free(req);
            });
        }
        free(buf_client.base);
    }
    else if(packet_type == ACTION)
    {
        uint64_t c_id = 0;
        memcpy(&c_id, &buf[HEADER_SIZE_FROM_SERVER], sizeof(c_id));

        uint8_t cmd = 0;
        memcpy(&cmd, &buf[HEADER_SIZE_FROM_SERVER+8], sizeof(cmd));

        auto client = client_map.find(c_id);
        if(client == client_map.end())
        {
            printf("Unknown client: %llu\n", c_id);
            MsgToServer::SendMessage(LEVEL_ERROR, "Unknown client id");
            return;
        }

        std::stringstream ss;
        MESSAGE_LEVEL msg_level = LEVEL_INFO;
        switch(cmd)
        {
            case 1:
                uv_close(reinterpret_cast<uv_handle_t*>(client->second), close_client_cb);
                ss << "Client connection closed, id: " << c_id;
                break;
            default:
                ss << "Unknown action: " << cmd;
                msg_level = LEVEL_ERROR;
                break;
        }
        printf("%s\n", ss.str().c_str());
        MsgToServer::SendMessage(msg_level, ss.str().c_str());
    }
    else
    {
        MsgToServer::SendMessage(LEVEL_ERROR, "Unknown packet type");
    }


}

void MsgToServer::SendConnect(uv_stream_t* client_from, bool connect)
{
    PACKET_TYPE_FOR p_type = connect ? TYPE_CONNECT : TYPE_DISCONNECT;
    uv_buf_t buf = InitPacketToServer(client_from, p_type);
    uint32_t client_protocol = reinterpret_cast<client_data*>(client_from->data)->protocol;

    auto handler = protocol_map.find(client_protocol);
    if(handler == protocol_map.end())
    {
        return;
    }
    else if(handler->second != nullptr)
    {
        auto request = reinterpret_cast<uv_write_t*>(malloc(sizeof (uv_write_t)));
        uv_write(request, handler->second, &buf, 1, [](uv_write_t* req, int /*status*/){
            free(req);
        });
    }
    /* send to observers */
    auto observe_node = observe_map.find(client_protocol);
    for(auto &it : observe_node->second)
    {
        auto request = reinterpret_cast<uv_write_t*>(malloc(sizeof (uv_write_t)));
        uv_write(request, it, &buf, 1,  [](uv_write_t* req, int /*status*/){
            free(req);
        });
    }
    //send to absolute observers
    for(auto &iter : absolute_observe_list)
    {
        auto request = reinterpret_cast<uv_write_t*>(malloc(sizeof (uv_write_t)));
        uv_write(request, iter, &buf, 1,  [](uv_write_t* req, int /*status*/){
            free(req);
        });
    }

    free(buf.base);
}

void MsgToServer::SendMessage(uint8_t msg_level, const char* str, uint64_t *counter)
{
    size_t str_len = strlen(str);
    size_t res_str_len = str_len + 2;  /* +msg_level+0 */

    uint8_t* res_str = reinterpret_cast<uint8_t*>(malloc(res_str_len));
    memcpy(res_str, &msg_level, 1);
    memcpy(res_str + 1, str, str_len);
    res_str[res_str_len-1] = 0;

    uv_buf_t buf = InitPacketToServer(nullptr, TYPE_MESSAGE, res_str, res_str_len);

    if(msg_level == LEVEL_ERROR && counter != nullptr)
    {
        DataToBytes(*counter, &reinterpret_cast<uint8_t*>(buf.base)[16]);
    }
    for(int i = msg_level; i < 7; ++i)
    {
        for(auto &it : log_map[i])
        {
            auto request = reinterpret_cast<uv_write_t*>(malloc(sizeof(uv_write_t)));
            uv_write(request, it, &buf, 1,  [](uv_write_t* req, int /*status*/){
                free(req);
            });
        }
    }
    free(res_str);
    free(buf.base);
}

void MsgToServer::SendData(uv_stream_t* client_from, const uint8_t* data, size_t data_len)
{
    uint32_t client_protocol = reinterpret_cast<client_data*>(client_from->data)->protocol;
    uv_buf_t buf = InitPacketToServer(client_from, TYPE_DATA, data, data_len);

    auto handler = protocol_map.find(client_protocol);
    if(handler == protocol_map.end())
    {
        return;
    }
    else if(handler->second != nullptr)
    {
        auto request = reinterpret_cast<uv_write_t*>(malloc(sizeof (uv_write_t)));
        uv_write(request, handler->second, &buf, 1, [](uv_write_t* req, int /*status*/){
            free(req);
        });
    }
    //send to observers
    auto observe_node = observe_map.find(client_protocol);
    for(auto &it : observe_node->second)
    {
        auto request = reinterpret_cast<uv_write_t*>(malloc(sizeof (uv_write_t)));
        uv_write(request, it, &buf, 1,  [](uv_write_t* req, int /*status*/){
            free(req);
        });
    }
    //send to absolute_observers
    for(auto &iter : absolute_observe_list)
    {
        auto request = reinterpret_cast<uv_write_t*>(malloc(sizeof (uv_write_t)));
        uv_write(request, iter, &buf, 1,  [](uv_write_t* req, int /*status*/){
            free(req);
        });
    }
    free(buf.base);
}

void MsgToServer::SendError(uv_stream_t* client_from, uint32_t error_code)
{
    auto error_str = reinterpret_cast<uint8_t*>(malloc(4));
    DataToBytes(error_code, error_str);
    uv_buf_t buf = InitPacketToServer(client_from, TYPE_ERROR, error_str, 4);
    uint32_t client_protocol = reinterpret_cast<client_data*>(client_from->data)->protocol;

    auto handler = protocol_map.find(client_protocol);
    if(handler == protocol_map.end())
    {
        return;
    }
    else if(handler->second != nullptr)
    {
        auto request = reinterpret_cast<uv_write_t*>(malloc(sizeof (uv_write_t)));
        uv_write(request, handler->second, &buf, 1, [](uv_write_t* req, int /*status*/){
            free(req);
        });
    }
    /* send to observers */
    auto observe_node = observe_map.find(client_protocol);
    for(auto &it : observe_node->second)
    {
        auto request = reinterpret_cast<uv_write_t*>(malloc(sizeof (uv_write_t)));
        uv_write(request, it, &buf, 1,  [](uv_write_t* req, int /*status*/){
            free(req);
        });
    }
    //send to absolute_observers
    for(auto &iter : absolute_observe_list)
    {
        auto request = reinterpret_cast<uv_write_t*>(malloc(sizeof (uv_write_t)));
        uv_write(request, iter, &buf, 1,  [](uv_write_t* req, int /*status*/){
            free(req);
        });
    }
    free(buf.base);
    free(error_str);
}

void MsgToServer::SendErrorPersonality(uv_stream_t* to_server, uint32_t error_code)
{
    auto error_str = reinterpret_cast<uint8_t*>(malloc(4));
    DataToBytes(error_code, error_str);
    uv_buf_t buf = InitPacketToServer(nullptr, TYPE_ERROR, error_str, 4);

    auto request = reinterpret_cast<uv_write_t*>(malloc(sizeof(uv_write_t)));
    uv_write(request, to_server, &buf, 1, [](uv_write_t* req, int /*status*/){
        free(req);
    });
    free(error_str);
    free(buf.base);
}