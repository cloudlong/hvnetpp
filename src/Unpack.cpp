#include "hvnetpp/Unpack.h"
#include "hvnetpp/Buffer.h"
#include "rtclog.h"

#include <algorithm>
#include <cstdint>

namespace hvnetpp {

namespace {

// 从 p 处按 bytes/coding 解出长度字段值
uint64_t decodeLengthField(const unsigned char* p, unsigned short bytes, unpack_coding_e coding) {
    uint64_t value = 0;
    if (coding == ENCODE_BY_BIG_ENDIAN) {
        for (unsigned short i = 0; i < bytes; ++i) {
            value = (value << 8) | p[i];
        }
    } else {
        for (unsigned short i = bytes; i > 0; --i) {
            value = (value << 8) | p[i - 1];
        }
    }
    return value;
}

void emitPacket(Buffer* in, Buffer* out, size_t len) {
    out->retrieveAll();
    out->append(in->peek(), len);
    in->retrieve(len);
}

} // namespace

int unpackBuffer(const unpack_setting_t& setting, Buffer* in, Buffer* out, bool* protoError) {
    if (protoError) {
        *protoError = false;
    }
    const size_t readable = in->readableBytes();

    switch (setting.mode) {
    case UNPACK_MODE_NONE:
        if (readable == 0) {
            return 0;
        }
        emitPacket(in, out, readable);
        return 1;

    case UNPACK_BY_FIXED_LENGTH:
        if (setting.fixed_length == 0) {
            RTCLOG(RTC_ERROR, "unpack: fixed_length == 0");
            if (protoError) *protoError = true;
            return -1;
        }
        if (readable < setting.fixed_length) {
            return 0;
        }
        emitPacket(in, out, setting.fixed_length);
        return 1;

    case UNPACK_BY_DELIMITER: {
        if (setting.delimiter_bytes == 0 || setting.delimiter_bytes > HVNETPP_DELIMITER_BYTES_MAX) {
            RTCLOG(RTC_ERROR, "unpack: invalid delimiter_bytes %d", setting.delimiter_bytes);
            if (protoError) *protoError = true;
            return -1;
        }
        const char* begin = in->peek();
        const char* end = begin + readable;
        const char* delim = reinterpret_cast<const char*>(setting.delimiter);
        const char* pos = std::search(begin, end, delim, delim + setting.delimiter_bytes);
        if (pos == end) {
            if (readable > setting.package_max_length) {
                RTCLOG(RTC_ERROR, "unpack: no delimiter within package_max_length %u", setting.package_max_length);
                if (protoError) *protoError = true;
                return -1;
            }
            return 0;
        }
        size_t packetLen = static_cast<size_t>(pos - begin) + setting.delimiter_bytes;
        if (packetLen > setting.package_max_length) {
            RTCLOG(RTC_ERROR, "unpack: packet length %zu exceeds package_max_length %u", packetLen, setting.package_max_length);
            if (protoError) *protoError = true;
            return -1;
        }
        emitPacket(in, out, packetLen);
        return 1;
    }

    case UNPACK_BY_LENGTH_FIELD: {
        if (setting.length_field_bytes != 1 && setting.length_field_bytes != 2 &&
            setting.length_field_bytes != 4 && setting.length_field_bytes != 8) {
            RTCLOG(RTC_ERROR, "unpack: invalid length_field_bytes %d", setting.length_field_bytes);
            if (protoError) *protoError = true;
            return -1;
        }
        const size_t headerLen = static_cast<size_t>(setting.body_offset);
        if (headerLen < static_cast<size_t>(setting.length_field_offset) + setting.length_field_bytes) {
            RTCLOG(RTC_ERROR, "unpack: body_offset %d smaller than length field end", setting.body_offset);
            if (protoError) *protoError = true;
            return -1;
        }
        if (readable < headerLen) {
            return 0;
        }
        const unsigned char* p = reinterpret_cast<const unsigned char*>(in->peek()) + setting.length_field_offset;
        uint64_t bodyLen = decodeLengthField(p, setting.length_field_bytes, setting.length_field_coding);
        int64_t packetLen = static_cast<int64_t>(headerLen) + static_cast<int64_t>(bodyLen) + setting.length_adjustment;
        if (packetLen < static_cast<int64_t>(headerLen) ||
            packetLen > static_cast<int64_t>(setting.package_max_length)) {
            RTCLOG(RTC_ERROR, "unpack: bad packet length %lld (max %u)",
                   static_cast<long long>(packetLen), setting.package_max_length);
            if (protoError) *protoError = true;
            return -1;
        }
        if (readable < static_cast<size_t>(packetLen)) {
            return 0;
        }
        emitPacket(in, out, static_cast<size_t>(packetLen));
        return 1;
    }

    default:
        RTCLOG(RTC_ERROR, "unpack: unknown mode %d", setting.mode);
        if (protoError) *protoError = true;
        return -1;
    }
}

} // namespace hvnetpp
