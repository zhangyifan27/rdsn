#include "hdfs_service.h"

#include <algorithm>
#include <fstream>

#include <dsn/dist/fmt_logging.h>
#include <dsn/tool-api/async_calls.h>
#include <dsn/tool-api/task.h>
#include <dsn/tool-api/task_tracker.h>
#include <dsn/utility/error_code.h>
#include <dsn/utility/filesystem.h>
#include <dsn/utility/flags.h>
#include <dsn/utility/safe_strerror_posix.h>
#include <dsn/utility/utils.h>

namespace dsn {
namespace dist {
namespace block_service {

DEFINE_THREAD_POOL_CODE(THREAD_POOL_HDFS_SERVICE)
DEFINE_TASK_CODE(LPC_HDFS_SERVICE_CALL, TASK_PRIORITY_COMMON, THREAD_POOL_HDFS_SERVICE)

DSN_DEFINE_uint64("replication",
                  hdfs_read_batch_size_bytes,
                  64 << 20,
                  "hdfs read batch size, the default value is 64MB");

DSN_DEFINE_uint64("replication",
                  hdfs_write_batch_size_bytes,
                  64 << 20,
                  "hdfs write batch size, the default value is 64MB");

hdfs_service::hdfs_service() {}

hdfs_service::~hdfs_service() { hdfsDisconnect(_fs); }

error_code hdfs_service::initialize(const std::vector<std::string> &args)
{
    if (args.size() != 2) {
        return ERR_INVALID_PARAMETERS;
    }
    _hdfs_nn = args[0];
    _hdfs_path = args[1];
    return create_fs();
}

error_code hdfs_service::create_fs()
{
    ddebug_f("start to create fs.");
    _fs = nullptr;
    hdfsBuilder *builder = hdfsNewBuilder();
    hdfsBuilderSetNameNode(builder, _hdfs_nn.c_str());
    _fs = hdfsBuilderConnect(builder);
    if (!_fs) {
        derror_f(
            "Fail to connect hdfs name node {}, error: {}.", _hdfs_nn, utils::safe_strerror(errno));
        return ERR_FS_INTERNAL;
    }
    ddebug_f("Succeed to connect hdfs name node {}.", _hdfs_nn);
    return ERR_OK;
}

std::string hdfs_service::get_entry_name(const std::string &hdfs_path)
{
    // get entry name from a hdfs path.
    int i = hdfs_path.length() - 1;
    for (; i >= 0; --i) {
        if (hdfs_path[i] == '/') {
            break;
        }
    }
    return hdfs_path.substr(i + 1);
}

dsn::task_ptr hdfs_service::list_dir(const ls_request &req,
                                     dsn::task_code code,
                                     const ls_callback &cb,
                                     dsn::task_tracker *tracker = nullptr)
{
    ls_future_ptr tsk(new ls_future(code, cb, 0));
    tsk->set_tracker(tracker);

    auto list_dir_background = [this, req, tsk]() {
        std::string path = ::dsn::utils::filesystem::path_combine(_hdfs_path, req.dir_name);
        ls_response resp;
        if (!_fs && create_fs() != ERR_OK) {
            resp.err = ERR_FS_INTERNAL;
            tsk->enqueue_with(resp);
            return;
        }

        if (hdfsExists(_fs, path.c_str()) == -1) {
            derror_f("HDFS list directory failed: path {} not found.", path);
            resp.err = ERR_OBJECT_NOT_FOUND;
            tsk->enqueue_with(resp);
            return;
        }

        hdfsFileInfo *dir_info = hdfsGetPathInfo(_fs, path.c_str());
        if (dir_info == nullptr) {
            derror_f("HDFS get path {} failed.", path);
            resp.err = ERR_FS_INTERNAL;
        } else if (dir_info->mKind == kObjectKindFile) {
            derror_f("HDFS list directory failed, {} is not a directory", path);
            resp.err = ERR_INVALID_PARAMETERS;
        } else {
            int entries = 0;
            hdfsFileInfo *info = hdfsListDirectory(_fs, path.c_str(), &entries);
            if (info == nullptr) {
                derror_f("HDFS list directory {} failed.", path);
                resp.err = ERR_FS_INTERNAL;
            } else {
                for (int i = 0; i < entries; i++) {
                    ls_entry tentry;
                    tentry.entry_name = get_entry_name(std::string(info[i].mName));
                    tentry.is_directory = (info[i].mKind == kObjectKindDirectory);
                    resp.entries->emplace_back(tentry);
                }
                resp.err = ERR_OK;
            }
            hdfsFreeFileInfo(info, entries);
        }
        hdfsFreeFileInfo(dir_info, 1);
        tsk->enqueue_with(resp);
    };

    dsn::tasking::enqueue(LPC_HDFS_SERVICE_CALL, nullptr, list_dir_background);
    return tsk;
}

dsn::task_ptr hdfs_service::create_file(const create_file_request &req,
                                        dsn::task_code code,
                                        const create_file_callback &cb,
                                        dsn::task_tracker *tracker = nullptr)
{
    create_file_future_ptr tsk(new create_file_future(code, cb, 0));
    tsk->set_tracker(tracker);
    std::string hdfs_file = ::dsn::utils::filesystem::path_combine(_hdfs_path, req.file_name);

    if (req.ignore_metadata) {
        create_file_response resp;
        resp.err = ERR_OK;
        resp.file_handle = new hdfs_file_object(this, hdfs_file);
        tsk->enqueue_with(resp);
        return tsk;
    }

    auto create_file_in_background = [this, req, hdfs_file, tsk]() {
        create_file_response resp;
        resp.err = ERR_IO_PENDING;
        dsn::ref_ptr<hdfs_file_object> f = new hdfs_file_object(this, hdfs_file);
        resp.err = f->get_file_meta();
        if (resp.err == ERR_OK || resp.err == ERR_OBJECT_NOT_FOUND) {
            resp.err = ERR_OK;
            resp.file_handle = f;
            ddebug_f("create remote file {} succeed", hdfs_file);
        }
        tsk->enqueue_with(resp);
    };

    dsn::tasking::enqueue(LPC_HDFS_SERVICE_CALL, nullptr, create_file_in_background);
    return tsk;
}

dsn::task_ptr hdfs_service::remove_path(const remove_path_request &req,
                                        dsn::task_code code,
                                        const remove_path_callback &cb,
                                        dsn::task_tracker *tracker)
{
    remove_path_future_ptr tsk(new remove_path_future(code, cb, 0));
    tsk->set_tracker(tracker);

    auto remove_path_background = [this, req, tsk]() {
        std::string path = ::dsn::utils::filesystem::path_combine(_hdfs_path, req.path);
        remove_path_response resp;
        if (!_fs && create_fs() != ERR_OK) {
            resp.err = ERR_FS_INTERNAL;
            tsk->enqueue_with(resp);
            return;
        }

        // Check if path exists.
        if (hdfsExists(_fs, path.c_str()) == -1) {
            derror_f("HDFS remove_path failed: path {} not found.", path);
            resp.err = ERR_OBJECT_NOT_FOUND;
            tsk->enqueue_with(resp);
            return;
        }

        int entries = 0;
        hdfsFileInfo *info = hdfsListDirectory(_fs, path.c_str(), &entries);
        hdfsFreeFileInfo(info, entries);
        if (entries > 0 && !req.recursive) {
            derror_f("HDFS remove_path failed: directory {} is not empty.", path);
            resp.err = ERR_DIR_NOT_EMPTY;
            tsk->enqueue_with(resp);
            return;
        }

        // Remove directory now.
        if (hdfsDelete(_fs, path.c_str(), req.recursive) == -1) {
            derror_f("HDFS remove_path {} failed.", path);
            resp.err = ERR_FS_INTERNAL;
        } else {
            resp.err = ERR_OK;
        }
        tsk->enqueue_with(resp);
    };

    dsn::tasking::enqueue(LPC_HDFS_SERVICE_CALL, nullptr, remove_path_background);
    return tsk;
}

hdfs_file_object::hdfs_file_object(hdfs_service *s, const std::string &name)
    : block_file(name), _service(s), _md5sum(""), _size(0), _has_meta_synced(false)
{
}

error_code hdfs_file_object::get_file_meta()
{
    if (!_service->get_fs() && _service->create_fs() != ERR_OK) {
        return ERR_FS_INTERNAL;
    }
    if (hdfsExists(_service->get_fs(), file_name().c_str()) == -1) {
        dwarn_f("HDFS file {} does not exist.", file_name());
        return ERR_OBJECT_NOT_FOUND;
    }
    hdfsFileInfo *info = hdfsGetPathInfo(_service->get_fs(), file_name().c_str());
    if (info == nullptr) {
        derror_f("HDFS get file info failed, file: {}.", file_name());
        return ERR_FS_INTERNAL;
    }
    _size = info->mSize;
    _has_meta_synced = true;
    hdfsFreeFileInfo(info, 1);
    return ERR_OK;
}

hdfs_file_object::~hdfs_file_object() {}

error_code hdfs_file_object::write_data_in_batches(const char *data,
                                                   const uint64_t &data_size,
                                                   uint64_t &written_size)
{
    written_size = 0;
    if (!_service->get_fs() && _service->create_fs() != ERR_OK) {
        return ERR_FS_INTERNAL;
    }
    hdfsFile writeFile =
        hdfsOpenFile(_service->get_fs(), file_name().c_str(), O_WRONLY | O_CREAT, 0, 0, 0);
    if (!writeFile) {
        derror_f("Failed to open hdfs file {} for writting, error: {}.",
                 file_name(),
                 utils::safe_strerror(errno));
        return ERR_FS_INTERNAL;
    }
    uint64_t cur_pos = 0;
    uint64_t write_len = 0;
    while (cur_pos < data_size) {
        write_len = std::min(data_size - cur_pos, FLAGS_hdfs_write_batch_size_bytes);
        tSize num_written_bytes = hdfsWrite(
            _service->get_fs(), writeFile, (void *)(data + cur_pos), static_cast<tSize>(write_len));
        if (num_written_bytes == -1) {
            derror_f("Failed to write hdfs file {}, error: {}.",
                     file_name(),
                     utils::safe_strerror(errno));
            hdfsCloseFile(_service->get_fs(), writeFile);
            return ERR_FS_INTERNAL;
        }
        cur_pos += num_written_bytes;
    }
    if (hdfsHFlush(_service->get_fs(), writeFile) != 0) {
        derror_f(
            "Failed to flush hdfs file {}, error: {}.", file_name(), utils::safe_strerror(errno));
        hdfsCloseFile(_service->get_fs(), writeFile);
        return ERR_FS_INTERNAL;
    }
    written_size = cur_pos;
    if (hdfsCloseFile(_service->get_fs(), writeFile) != 0) {
        derror_f(
            "Failed to close hdfs file {}, error: {}", file_name(), utils::safe_strerror(errno));
        return ERR_FS_INTERNAL;
    }

    ddebug("start to synchronize meta data after successfully wrote data to hdfs");
    return get_file_meta();
}

dsn::task_ptr hdfs_file_object::write(const write_request &req,
                                      dsn::task_code code,
                                      const write_callback &cb,
                                      dsn::task_tracker *tracker = nullptr)
{
    add_ref();
    write_future_ptr tsk(new write_future(code, cb, 0));
    tsk->set_tracker(tracker);
    auto write_background = [this, req, tsk]() {
        write_response resp;
        resp.err = write_data_in_batches(req.buffer.data(), req.buffer.length(), resp.written_size);
        tsk->enqueue_with(resp);
        release_ref();
    };
    ::dsn::tasking::enqueue(LPC_HDFS_SERVICE_CALL, nullptr, std::move(write_background));
    return tsk;
}

dsn::task_ptr hdfs_file_object::upload(const upload_request &req,
                                       dsn::task_code code,
                                       const upload_callback &cb,
                                       dsn::task_tracker *tracker = nullptr)
{
    upload_future_ptr t(new upload_future(code, cb, 0));
    t->set_tracker(tracker);

    add_ref();
    auto upload_background = [this, req, t]() {
        upload_response resp;
        resp.uploaded_size = 0;
        std::ifstream is(req.input_local_name, std::ios::binary | std::ios::in);
        if (is.is_open()) {
            int64_t file_sz = 0;
            dsn::utils::filesystem::file_size(req.input_local_name, file_sz);
            char *buffer = new char[file_sz + 1];
            is.read(buffer, file_sz);
            is.close();
            resp.err = write_data_in_batches(buffer, file_sz, resp.uploaded_size);
        } else {
            derror_f("HDFS upload failed: open local file {} failed when upload to {}, error: {}",
                     req.input_local_name,
                     file_name(),
                     utils::safe_strerror(errno));
            resp.err = dsn::ERR_FILE_OPERATION_FAILED;
        }
        t->enqueue_with(resp);
        release_ref();
    };

    dsn::tasking::enqueue(LPC_HDFS_SERVICE_CALL, nullptr, upload_background);
    return t;
}

error_code hdfs_file_object::read_data_in_batches(uint64_t start_pos,
                                                  int64_t length,
                                                  std::string &read_buffer,
                                                  size_t &read_length)
{
    // get file meta if it is not synchronized.
    if (!_has_meta_synced) {
        error_code err = get_file_meta();
        if (err != ERR_OK) {
            derror_f("Failed to read remote file {}", file_name());
            return err;
        }
    }
    if (!_service->get_fs() && _service->create_fs() != ERR_OK) {
        return ERR_FS_INTERNAL;
    }

    hdfsFile readFile = hdfsOpenFile(_service->get_fs(), file_name().c_str(), O_RDONLY, 0, 0, 0);
    if (!readFile) {
        derror_f("Failed to open hdfs file {} for reading, error: {}.",
                 file_name(),
                 utils::safe_strerror(errno));
        return ERR_FS_INTERNAL;
    }
    char *buf = new char[_size + 1];
    char *dst_buf = buf;

    // if length = -1, we should read the whole file.
    uint64_t data_length = (length == -1 ? _size : length);
    uint64_t cur_pos = start_pos;
    uint64_t read_size = 0;
    while (cur_pos < start_pos + data_length) {
        read_size = std::min(start_pos + data_length - cur_pos, FLAGS_hdfs_read_batch_size_bytes);
        tSize num_read_bytes = hdfsPread(_service->get_fs(),
                                         readFile,
                                         static_cast<tOffset>(cur_pos),
                                         (void *)dst_buf,
                                         static_cast<tSize>(read_size));
        if (num_read_bytes > 0) {
            cur_pos += num_read_bytes;
            dst_buf += num_read_bytes;
        } else if (num_read_bytes == -1) {
            derror_f("Failed to read hdfs file {}, error: {}.",
                     file_name(),
                     utils::safe_strerror(errno));
            hdfsCloseFile(_service->get_fs(), readFile);
            return ERR_FS_INTERNAL;
        }
    }
    read_length = cur_pos - start_pos;
    read_buffer = std::string(buf, dst_buf - buf);
    if (hdfsCloseFile(_service->get_fs(), readFile) != 0) {
        derror_f(
            "Failed to close hdfs file {}, error: {}.", file_name(), utils::safe_strerror(errno));
        return ERR_FS_INTERNAL;
    }
    return ERR_OK;
}

dsn::task_ptr hdfs_file_object::read(const read_request &req,
                                     dsn::task_code code,
                                     const read_callback &cb,
                                     dsn::task_tracker *tracker = nullptr)
{
    read_future_ptr tsk(new read_future(code, cb, 0));
    tsk->set_tracker(tracker);

    add_ref();
    auto read_func = [this, req, tsk]() {
        size_t read_length = 0;
        read_response resp;
        std::string read_buffer;
        resp.err =
            read_data_in_batches(req.remote_pos, req.remote_length, read_buffer, read_length);
        if (resp.err == ERR_OK) {
            resp.buffer = blob::create_from_bytes(std::move(read_buffer));
        }
        tsk->enqueue_with(resp);
        release_ref();
    };

    dsn::tasking::enqueue(LPC_HDFS_SERVICE_CALL, nullptr, std::move(read_func));
    return tsk;
}

dsn::task_ptr hdfs_file_object::download(const download_request &req,
                                         dsn::task_code code,
                                         const download_callback &cb,
                                         dsn::task_tracker *tracker = nullptr)
{
    download_future_ptr t(new download_future(code, cb, 0));
    t->set_tracker(tracker);

    add_ref();
    auto download_background = [this, req, t]() {
        download_response resp;
        resp.downloaded_size = 0;
        std::string read_buffer;
        resp.err = read_data_in_batches(
            req.remote_pos, req.remote_length, read_buffer, resp.downloaded_size);
        if (resp.err == ERR_OK) {
            std::ofstream out(req.output_local_name,
                              std::ios::binary | std::ios::out | std::ios::trunc);
            if (out.is_open()) {
                out << read_buffer;
                out.close();
            } else {
                derror_f("HDFS download failed: fail to open localfile {} when download {}, "
                         "error: {}",
                         req.output_local_name,
                         file_name(),
                         utils::safe_strerror(errno));
                resp.err = ERR_FILE_OPERATION_FAILED;
                resp.downloaded_size = 0;
            }
        }
        t->enqueue_with(resp);
        release_ref();
    };

    dsn::tasking::enqueue(LPC_HDFS_SERVICE_CALL, nullptr, download_background);
    return t;
}
} // namespace block_service
} // namespace dist
} // namespace dsn
