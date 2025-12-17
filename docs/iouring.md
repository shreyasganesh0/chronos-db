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
