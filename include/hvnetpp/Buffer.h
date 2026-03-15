#pragma once

#include <vector>
#include <string>
#include <algorithm>
#include <assert.h>
#include <cstring>

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

    size_t readableBytes() const { return writerIndex_ - readerIndex_; }
    size_t writableBytes() const { return buffer_.size() - writerIndex_; }
    size_t prependableBytes() const { return readerIndex_; }

    const char* peek() const { return begin() + readerIndex_; }

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
};

} // namespace hvnetpp
