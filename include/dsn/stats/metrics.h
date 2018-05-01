// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

// Copyright (c) 2018, Xiaomi, Inc.  All rights reserved.
// This source code is licensed under the Apache License Version 2.0, which
// can be found in the LICENSE file in the root directory of this source tree.

#pragma once

#include <cstdint>
#include <cstring>
#include <atomic>

#include <dsn/utility/utils.h>
#include <dsn/utility/synchronize.h>
#include <dsn/utility/autoref_ptr.h>
#include <dsn/cpp/json_helper.h>

// Types of metrics
// ------------------------------------------------------------
// Gauge: Set or get a point-in-time value.
//  - Primitive types (bool, int64_t/uint64_t, double): Lock-free gauges.
// Counter: Get, reset, increment or decrement an int64_t value.
// Histogram: Increment buckets of values segmented by configurable max and precision.
//

#define METRIC_DEFINE_counter(section, app, name, desc) \
    ::dsn::metric_prototype METRIC_##name(::dsn::metric_prototype::ctor_args(section, name))

#define METRIC_DEFINE_histogram(section, name, desc, max_val, num_sig_digits) \
    ::dsn::histogram_prototype METRIC_##name(::dsn::metric_prototype::ctor_args(section, name), max_val, num_sig_digits)

#define METRIC_DECLARE_histogram(name) extern ::dsn::histogram_prototype METRIC_##name

#define METRIC_DECLARE_counter(name)

namespace dsn {
namespace stats {

class metric_type
{
public:
    enum type
    {
        kGauge, kCounter, kHistogram
    };

    static const char *name(type t);

private:
    static const char *const kGaugeType;
    static const char *const kCounterType;
    static const char *const kHistogramType;
};

class metric_prototype
{
public:
    // Simple struct to aggregate the arguments common to all prototypes.
    // This makes constructor chaining a little less tedious.
    struct ctor_args
    {
        ctor_args(const char *section, const char *app, const char *name, const char *description)
                :_section(section), _app(app), _name(name), _description(description)
        {
        }

        const char *const _section;
        const char *const _app;
        const char *const _name;
        const char *const _description;
    };

    const char *section() const { return _args._section; }
    const char *app() const { return _args._app; }
    const char *name() const { return _args._name; }
    const char *description() const { return _args._description; }

    explicit metric_prototype(const ctor_args& args)
            :_args(args)
    {
    }

    virtual metric_type::type type() = 0;

private:
    const ctor_args _args;
};

class metric_base : public ref_counter
{
public:
    explicit metric_base(const metric_prototype *proto) {}
};

class counter_prototype
{

};

// Simple incrementing 64-bit integer.
// Only use Counters in cases that we expect the count to only increase. For example,
// a counter is appropriate for "number of transactions processed by the server",
// but not for "number of transactions currently in flight". Monitoring software
// knows that counters only increase and thus can compute rates over time, rates
// across multiple servers, etc, which aren't appropriate in the case of gauges.
class counter : public metric_base
{
public:
    // Increment the counter by 1.
    void increment() { add(1); }

    // Add the given value to the counter.
    void add(uint64_t val) { _val.fetch_add(val, std::memory_order_relaxed); }

    uint64_t get() { return _val.load(std::memory_order_relaxed); }

private:
    friend class metric_registry;
    explicit counter(const metric_prototype *proto)
            :metric_base(proto) {}

    std::atomic_uint_fast64_t _val;
};

class histogram_prototype : public metric_prototype
{
public:
    histogram_prototype(const ctor_args& args, uint64_t max_val, int num_sig_digits)
            :metric_prototype(args)
    {

    }

    metric_type::type type() override
    {
        return metric_type::kHistogram;
    }
};

class hdr_histogram;
class histogram : public metric_base
{
public:
    struct snapshot
    {
        uint64_t total_count{0};
        uint64_t total_sum{0};
        uint64_t min{0};
        uint64_t max{0};
        double avg{0};
        uint64_t p95{0};
        uint64_t p99{0};
        uint64_t p999{0};
        uint64_t p9999{0};

        DEFINE_JSON_SERIALIZATION(total_count, total_sum, min, max, avg, p95, p99, p999, p9999)
    };

    void record(uint64_t val);

    void get_snapshot(snapshot&) const;

private:
    friend class metric_registry;
    explicit histogram(const metric_prototype *proto)
            :metric_base(proto) {}

    std::unique_ptr<hdr_histogram> _histogram;
};

class metric_registry
{
public:
    dsn::ref_ptr<counter> find_or_create_counter(metric_prototype *proto)
    {
        return find_or_create<counter>(proto);
    }

    dsn::ref_ptr<histogram> find_or_create_histogram(metric_prototype *proto)
    {
        return find_or_create<histogram>(proto);
    }

    std::string list_metrics_in_json();

private:
    template <typename T>
    dsn::ref_ptr<T> find_or_create(metric_prototype *proto)
    {
        utils::auto_write_lock l(_lock);
        auto it = _metric_map.find(proto);
        if (it == _metric_map.end()) {
            auto p = dsn::ref_ptr<T>(new T(proto));
            _metric_map.emplace(proto, p);
            return p;
        } else {
            // downcast from metrics_base to T
            return static_cast<T *>(it->second.get());
        }
    }

private:
    typedef std::unordered_map<metric_prototype *, dsn::ref_ptr<metric_base>> metric_map;

    mutable utils::rw_lock_nr _lock;
    metric_map _metric_map;
};

} // namespace stats
} // namespace dsn
