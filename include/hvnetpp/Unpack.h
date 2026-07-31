#pragma once

// 拆包设置，字段语义与 libhv 的 unpack_setting_t 保持一致

namespace hvnetpp {

class Buffer;

enum unpack_mode_e {
    UNPACK_MODE_NONE        = 0,
    UNPACK_BY_FIXED_LENGTH  = 1,
    UNPACK_BY_DELIMITER     = 2,
    UNPACK_BY_LENGTH_FIELD  = 3,
};

enum unpack_coding_e {
    ENCODE_BY_BIG_ENDIAN    = 0,
    ENCODE_BY_LITTLE_ENDIAN = 1,
    // ENCODE_BY_VARINT 预留，暂不支持
};

#define HVNETPP_DEFAULT_PACKAGE_MAX_LENGTH (1 << 21) // 2MB
#define HVNETPP_DELIMITER_BYTES_MAX 8

struct unpack_setting_t {
    unpack_mode_e mode = UNPACK_MODE_NONE;
    unsigned int package_max_length = HVNETPP_DEFAULT_PACKAGE_MAX_LENGTH;

    // UNPACK_BY_FIXED_LENGTH
    unsigned int fixed_length = 0;

    // UNPACK_BY_DELIMITER: 包含分隔符本身
    unsigned char delimiter[HVNETPP_DELIMITER_BYTES_MAX] = {0};
    unsigned short delimiter_bytes = 0;

    // UNPACK_BY_LENGTH_FIELD:
    // packet_length = body_offset + length_field_value + length_adjustment
    // 回调收到的是完整包（含包头）
    unsigned short body_offset = 0;         // 即包头长度
    unsigned short length_field_offset = 0;
    unsigned short length_field_bytes = 0;  // 1/2/4/8
    short length_adjustment = 0;
    unpack_coding_e length_field_coding = ENCODE_BY_BIG_ENDIAN;
};

// 尝试从 in 中取出一个完整包放入 out（out 会先被清空）。
// 返回 1: 取出一个包; 0: 数据不足; -1: 协议错误（*protoError 置 true，调用方应关闭连接）
int unpackBuffer(const unpack_setting_t& setting, Buffer* in, Buffer* out, bool* protoError);

} // namespace hvnetpp
