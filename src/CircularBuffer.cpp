#include "hvnetpp/CircularBuffer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>

namespace hvnetpp {
namespace internal {

#ifndef MAP_ANONYMOUS
#  define MAP_ANONYMOUS MAP_ANON
#endif

CircularBuffer::CircularBuffer(unsigned int order)
    : size_(1UL << order),
      data_(nullptr),
      head_(0),
      tail_(0) {
    createBufferMirror();
}

CircularBuffer::~CircularBuffer() {
    if (data_) {
        munmap(data_, size_ << 1);
    }
}

unsigned char* CircularBuffer::headPtr() const {
    if (!data_) {
        return nullptr;
    }
    return data_ + (head_.load(std::memory_order_relaxed) & (size_ - 1));
}

unsigned char* CircularBuffer::tailPtr() const {
    if (!data_) {
        return nullptr;
    }
    return data_ + (tail_.load(std::memory_order_relaxed) & (size_ - 1));
}

unsigned char* CircularBuffer::getPointer(unsigned int offset) const {
    if (!data_) {
        return nullptr;
    }
    return data_ + (offset & (size_ - 1));
}

void CircularBuffer::createBufferMirror() {
    char path[] = "/tmp/cb-XXXXXX";
    int fd;
    
    fd = mkstemp(path);
    if (fd < 0) return;

    if (unlink(path) != 0) {
        close(fd);
        return;
    }

    if (ftruncate(fd, size_) != 0) {
        close(fd);
        return;
    }

    // create the array of data
    data_ = static_cast<unsigned char*>(mmap(NULL, size_ << 1, PROT_NONE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0));
    if (data_ == MAP_FAILED) {
        data_ = nullptr;
        close(fd);
        return;
    }

    void* address = mmap(data_, size_, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED, fd, 0);
    if (address != data_) {
        munmap(data_, size_ << 1);
        data_ = nullptr;
        close(fd);
        return;
    }

    address = mmap(data_ + size_, size_, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_SHARED, fd, 0);
    if (address != data_ + size_) {
        munmap(data_, size_ << 1);
        data_ = nullptr;
        close(fd);
        return;
    }

    close(fd);
}

} // namespace internal
} // namespace hvnetpp
