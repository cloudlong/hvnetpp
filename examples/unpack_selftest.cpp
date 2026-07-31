// unpack 解析器回归测试：三种模式 + 分片/粘包/越界用例
#include "hvnetpp/Unpack.h"
#include "hvnetpp/Buffer.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

using namespace hvnetpp;

static int g_failures = 0;

#define CHECK(expr) \
    do { \
        if (!(expr)) { \
            ++g_failures; \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        } \
    } while (0)

static void testFixedLength() {
    unpack_setting_t s;
    s.mode = UNPACK_BY_FIXED_LENGTH;
    s.fixed_length = 4;

    Buffer in, out;
    bool err = false;

    // 不足一个包
    in.append("ab", 2);
    CHECK(unpackBuffer(s, &in, &out, &err) == 0);
    // 补齐后粘着第二个包的一半
    in.append("cdEF", 4);
    CHECK(unpackBuffer(s, &in, &out, &err) == 1);
    CHECK(out.retrieveAllAsString() == "abcd");
    CHECK(unpackBuffer(s, &in, &out, &err) == 0);
    in.append("GH", 2);
    CHECK(unpackBuffer(s, &in, &out, &err) == 1);
    CHECK(out.retrieveAllAsString() == "EFGH");
    CHECK(in.readableBytes() == 0);
    CHECK(!err);
    printf("testFixedLength OK\n");
}

static void testDelimiter() {
    unpack_setting_t s;
    s.mode = UNPACK_BY_DELIMITER;
    s.delimiter[0] = '\r';
    s.delimiter[1] = '\n';
    s.delimiter_bytes = 2;
    s.package_max_length = 16;

    Buffer in, out;
    bool err = false;

    in.append("hello", 5);
    CHECK(unpackBuffer(s, &in, &out, &err) == 0);
    in.append("\r\nwor", 5);
    CHECK(unpackBuffer(s, &in, &out, &err) == 1);
    CHECK(out.retrieveAllAsString() == "hello\r\n"); // 包含分隔符
    CHECK(unpackBuffer(s, &in, &out, &err) == 0);
    // 分隔符本身被分片
    in.append("ld\r", 3);
    CHECK(unpackBuffer(s, &in, &out, &err) == 0);
    in.append("\n", 1);
    CHECK(unpackBuffer(s, &in, &out, &err) == 1);
    CHECK(out.retrieveAllAsString() == "world\r\n");

    // 超长无分隔符 -> 协议错误
    Buffer in2;
    std::string big(32, 'x');
    in2.append(big);
    CHECK(unpackBuffer(s, &in2, &out, &err) == -1);
    CHECK(err);
    printf("testDelimiter OK\n");
}

static void testLengthField() {
    // 4 字节大端长度头，length 为 body 长度
    unpack_setting_t s;
    s.mode = UNPACK_BY_LENGTH_FIELD;
    s.body_offset = 4;
    s.length_field_offset = 0;
    s.length_field_bytes = 4;
    s.length_field_coding = ENCODE_BY_BIG_ENDIAN;
    s.package_max_length = 64;

    Buffer in, out;
    bool err = false;

    // 分片送包头
    in.appendInt16(0);
    CHECK(unpackBuffer(s, &in, &out, &err) == 0);
    in.appendInt16(5); // 完整头: body len = 5
    CHECK(unpackBuffer(s, &in, &out, &err) == 0);
    in.append("hel", 3);
    CHECK(unpackBuffer(s, &in, &out, &err) == 0);
    in.append("lo", 2);
    CHECK(unpackBuffer(s, &in, &out, &err) == 1);
    CHECK(out.readableBytes() == 9); // 头4 + 体5
    out.retrieve(4);
    CHECK(out.retrieveAllAsString() == "hello");

    // 一次读到两个完整包
    in.appendInt32(1); in.append("A", 1);
    in.appendInt32(2); in.append("BC", 2);
    CHECK(unpackBuffer(s, &in, &out, &err) == 1);
    CHECK(out.readableBytes() == 5);
    CHECK(unpackBuffer(s, &in, &out, &err) == 1);
    CHECK(out.readableBytes() == 6);
    CHECK(in.readableBytes() == 0);

    // 小端
    unpack_setting_t sle = s;
    sle.length_field_coding = ENCODE_BY_LITTLE_ENDIAN;
    Buffer inle;
    const unsigned char hdr[4] = {3, 0, 0, 0}; // LE: 3
    inle.append(hdr, 4);
    inle.append("xyz", 3);
    CHECK(unpackBuffer(sle, &inle, &out, &err) == 1);
    CHECK(out.readableBytes() == 7);

    // 越界长度 -> 协议错误
    Buffer inbad;
    inbad.appendInt32(1000); // > package_max_length 64
    CHECK(unpackBuffer(s, &inbad, &out, &err) == -1);
    CHECK(err);

    // length_adjustment: length 字段含包头总长
    unpack_setting_t st = s;
    st.length_adjustment = -4; // packet = 4 + len - 4 = len
    Buffer int2;
    int2.appendInt32(9); // 总长 9 = 头4 + 体5
    int2.append("hello", 5);
    CHECK(unpackBuffer(st, &int2, &out, &err) == 1);
    CHECK(out.readableBytes() == 9);
    printf("testLengthField OK\n");
}

int main() {
    testFixedLength();
    testDelimiter();
    testLengthField();
    if (g_failures == 0) {
        printf("unpack_selftest: ALL PASSED\n");
        return 0;
    }
    fprintf(stderr, "unpack_selftest: %d FAILURES\n", g_failures);
    return 1;
}
