#pragma once

#include <vector>
#include <string>
#include <algorithm>
#include <assert.h>
#include <cstring>
#include <cstdint>
#include <endian.h>

namespace hvnetpp {

class Buffer {
public:
    static const size_t kCheapPrepend = 8;
    static const size_t kInitialSize = 1024;

    explicit Buffer(size_t initialSize = kInitialSize)
        : buffer_(kCheapPrepend + initialSize),
          readerIndex_(kCheapPrepend),
          writerIndex_(kCheapPrepend) {
    }

    void swap(Buffer& rhs) {
        buffer_.swap(rhs.buffer_);
        std::swap(readerIndex_, rhs.readerIndex_);
        std::swap(writerIndex_, rhs.writerIndex_);
    }

    size_t readableBytes() const { return writerIndex_ - readerIndex_; }
    size_t writableBytes() const { return buffer_.size() - writerIndex_; }
    size_t prependableBytes() const { return readerIndex_; }

    const char* peek() const { return begin() + readerIndex_; }

    const char* findCRLF() const {
        const char* crlf = std::search(peek(), beginWrite(), kCRLF, kCRLF + 2);
        return crlf == beginWrite() ? nullptr : crlf;
    }

    const char* findCRLF(const char* start) const {
        assert(peek() <= start);
        assert(start <= beginWrite());
        const char* crlf = std::search(start, beginWrite(), kCRLF, kCRLF + 2);
        return crlf == beginWrite() ? nullptr : crlf;
    }

    const char* findEOL() const {
        const void* eol = memchr(peek(), '\n', readableBytes());
        return static_cast<const char*>(eol);
    }

    const char* findEOL(const char* start) const {
        assert(peek() <= start);
        assert(start <= beginWrite());
        const void* eol = memchr(start, '\n', beginWrite() - start);
        return static_cast<const char*>(eol);
    }

    void retrieve(size_t len) {
        assert(len <= readableBytes());
        if (len < readableBytes()) {
            readerIndex_ += len;
        } else {
            retrieveAll();
        }
    }

    void retrieveAll() {
        readerIndex_ = kCheapPrepend;
        writerIndex_ = kCheapPrepend;
    }

    std::string retrieveAllAsString() {
        return retrieveAsString(readableBytes());
    }

    std::string retrieveAsString(size_t len) {
        assert(len <= readableBytes());
        std::string result(peek(), len);
        retrieve(len);
        return result;
    }

    void append(const std::string& str) {
        append(str.data(), str.size());
    }

    void append(const char* data, size_t len) {
        ensureWritableBytes(len);
        std::copy(data, data + len, beginWrite());
        hasWritten(len);
    }

    void append(const void* data, size_t len) {
        append(static_cast<const char*>(data), len);
    }

    void ensureWritableBytes(size_t len) {
        if (writableBytes() < len) {
            makeSpace(len);
        }
        assert(writableBytes() >= len);
    }

    char* beginWrite() { return begin() + writerIndex_; }
    const char* beginWrite() const { return begin() + writerIndex_; }

    void hasWritten(size_t len) {
        assert(len <= writableBytes());
        writerIndex_ += len;
    }

    void prepend(const void* data, size_t len) {
        assert(len <= prependableBytes());
        readerIndex_ -= len;
        const char* d = static_cast<const char*>(data);
        std::copy(d, d + len, begin() + readerIndex_);
    }

    // Append/peek/read integers in network byte order (big endian)
    void appendInt64(int64_t x) { uint64_t be = htobe64(static_cast<uint64_t>(x)); append(&be, sizeof be); }
    void appendInt32(int32_t x) { uint32_t be = htobe32(static_cast<uint32_t>(x)); append(&be, sizeof be); }
    void appendInt16(int16_t x) { uint16_t be = htobe16(static_cast<uint16_t>(x)); append(&be, sizeof be); }
    void appendInt8(int8_t x) { append(&x, sizeof x); }

    int64_t peekInt64() const {
        assert(readableBytes() >= sizeof(int64_t));
        uint64_t be = 0;
        ::memcpy(&be, peek(), sizeof be);
        return static_cast<int64_t>(be64toh(be));
    }

    int32_t peekInt32() const {
        assert(readableBytes() >= sizeof(int32_t));
        uint32_t be = 0;
        ::memcpy(&be, peek(), sizeof be);
        return static_cast<int32_t>(be32toh(be));
    }

    int16_t peekInt16() const {
        assert(readableBytes() >= sizeof(int16_t));
        uint16_t be = 0;
        ::memcpy(&be, peek(), sizeof be);
        return static_cast<int16_t>(be16toh(be));
    }

    int8_t peekInt8() const {
        assert(readableBytes() >= sizeof(int8_t));
        return static_cast<int8_t>(*peek());
    }

    int64_t readInt64() { int64_t x = peekInt64(); retrieve(sizeof x); return x; }
    int32_t readInt32() { int32_t x = peekInt32(); retrieve(sizeof x); return x; }
    int16_t readInt16() { int16_t x = peekInt16(); retrieve(sizeof x); return x; }
    int8_t readInt8() { int8_t x = peekInt8(); retrieve(sizeof x); return x; }

    void prependInt64(int64_t x) { uint64_t be = htobe64(static_cast<uint64_t>(x)); prepend(&be, sizeof be); }
    void prependInt32(int32_t x) { uint32_t be = htobe32(static_cast<uint32_t>(x)); prepend(&be, sizeof be); }
    void prependInt16(int16_t x) { uint16_t be = htobe16(static_cast<uint16_t>(x)); prepend(&be, sizeof be); }
    void prependInt8(int8_t x) { prepend(&x, sizeof x); }

    // Read data from fd (scatter/gather IO)
    ssize_t readFd(int fd, int* savedErrno);

private:
    char* begin() { return &*buffer_.begin(); }
    const char* begin() const { return &*buffer_.begin(); }

    void makeSpace(size_t len) {
        if (writableBytes() + prependableBytes() < len + kCheapPrepend) {
            buffer_.resize(writerIndex_ + len);
        } else {
            // move readable data to the front, freeing space at the end
            assert(kCheapPrepend < readerIndex_);
            size_t readable = readableBytes();
            std::memmove(begin() + kCheapPrepend, begin() + readerIndex_, readable);
            readerIndex_ = kCheapPrepend;
            writerIndex_ = readerIndex_ + readable;
            assert(readable == readableBytes());
        }
    }

    std::vector<char> buffer_;
    size_t readerIndex_;
    size_t writerIndex_;

    static const char kCRLF[];
};

} // namespace hvnetpp
