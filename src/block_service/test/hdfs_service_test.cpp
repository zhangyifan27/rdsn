#include <dsn/dist/block_service.h>
#include <dsn/utility/filesystem.h>
#include <dsn/utility/flags.h>
#include <dsn/utility/rand.h>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>

#include "block_service/hdfs/hdfs_service.h"

using namespace dsn;
using namespace dsn::dist::block_service;

static std::string example_name_node = "<hdfs_name_none>";
static std::string example_backup_path = "<hdfs_path>";
// Please modify following paras in 'config-test.ini' to enable hdfs_service_test,
// or hdfs_service_test will be skipped and return true.
DSN_DEFINE_string("hdfs_test", test_name_node, "<hdfs_name_none>", "hdfs name node");
DSN_DEFINE_string("hdfs_test",
                  test_backup_path,
                  "<hdfs_path>",
                  "path for uploading and downloading test files");

DSN_DEFINE_uint32("hdfs_test", num_test_file_lines, 4096, "number of lines in test file");
DSN_DEFINE_uint32("hdfs_test",
                  num_total_files_for_hdfs_concurrent_test,
                  64,
                  "number of total files for hdfs concurrent test");

DEFINE_TASK_CODE(LPC_TEST_HDFS, TASK_PRIORITY_HIGH, dsn::THREAD_POOL_DEFAULT)

class HDFSClientTest : public testing::Test
{
protected:
    virtual void SetUp() override;
    virtual void TearDown() override;
    void generate_test_file(const char *filename);
    std::string name_node;
    std::string backup_path;
};

void HDFSClientTest::SetUp()
{
    name_node = FLAGS_test_name_node;
    backup_path = FLAGS_test_backup_path;
}

void HDFSClientTest::TearDown() {}

void HDFSClientTest::generate_test_file(const char *filename)
{
    // generate a local test file.
    int lines = FLAGS_num_test_file_lines;
    FILE *fp = fopen(filename, "wb");
    for (int i = 0; i < lines; ++i) {
        fprintf(fp, "%04d_this_is_a_simple_test_file\n", i);
    }
    fclose(fp);
}

