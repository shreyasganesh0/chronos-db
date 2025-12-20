# IO Uring

## Basics
- has two interfaces, low level io_uring and higher level api from liburing
- uses ring buffers b/n kernel and userspace
- minimize the syscalls to needed
- Algorithm
    - 2 ring buffers SQ(submission of requests) and CQ(completion of requests)
    - set them up using io_uring_setup()
        - map to userspace using mmap
    - submit action requests as SQE (SQ entries) added to the tail of SQ
        - actions are things like read/write, accept connecitons (other I/O)
    - io_uring_enter() used to notify kernel of SQE to SQ
        - can add multiple (batching before making this call)
        - io_uring_enter() can also be used to wait for N entries
        to be processed before checking
    - processed requests added as CQEs to the tail of the CQ ring buffer
    - FIFO read from CQ head for ordered read in order of SQE
    - polling mode allows kernel to poll for entries in SQ instead 
    of having to call io_uring_enter() on each submission

## Low Level Interface
- Completion Queue Entry
```
struct io_uring_cqe {

    __u64 user_data;
    __s32 res;
    __u32 flags;
};
```
    - user_data is a field that is passed as is from whatever is added
    to the sqe->user_data field
        - could be used to tag particular requests since requests may not complete in the same order they were submitted
        - usually done by passing a pointer that can be used to identify the type of request that was completed
    - res field contains result of the system call
        - read would contain number of bytes read in the res field or the error (basically whatever read would return)

- Ordering can be fixed using linking requests as well

- Submission Queue Entry
```
struct io_uring_sqe {
    __u8 opcode; // type of operation for sqe
    __u8 flags;
    __u16 ioprio; // ioprio for request
    __s32 fd; // file descriptor to do IO on
    __u64 off; // offset into file
    __u64 addr; // pointer to buffer or iovecs
    __u32 len; // buffer size for iovecs
    union {
        __kernel_rwf_t rw_flags;
        __u32 fsync_flags;
        __u16 poll_events;
        __u32 sync_range_flags;
        __u32 msg_flags;
    };
    __u64 user_data; // data to be passed back at completition
    union {
        __u16 buf_index; // index into buffer if used
        __u64 __pad2[3];
    };
};
```
