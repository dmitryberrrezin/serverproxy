#pragma once

#include <map>
#include <set>
#include <list>
#include <uv.h>

const int DEFAULT_BACKLOG = 256;
const char* const IP = "127.0.0.1";
const uint32_t CURRENT_PROTOCOL_VERSION = 0x00000001;
const int HEADER_SIZE_TO_SERVER = 38;
const int HEADER_SIZE_FROM_SERVER = 18;

extern std::map<uint64_t, uv_stream_t*> client_map; /* key - id, value - client */
extern std::map<uint32_t, uv_stream_t*> protocol_map;/* key - protocol, value - server */
extern std::map<uint32_t, std::set<uv_stream_t*>> observe_map; /* key - protocol id, value - set of followers */
extern std::list<uv_stream_t*> absolute_observe_list; /* list of observers of all data */
extern std::map<uint8_t, std::set<uv_stream_t*>> log_map; /* key - log id, value - set of followers */
extern std::map<uv_stream_t*, std::vector<uint8_t>> packet_storage; /* key - server, value - storage data for parsing */

enum PACKET_TYPE_FROM : uint16_t
{
    REGISTER,
    REGISTER_FORCE,
    UNREGISTER,
    OBSERVE,
    DATA,
    ACTION
};

enum PACKET_TYPE_FOR : uint16_t
{
    TYPE_CONNECT,
    TYPE_DISCONNECT,
    TYPE_ERROR,
    TYPE_DATA,
    TYPE_MESSAGE
};

enum MESSAGE_LEVEL : uint8_t
{
    LEVEL_FATAL,
    LEVEL_CRIT,
    LEVEL_ERROR,
    LEVEL_WARN,
    LEVEL_INFO,
    LEVEL_DEBUG,
    LEVEL_TRACE
};

struct client_data
{
    uint64_t id;
    uint32_t protocol;
    uint32_t ip;
    uint16_t port_client;
    uint16_t port_server;
};

struct listener_data
{
    uint16_t listening_port;
};