TEST_F(HDFSClientTest, test_basic_operation)
{
    if (name_node == example_name_node || backup_path == example_backup_path) {
        return;
    }

    std::vector<std::string> args = {name_node, backup_path};
    std::shared_ptr<hdfs_service> s = std::make_shared<hdfs_service>();
    ASSERT_EQ(dsn::ERR_OK, s->initialize(args));

    std::string local_test_file = "test_file";
    std::string remote_test_file = "hdfs_client_test/test_file";
    int64_t test_file_size = 0;

    generate_test_file(local_test_file.c_str());
    dsn::utils::filesystem::file_size(local_test_file, test_file_size);

    create_file_response cf_resp;
    ls_response l_resp;
    upload_response u_resp;
    download_response d_resp;
    read_response r_resp;
    write_response w_resp;
    remove_path_response rem_resp;

    // fisrt clean up all old file in test directory.
    std::cout << "clean up all old files" << std::endl;
    s->remove_path(remove_path_request{"hdfs_client_test", true},
                   LPC_TEST_HDFS,
                   [&rem_resp](const remove_path_response &resp) { rem_resp = resp; },
                   nullptr)
        ->wait();
    ASSERT_TRUE(dsn::ERR_OK == rem_resp.err || dsn::ERR_OBJECT_NOT_FOUND == rem_resp.err);

    // test upload file.
    std::cout << "create and upload: " << remote_test_file << std::endl;
    s->create_file(create_file_request{remote_test_file, true},
                   LPC_TEST_HDFS,
                   [&cf_resp](const create_file_response &r) { cf_resp = r; },
                   nullptr)
        ->wait();
    ASSERT_EQ(cf_resp.err, dsn::ERR_OK);
    cf_resp.file_handle
        ->upload(upload_request{local_test_file},
                 LPC_TEST_HDFS,
                 [&u_resp](const upload_response &r) { u_resp = r; },
                 nullptr)
        ->wait();
    ASSERT_EQ(dsn::ERR_OK, u_resp.err);
    ASSERT_EQ(test_file_size, cf_resp.file_handle->get_size());

    // test list directory.
    s->list_dir(ls_request{"hdfs_client_test"},
                LPC_TEST_HDFS,
                [&l_resp](const ls_response &resp) { l_resp = resp; },
                nullptr)
        ->wait();
    ASSERT_EQ(dsn::ERR_OK, l_resp.err);
    ASSERT_EQ(1, l_resp.entries->size());
    ASSERT_EQ("test_file", l_resp.entries->at(0).entry_name);
    ASSERT_EQ(false, l_resp.entries->at(0).is_directory);

    // test download file.
    std::cout << "test download " << remote_test_file << std::endl;
    s->create_file(create_file_request{remote_test_file, false},
                   LPC_TEST_HDFS,
                   [&cf_resp](const create_file_response &resp) { cf_resp = resp; },
                   nullptr)
        ->wait();
    ASSERT_EQ(dsn::ERR_OK, cf_resp.err);
    ASSERT_EQ(test_file_size, cf_resp.file_handle->get_size());
    std::string local_file_for_download = "test_file_d";
    cf_resp.file_handle
        ->download(download_request{local_file_for_download, 0, -1},
                   LPC_TEST_HDFS,
                   [&d_resp](const download_response &resp) { d_resp = resp; },
                   nullptr)
        ->wait();
    ASSERT_EQ(dsn::ERR_OK, d_resp.err);
    ASSERT_EQ(test_file_size, d_resp.downloaded_size);

    // compare local_test_file and local_file_for_download.
    int64_t file_size = 0;
    dsn::utils::filesystem::file_size(local_file_for_download, file_size);
    ASSERT_EQ(test_file_size, file_size);
    std::string test_file_md5sum;
    dsn::utils::filesystem::md5sum(local_test_file, test_file_md5sum);
    std::string downloaded_file_md5sum;
    dsn::utils::filesystem::md5sum(local_file_for_download, downloaded_file_md5sum);
    ASSERT_EQ(test_file_md5sum, downloaded_file_md5sum);

    // test read and write.
    std::cout << "test read write operation" << std::endl;
    std::string test_write_file = "hdfs_client_test/test_write_file";
    s->create_file(create_file_request{test_write_file, false},
                   LPC_TEST_HDFS,
                   [&cf_resp](const create_file_response &r) { cf_resp = r; },
                   nullptr)
        ->wait();
    ASSERT_EQ(dsn::ERR_OK, cf_resp.err);
    const char *test_buffer = "write_hello_world_for_test";
    int length = strlen(test_buffer);
    dsn::blob bb(test_buffer, 0, length);
    cf_resp.file_handle
        ->write(write_request{bb},
                LPC_TEST_HDFS,
                [&w_resp](const write_response &w) { w_resp = w; },
                nullptr)
        ->wait();
    ASSERT_EQ(dsn::ERR_OK, w_resp.err);
    ASSERT_EQ(length, w_resp.written_size);
    ASSERT_EQ(length, cf_resp.file_handle->get_size());
    std::cout << "test read just written contents" << std::endl;
    cf_resp.file_handle
        ->read(read_request{0, -1},
               LPC_TEST_HDFS,
               [&r_resp](const read_response &r) { r_resp = r; },
               nullptr)
        ->wait();
    ASSERT_EQ(dsn::ERR_OK, r_resp.err);
    ASSERT_EQ(length, r_resp.buffer.length());
    ASSERT_EQ(0, memcmp(r_resp.buffer.data(), test_buffer, length));

    // test partitial read.
    cf_resp.file_handle
        ->read(read_request{5, 10},
               LPC_TEST_HDFS,
               [&r_resp](const read_response &r) { r_resp = r; },
               nullptr)
        ->wait();
    ASSERT_EQ(dsn::ERR_OK, r_resp.err);
    ASSERT_EQ(10, r_resp.buffer.length());
    ASSERT_EQ(0, memcmp(r_resp.buffer.data(), test_buffer + 5, 10));

    // clean up local test files.
    utils::filesystem::remove_path(local_test_file);
    utils::filesystem::remove_path(local_file_for_download);
}

