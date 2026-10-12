#ifndef FITSHOWPACKET_H
#define FITSHOWPACKET_H

#include <QByteArray>
#include <cstdint>

// Framing extracted from fitshowtreadmill::writePayload/checkIncomingPacket.
// This helper performs no I/O and does not imply a device accepts a command.
namespace FitShowPacket {
inline QByteArray encode(const uint8_t *payload, int size) {
    if (!payload || size < 1 || size > 22) return {};
    QByteArray frame;
    frame.reserve(size + 3);
    frame.append(char(0x02));
    uint8_t checksum = 0;
    for (int i = 0; i < size; ++i) {
        frame.append(char(payload[i]));
        checksum ^= payload[i];
    }
    frame.append(char(checksum));
    frame.append(char(0x03));
    return frame;
}
inline bool validResponse(const uint8_t *data, int size) {
    // Every parsed response needs command + parameter, in addition to framing.
    if (!data || size < 5 || size > 255 || data[0] != 0x02 || data[size - 1] != 0x03) return false;
    uint8_t checksum = 0;
    for (int i = 1; i < size - 2; ++i) checksum ^= data[i];
    return checksum == data[size - 2];
}
}
#endif
