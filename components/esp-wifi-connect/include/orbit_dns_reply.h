#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// Bounded captive-portal DNS: one uncompressed question, no reflected additional
// records. Unsupported types get an empty answer so clients can fall back to A.
inline size_t OrbitDnsReply(uint8_t* packet, size_t length, size_t capacity,
                            const void* ipv4_network_order) {
    if (length < 12 || length > 512 || length > capacity || (packet[2] & 0xf8) != 0 ||
        packet[4] != 0 || packet[5] != 1 || packet[6] != 0 || packet[7] != 0 || packet[8] != 0 ||
        packet[9] != 0)
        return 0;
    size_t cursor = 12;
    while (true) {
        if (cursor >= length)
            return 0;
        const size_t label = packet[cursor++];
        if (label == 0)
            break;
        if (label > 63 || label > length - cursor)
            return 0;
        cursor += label;
        if (cursor - 12 > 254)
            return 0;
    }
    if (length - cursor < 4)
        return 0;
    const uint16_t type = (packet[cursor] << 8) | packet[cursor + 1];
    const uint16_t klass = (packet[cursor + 2] << 8) | packet[cursor + 3];
    cursor += 4;
    const bool answer = klass == 1 && (type == 1 || type == 255);
    if (answer && capacity - cursor < 16)
        return 0;
    packet[2] = 0x80 | (packet[2] & 1);  // Reply; retain recursion-desired.
    packet[3] = 0x80;
    packet[6] = 0;
    packet[7] = answer ? 1 : 0;
    packet[8] = packet[9] = packet[10] = packet[11] = 0;
    if (answer) {
        const uint8_t record[] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 28, 0, 4};
        memcpy(packet + cursor, record, sizeof(record));
        memcpy(packet + cursor + sizeof(record), ipv4_network_order, 4);
        cursor += 16;
    }
    return cursor;
}
