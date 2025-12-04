#pragma once

#include <cstdint>

namespace chronos::io {

    struct IntrusiveHook {

        IntrusiveHook *next = nullptr;
    };

    class IIOContext {
    public:

        virtual ~IIOContext() = default;

        //delete copy
        IIOContext(const IIOContext&) = delete;
        IIOContext& operator=(const IIOContext&) = delete;

        // allow move
        IIOContext(IIOContext&&) = default;
        IIOContext& operator=(IIOContext&&) = default;
        
        //pure virtual so direct refernce of class invalid
        virtual void submit(IntrusiveHook*) = 0;

        virtual void poll() = 0;
    };
};
