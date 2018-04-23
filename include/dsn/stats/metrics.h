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
#include <dsn/utility/utils.h>
#include <cstring>
#include <atomic>


// Types of metrics
// ------------------------------------------------------------
// Gauge: Set or get a point-in-time value.
//  - Primitive types (bool, int64_t/uint64_t, double): Lock-free gauges.
// Counter: Get, reset, increment or decrement an int64_t value.
// Histogram: Increment buckets of values segmented by configurable max and precision.
//

#define METRIC_DEFINE_counter(entity, name, label, unit, desc)
#define METRIC_DEFINE_histogram(entity, name, label, unit, desc, max_val, num_sig_digits)

#define METRIC_DECLARE_histogram(name)
#define METRIC_DECLARE_counter(name)

namespace dsn {
namespace stats {

} // namespace stats
} // namespace dsn