TEST_F(HDFSClientTest, test_concurrent_upload_download)
{
    if (name_node == example_name_node || backup_path == example_backup_path) {
        return;
    }

    std::vector<std::string> args = {name_node, backup_path};
    std::shared_ptr<hdfs_service> s = std::make_shared<hdfs_service>();
    ASSERT_EQ(dsn::ERR_OK, s->initialize(args));

    int total_files = FLAGS_num_total_files_for_hdfs_concurrent_test;
    std::vector<std::string> local_file_names;
    std::vector<std::string> remote_file_names;
    std::vector<std::string> downloaded_file_names;

    std::vector<int64_t> files_size;
    std::vector<std::string> files_md5sum;
    local_file_names.reserve(total_files);
    remote_file_names.reserve(total_files);
    downloaded_file_names.reserve(total_files);
    files_size.reserve(total_files);
    files_md5sum.reserve(total_files);

    // generate test files.
    for (int i = 0; i < total_files; ++i) {
        std::string file_name = "randomfile" + std::to_string(i);
        generate_test_file(file_name.c_str());
        int64_t file_size = 0;
        dsn::utils::filesystem::file_size(file_name, file_size);
        std::string md5sum;
        dsn::utils::filesystem::md5sum(file_name, md5sum);

        local_file_names.emplace_back(file_name);
        remote_file_names.emplace_back("hdfs_concurrent_test/" + file_name);
        downloaded_file_names.emplace_back(file_name + "_d");
        files_size.emplace_back(file_size);
        files_md5sum.emplace_back(md5sum);
    }

    printf("clean up all old files.\n");
    remove_path_response rem_resp;
    s->remove_path(remove_path_request{"hdfs_concurrent_test", true},
                   LPC_TEST_HDFS,
                   [&rem_resp](const remove_path_response &resp) { rem_resp = resp; },
                   nullptr)
        ->wait();
    ASSERT_TRUE(dsn::ERR_OK == rem_resp.err || dsn::ERR_OBJECT_NOT_FOUND == rem_resp.err);

    printf("test concurrent upload files.\n");
    {
        std::vector<block_file_ptr> block_files;
        for (int i = 0; i < total_files; ++i) {
            create_file_response cf_resp;
            s->create_file(create_file_request{remote_file_names[i], true},
                           LPC_TEST_HDFS,
                           [&cf_resp](const create_file_response &resp) { cf_resp = resp; },
                           nullptr)
                ->wait();
            ASSERT_EQ(dsn::ERR_OK, cf_resp.err);
            ASSERT_NE(nullptr, cf_resp.file_handle.get());
            block_files.push_back(cf_resp.file_handle);
        }

        std::vector<dsn::task_ptr> callbacks;
        for (int i = 0; i < total_files; ++i) {
            block_file_ptr p = block_files[i];
            dsn::task_ptr t =
                p->upload(upload_request{local_file_names[i]},
                          LPC_TEST_HDFS,
                          [p, &local_file_names, &files_size, i](const upload_response &resp) {
                              printf("file %s upload finished.\n", local_file_names[i].c_str());
                              ASSERT_EQ(dsn::ERR_OK, resp.err);
                              ASSERT_EQ(files_size[i], resp.uploaded_size);
                              ASSERT_EQ(files_size[i], p->get_size());
                          });
            callbacks.push_back(t);
        }

        for (int i = 0; i < total_files; ++i) {
            callbacks[i]->wait();
        }
    }

    printf("test concurrent download files.\n");
    {
        std::vector<block_file_ptr> block_files;
        for (int i = 0; i < total_files; ++i) {
            create_file_response cf_resp;
            s->create_file(create_file_request{remote_file_names[i], true},
                           LPC_TEST_HDFS,
                           [&cf_resp](const create_file_response &r) { cf_resp = r; },
                           nullptr)
                ->wait();
            ASSERT_EQ(dsn::ERR_OK, cf_resp.err);
            ASSERT_NE(nullptr, cf_resp.file_handle.get());
            block_files.push_back(cf_resp.file_handle);
        }

        std::vector<dsn::task_ptr> callbacks;
        for (int i = 0; i < total_files; ++i) {
            block_file_ptr p = block_files[i];
            dsn::task_ptr t = p->download(
                download_request{downloaded_file_names[i], 0, -1},
                LPC_TEST_HDFS,
                [&files_md5sum, &downloaded_file_names, &files_size, i, p](
                    const download_response &dr) {
                    printf("file %s download finished\n", downloaded_file_names[i].c_str());
                    ASSERT_EQ(dsn::ERR_OK, dr.err);
                    ASSERT_EQ(files_size[i], dr.downloaded_size);
                    ASSERT_EQ(files_size[i], p->get_size());
                    std::string md5;
                    dsn::utils::filesystem::md5sum(downloaded_file_names[i], md5);
                    ASSERT_EQ(files_md5sum[i], md5);
                });
            callbacks.push_back(t);
        }

        for (int i = 0; i < total_files; ++i) {
            callbacks[i]->wait();
        }
    }

    // clean up local test files.
    for (int i = 0; i < total_files; ++i) {
        utils::filesystem::remove_path(local_file_names[i]);
        utils::filesystem::remove_path(downloaded_file_names[i]);
    }
}
