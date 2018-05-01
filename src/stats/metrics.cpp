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

#include <dsn/stats/metrics.h>
#include <dsn/stats/hdr_histogram.h>

namespace dsn {
namespace stats {


//
// metric_type
//

/*static*/ const char *const metric_type::kGaugeType = "gauge";
/*static*/ const char *const metric_type::kCounterType = "counter";
/*static*/ const char *const metric_type::kHistogramType = "histogram";

/*static*/ const char *metric_type::name(metric_type::type type)
{
    switch (type) {
    case kGauge:return kGaugeType;
    case kCounter:return kCounterType;
    case kHistogram:return kHistogramType;
    default:dassert("unknown type");
        __builtin_unreachable();
    }
}

//
// histogram
//


void histogram::get_snapshot(histogram::snapshot& snap) const
{
    if (_histogram->total_count()>0) {
        snap.total_count = _histogram->total_count();
        snap.total_sum = _histogram->sum();
        snap.max = _histogram->max();
        snap.min = _histogram->min();
        snap.avg = _histogram->avg();
        snap.p95 = _histogram->value_at_percentile(0.95);
        snap.p99 = _histogram->value_at_percentile(0.99);
        snap.p999 = _histogram->value_at_percentile(0.999);
        snap.p9999 = _histogram->value_at_percentile(0.9999);
    }
}

void histogram::record(uint64_t val)
{
    _histogram->record(val);
}

std::string metric_registry::list_metrics_in_json()
{
    // {
    //   "app" : {
    //             "section"
    //                  counter_info[]>>
    //           }
    // }


}

} // namespace stats
} // namespace dsn

