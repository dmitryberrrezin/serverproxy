#pragma once
#include "definitions.h"

#include <cstdint>
#include <uv.h>
#include <map>
#include <set>
#include <vector>
#include <cstring>

template <typename T>
void DataToBytes(const T &data, uint8_t* dest)
{
    memcpy(dest, &data, sizeof(T));
}
uint64_t CurrentTimestamp();
uint32_t GetInt32IP(uv_stream_t* client, uint16_t* port = nullptr);
uv_buf_t InitPacketToServer(uv_stream_t* client,
                            PACKET_TYPE_FOR p_type,
                            const uint8_t *add_data = nullptr,
                            size_t add_len = 0);
void ParsePacketFromServer(uv_stream_t* server, std::vector<uint8_t> &buf);
int subscribe(uv_stream_t* server, uint32_t protocol_id, bool force = false);
int observe(uv_stream_t* server, uint32_t protocol_id);
int unsubscribe(uv_stream_t* server, uint32_t protocol_id);

namespace MsgToServer
{
    void SendConnect(uv_stream_t* client_from, bool connect); /* msg connect/disconnect */
    void SendMessage(uint8_t msg_level, const char* str, uint64_t *counter = nullptr); /* for text msg, counter - for ERROR msg only */
    void SendData(uv_stream_t* client_from, const uint8_t* data, size_t data_len);
    void SendError(uv_stream_t* client_from, uint32_t error_code);
    void SendErrorPersonality(uv_stream_t* to_server, uint32_t error_code);
}


