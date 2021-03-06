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

#pragma once

#include <dsn/dist/replication/replication_types.h>
#include <dsn/dist/replication/replication_other_types.h>
#include <dsn/dist/replication/duplication_common.h>
#include <dsn/cpp/json_helper.h>
#include <dsn/tool-api/zlocks.h>

#include <utility>
#include <fmt/format.h>

namespace dsn {
namespace replication {

class app_state;

class duplication_info;
using duplication_info_s_ptr = std::shared_ptr<duplication_info>;

/// This class is thread-safe.
class duplication_info
{
public:
    /// \see meta_duplication_service::new_dup_from_init
    /// \see duplication_info::decode_from_blob
    duplication_info(dupid_t dupid,
                     int32_t appid,
                     int32_t partition_count,
                     uint64_t create_now_ms,
                     std::string remote_cluster_name,
                     std::string meta_store_path)
        : id(dupid),
          app_id(appid),
          remote(std::move(remote_cluster_name)),
          store_path(std::move(meta_store_path)),
          create_timestamp_ms(create_now_ms)
    {
        for (int i = 0; i < partition_count; i++) {
            _progress[i] = {};
        }
    }

    duplication_info() = default;

    void start()
    {
        zauto_write_lock l(_lock);
        _is_altering = true;
        _next_status = duplication_status::DS_START;
    }

    // error will be returned if this state transition is not allowed.
    error_code
    alter_status(duplication_status::type to_status,
                 duplication_fail_mode::type to_fail_mode = duplication_fail_mode::FAIL_SLOW);

    // call this function after data has been persisted on meta storage.
    void persist_status();

    // not thread-safe
    duplication_status::type status() const { return _status; }
    duplication_fail_mode::type fail_mode() const { return _fail_mode; }

    // if this duplication is in valid status.
    bool is_valid() const { return is_duplication_status_valid(_status); }

    ///
    /// alter_progress -> persist_progress
    ///

    // Returns: false if `d` is not supposed to be persisted,
    //          maybe because meta storage is busy or `d` is stale.
    bool alter_progress(int partition_index, decree d);

    void persist_progress(int partition_index);

    void init_progress(int partition_index, decree confirmed);

    // Generates a json blob to be stored in meta storage.
    // The status in json is `next_status`.
    blob to_json_blob() const;

    /// \see meta_duplication_service::recover_from_meta_state
    static duplication_info_s_ptr decode_from_blob(dupid_t dup_id,
                                                   int32_t app_id,
                                                   int32_t partition_count,
                                                   std::string store_path,
                                                   const blob &json);

    // duplication_query_rpc is handled in THREAD_POOL_META_SERVER,
    // which is not thread safe for read.
    void append_if_valid_for_query(const app_state &app,
                                   /*out*/ std::vector<duplication_entry> &entry_list) const;

    duplication_entry to_duplication_entry() const
    {
        duplication_entry entry;
        entry.dupid = id;
        entry.create_ts = create_timestamp_ms;
        entry.remote = remote;
        entry.status = _status;
        entry.__set_fail_mode(_fail_mode);
        entry.__isset.progress = true;
        for (const auto &kv : _progress) {
            if (!kv.second.is_inited) {
                continue;
            }
            entry.progress[kv.first] = kv.second.stored_decree;
        }
        return entry;
    }

    void report_progress_if_time_up();

    // This function should only be used for testing.
    // Not thread-safe.
    bool is_altering() const { return _is_altering; }

    // Test util
    bool equals_to(const duplication_info &rhs) const { return to_string() == rhs.to_string(); }

    // To json encoded string.
    std::string to_string() const;

private:
    friend class duplication_info_test;
    friend class meta_duplication_service_test;

    // Whether there's ongoing meta storage update.
    bool _is_altering{false};

    mutable zrwlock_nr _lock;

    static constexpr int PROGRESS_UPDATE_PERIOD_MS = 5000;          // 5s
    static constexpr int PROGRESS_REPORT_PERIOD_MS = 1000 * 60 * 5; // 5min

    struct partition_progress
    {
        int64_t volatile_decree{invalid_decree};
        int64_t stored_decree{invalid_decree};
        bool is_altering{false};
        uint64_t last_progress_update_ms{0};
        bool is_inited{false};
    };

    // partition_idx => progress
    std::map<int, partition_progress> _progress;

    uint64_t _last_progress_report_ms{0};

    duplication_status::type _status{duplication_status::DS_INIT};
    duplication_status::type _next_status{duplication_status::DS_INIT};

    duplication_fail_mode::type _fail_mode{duplication_fail_mode::FAIL_SLOW};
    duplication_fail_mode::type _next_fail_mode{duplication_fail_mode::FAIL_SLOW};
    struct json_helper
    {
        std::string remote;
        duplication_status::type status;
        int64_t create_timestamp_ms;
        duplication_fail_mode::type fail_mode;

        DEFINE_JSON_SERIALIZATION(remote, status, create_timestamp_ms, fail_mode);
    };

public:
    const dupid_t id{0};
    const int32_t app_id{0};
    const std::string remote;
    const std::string store_path; // store path on meta service = get_duplication_path(app, dupid)
    const uint64_t create_timestamp_ms{0}; // the time when this dup is created.
};

extern void json_encode(dsn::json::JsonWriter &out, const duplication_status::type &s);

extern bool json_decode(const dsn::json::JsonObject &in, duplication_status::type &s);

extern void json_encode(dsn::json::JsonWriter &out, const duplication_fail_mode::type &s);

extern bool json_decode(const dsn::json::JsonObject &in, duplication_fail_mode::type &s);

// Macros for writing log message prefixed by appid and dupid.
#define ddebug_dup(_dup_, ...)                                                                     \
    ddebug_f("[a{}d{}] {}", _dup_->app_id, _dup_->id, fmt::format(__VA_ARGS__));
#define dwarn_dup(_dup_, ...)                                                                      \
    dwarn_f("[a{}d{}] {}", _dup_->app_id, _dup_->id, fmt::format(__VA_ARGS__));
#define derror_dup(_dup_, ...)                                                                     \
    derror_f("[a{}d{}] {}", _dup_->app_id, _dup_->id, fmt::format(__VA_ARGS__));
#define dfatal_dup(_dup_, ...)                                                                     \
    dfatal_f("[a{}d{}] {}", _dup_->app_id, _dup_->id, fmt::format(__VA_ARGS__));
#define dassert_dup(_pred_, _dup_, ...)                                                            \
    dassert_f(_pred_, "[a{}d{}] {}", _dup_->app_id, _dup_->id, fmt::format(__VA_ARGS__));

} // namespace replication
} // namespace dsn
