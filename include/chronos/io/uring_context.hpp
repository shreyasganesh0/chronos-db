#pragma once
#include <liburing.h>
#include "chronos/io/request.hpp"
#include "chronos/common/result.hpp"

namespace chronos::io {

class UringContext {
public:
    // Initialize the ring with a specific depth (e.g., 256 entries)
    explicit UringContext(unsigned int queue_depth);
    ~UringContext();

    // Disable copying 
    UringContext(const UringContext&) = delete;
    UringContext& operator=(const UringContext&) = delete;

    // 1. Submit a Request
    // Translates IORequest -> io_uring_sqe
    Result<void> submit(IORequest* req);

    // 2. Poll for Completions
    // Checks CQ ring. Calls callbacks. Returns number of events processed.
    int poll();

private:
    struct io_uring ring_;
};

} // namespace chronos::io
