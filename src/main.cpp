#include <iostream>
#include <cassert>
#include "chronos/common/result.hpp"

using namespace chronos;

// 1. Test Function returning a Value
Result<int> divide(int a, int b) {
    if (b == 0) return Error::GenericError; // Implicit conversion from Enum
    return a / b;                           // Implicit conversion from int
}

// 2. Test Function returning Void (Status only)
Result<void> trigger_io(bool success) {
    if (!success) return Error::IOError;
    return Result<void>::success();
}

int main() {
    std::cout << "Running Result<T> Unit Tests..." << std::endl;

    // Test 1: Success Path
    auto res_ok = divide(10, 2);
    assert(res_ok.is_ok());
    assert(res_ok.value() == 5);

    // Test 2: Failure Path
    auto res_err = divide(10, 0);
    assert(res_err.is_err());
    assert(res_err.error() == Error::GenericError);

    // Test 3: Void Success
    auto void_ok = trigger_io(true);
    assert(void_ok.is_ok());

    // Test 4: Void Failure
    auto void_err = trigger_io(false);
    assert(void_err.is_err());
    assert(void_err.error() == Error::IOError);

    std::cout << "All Tests Passed." << std::endl;
    return 0;
}
