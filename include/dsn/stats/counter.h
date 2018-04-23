// Copyright (c) 2018, Xiaomi, Inc.  All rights reserved.
// This source code is licensed under the Apache License Version 2.0, which
// can be found in the LICENSE file in the root directory of this source tree.

#pragma once

#include <atomic>

namespace dsn {
namespace stats {

// Simple incrementing 64-bit integer.
// Only use Counters in cases that we expect the count to only increase. For example,
// a counter is appropriate for "number of transactions processed by the server",
// but not for "number of transactions currently in flight". Monitoring software
// knows that counters only increase and thus can compute rates over time, rates
// across multiple servers, etc, which aren't appropriate in the case of gauges.
class counter {
public:
    counter(const char* app,
            const char* section,
            const char* name,
            const char* dsptr)
    {
    }

    // Increment the counter by 1.
    void increment()
    {
        add(1);
    }

    // Add the given value to the counter.
    void add(uint64_t val)
    {
        _val.fetch_add(val, std::memory_order_relaxed);
    }

    uint64_t get()
    {
        return _val.load(std::memory_order_relaxed);
    }

private:
    std::atomic_uint_fast64_t _val;
};

} // namespace stats
} // namespace dsn
