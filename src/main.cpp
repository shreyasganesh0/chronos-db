#include <iostream>
#include <cassert>
#include "chronos/common/result.hpp"
#include "chronos/common/list.hpp"

using namespace chronos;

// 1. Define a Request struct that hooks into the list
struct TestRequest : public IntrusiveNode {
    int id;
    TestRequest(int id) : id(id) {}
};

int main() {
    std::cout << "[ChronosDB] System Boot..." << std::endl;

    // --- TEST 1: Result<T> ---
    Result<int> res = 42;
    assert(res.is_ok());
    assert(res.value() == 42);
    std::cout << "[PASS] Result<T> verified." << std::endl;

    // --- TEST 2: Intrusive List ---
    IntrusiveList<TestRequest> queue;

    // Create items on the STACK (Zero allocation!)
    TestRequest req1(100);
    TestRequest req2(200);
    TestRequest req3(300);

    // Push them
    queue.push_back(&req1);
    queue.push_back(&req2);
    queue.push_back(&req3);

    // Verify Order (FIFO)
    TestRequest* popped = queue.pop_front();
    assert(popped != nullptr);
    assert(popped->id == 100);
    
    popped = queue.pop_front();
    assert(popped->id == 200);

    popped = queue.pop_front();
    assert(popped->id == 300);

    assert(queue.empty());
    std::cout << "[PASS] IntrusiveList verified." << std::endl;

    return 0;
}
