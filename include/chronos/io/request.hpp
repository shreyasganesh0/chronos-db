#pragma once
#include <span>
#include <functional>
#include "chronos/common/list.hpp"
#include "chronos/common/result.hpp"

namespace chronos::io {

struct IORequest;

// The Callback Signature
// ctx: The pointer to the request itself (usually)
// res: The result of the operation (bytes read or error code)
using IOCallback = void(*)(IORequest* req, Result<int> res);

struct IORequest : public IntrusiveNode {
    enum class Type { READ, WRITE, NOP };

    Type op = Type::NOP;
    int fd = -1;
    size_t offset = 0;
    
    std::span<char> buffer; 
    
    // Who to call when finished
    IOCallback callback = nullptr;

    static void prepare_read(IORequest* req, int fd, std::span<char> buf, size_t offset, IOCallback cb) {
        req->op = Type::READ;
        req->fd = fd;
        req->buffer = buf;
        req->offset = offset;
        req->callback = cb;
    }
};

} // namespace chronos::io
