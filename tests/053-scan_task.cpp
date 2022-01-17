#include "catch.hpp"
#include "test-utils.h"
#include "fs/scan_task.h"

using namespace syncspirit;
using namespace syncspirit::test;
using namespace syncspirit::utils;
using namespace syncspirit::model;
using namespace syncspirit::fs;

TEST_CASE("scan_task", "[fs]") {
    utils::set_default("trace");

    auto root_path = bfs::unique_path();
    bfs::create_directories(root_path);
    path_guard_t path_quard{root_path};

    config::fs_config_t config{0 , 3600};
    auto my_id = device_id_t::from_string("KHQNO2S-5QSILRK-YX4JZZ4-7L77APM-QNVGZJT-EKU7IFI-PNEPBMY-4MXFMQD").value();
    auto my_device = device_t::create(my_id, "my-device").value();
    auto cluster = cluster_ptr_t(new cluster_t(my_device, 1));

    auto db_folder = db::Folder();
    db_folder.set_id("some-id");
    db_folder.set_label("zzz");
    db_folder.set_path(root_path.string());
    auto folder = folder_t::create(cluster->next_uuid(), db_folder).value();
    cluster->get_folders().put(folder);

    db::FolderInfo db_folder_info;
    db_folder_info.set_index_id(1234);
    db_folder_info.set_max_sequence(3);
    auto folder_info = folder_info_t::create(cluster->next_uuid(), db_folder_info, my_device, folder).value();
    folder->get_folder_infos().put(folder_info);

    SECTION("without files") {
        SECTION("non-existing dir => err") {
            db_folder.set_path("/some/non-existing/path");

            folder = folder_t::create(cluster->next_uuid(), db_folder).value();
            cluster->get_folders().put(folder);

            auto folder_info = folder_info_t::create(cluster->next_uuid(), db_folder_info, my_device, folder).value();
            folder->get_folder_infos().put(folder_info);

            auto task = scan_task_t(cluster, folder->get_id(), config);
            auto r = task.advance();
            CHECK(std::get_if<io_errors_t>(&r));

            auto errs = std::get_if<io_errors_t>(&r);
            REQUIRE(errs->size() == 1);

            auto& err = errs->at(0);
            CHECK(err.ec);
            CHECK(err.path.string() == db_folder.path());
        }

        SECTION("no dirs, no files") {
            auto task = scan_task_t(cluster, folder->get_id(), config);
            auto r = task.advance();
            CHECK(std::get_if<bool>(&r));
            CHECK(*std::get_if<bool>(&r) == true);

            r = task.advance();
            CHECK(std::get_if<bool>(&r));
            CHECK(*std::get_if<bool>(&r) == false);
        }

        SECTION("some dirs, no files") {
            auto task = scan_task_t(cluster, folder->get_id(), config);
            auto dir = root_path / "some-dir";
            bfs::create_directories(dir);
            auto r = task.advance();
            CHECK(std::get_if<bool>(&r));
            CHECK(*std::get_if<bool>(&r) == true);

            r = task.advance();
            CHECK(std::get_if<bool>(&r));
            CHECK(*std::get_if<bool>(&r) == true);

            r = task.advance();
            CHECK(std::get_if<bool>(&r));
            CHECK(*std::get_if<bool>(&r) == false);
        }

        SECTION("no dirs, file outside of recorded is ignored") {
            auto task = scan_task_t(cluster, folder->get_id(), config);
            write_file(root_path / "some-file", "");
            auto r = task.advance();
            CHECK(std::get_if<bool>(&r));
            CHECK(*std::get_if<bool>(&r) == true);

            r = task.advance();
            CHECK(std::get_if<bool>(&r));
            CHECK(*std::get_if<bool>(&r) == false);
        }
    }

    SECTION("files") {
        auto modified = std::time_t{1642007468};
        auto pr_file = proto::FileInfo{};
        pr_file.set_name("a.txt");
        pr_file.set_sequence(2);

        SECTION("meta is not changed") {
            pr_file.set_block_size(5);
            pr_file.set_size(5);
            pr_file.set_modified_s(modified);

            auto path = root_path / "a.txt";
            write_file(path, "12345");
            bfs::last_write_time(path, modified);

            auto file = file_info_t::create(cluster->next_uuid(), pr_file, folder_info).value();
            folder_info->get_file_infos().put(file);

            auto task = scan_task_t(cluster, folder->get_id(), config);
            auto r = task.advance();
            CHECK(std::get_if<bool>(&r));
            CHECK(*std::get_if<bool>(&r) == true);

            r = task.advance();
            REQUIRE(std::get_if<unchanged_meta_t>(&r));
            auto ref = std::get_if<unchanged_meta_t>(&r);
            CHECK(ref->file == file);

            r = task.advance();
            CHECK(std::get_if<bool>(&r));
            CHECK(*std::get_if<bool>(&r) == false);
        }

        SECTION("meta is changed") {
            auto task = scan_task_ptr_t{};
            auto file = file_info_ptr_t{};

            SECTION("file size differs") {
                pr_file.set_block_size(5);
                pr_file.set_size(6);
                pr_file.set_modified_s(modified);

                auto path = root_path / "a.txt";
                write_file(path, "12345");
                bfs::last_write_time(path, modified);

                file = file_info_t::create(cluster->next_uuid(), pr_file, folder_info).value();
                folder_info->get_file_infos().put(file);
            }

            SECTION("modification time differs") {
                pr_file.set_block_size(5);
                pr_file.set_size(5);
                pr_file.set_modified_s(modified + 1);

                auto path = root_path / "a.txt";
                write_file(path, "12345");
                bfs::last_write_time(path, modified);

                file = file_info_t::create(cluster->next_uuid(), pr_file, folder_info).value();
                folder_info->get_file_infos().put(file);

            }
            task = new scan_task_t(cluster, folder->get_id(), config);
            auto r = task->advance();
            CHECK(std::get_if<bool>(&r));
            CHECK(*std::get_if<bool>(&r) == true);


            r = task->advance();
            REQUIRE(std::get_if<changed_meta_t>(&r));
            auto ref = std::get_if<changed_meta_t>(&r);
            CHECK(ref->file == file);

            r = task->advance();
            CHECK(std::get_if<bool>(&r));
            CHECK(*std::get_if<bool>(&r) == false);
        }

        SECTION("tmp") {
            pr_file.set_block_size(5);
            pr_file.set_size(5);
            pr_file.set_modified_s(modified);

            auto path = root_path / "a.txt.syncspirit-tmp";

            SECTION("size match -> ok, will recalc") {
                write_file(path, "12345");

                auto file = file_info_t::create(cluster->next_uuid(), pr_file, folder_info).value();
                folder_info->get_file_infos().put(file);

                auto task = scan_task_t(cluster, folder->get_id(), config);
                auto r = task.advance();
                CHECK(std::get_if<bool>(&r));
                CHECK(*std::get_if<bool>(&r) == true);

                r = task.advance();
                REQUIRE(std::get_if<incomplete_t>(&r));
                auto ref = std::get_if<incomplete_t>(&r);
                CHECK(ref->file);

                r = task.advance();
                CHECK(std::get_if<bool>(&r));
                CHECK(*std::get_if<bool>(&r) == false);
            }

            SECTION("size mismatch -> remove & ignore") {
                write_file(path, "123456");

                auto file = file_info_t::create(cluster->next_uuid(), pr_file, folder_info).value();
                folder_info->get_file_infos().put(file);

                auto task = scan_task_t(cluster, folder->get_id(), config);
                auto r = task.advance();
                CHECK(std::get_if<bool>(&r));
                CHECK(*std::get_if<bool>(&r) == true);

                r = task.advance();
                CHECK(std::get_if<bool>(&r));
                CHECK(*std::get_if<bool>(&r) == true);

                r = task.advance();
                CHECK(std::get_if<bool>(&r));
                CHECK(*std::get_if<bool>(&r) == false);
                CHECK(!bfs::exists(path));
            }
        }

        SECTION("tmp & non-tmp: tmp is ignored & removed") {
            pr_file.set_block_size(5);
            pr_file.set_size(5);
            pr_file.set_modified_s(modified);

            auto path = root_path / "a.txt";
            auto path_tmp = root_path / "a.txt.syncspirit-tmp";
            write_file(path, "12345");
            write_file(path_tmp, "12345");
            bfs::last_write_time(path, modified);

            auto file = file_info_t::create(cluster->next_uuid(), pr_file, folder_info).value();
            folder_info->get_file_infos().put(file);

            auto task = scan_task_t(cluster, folder->get_id(), config);
            auto r = task.advance();
            CHECK(std::get_if<bool>(&r));
            CHECK(*std::get_if<bool>(&r) == true);

            r = task.advance();
            REQUIRE(std::get_if<unchanged_meta_t>(&r));
            auto ref = std::get_if<unchanged_meta_t>(&r);
            CHECK(ref->file == file);

            r = task.advance();
            CHECK(std::get_if<bool>(&r));
            CHECK(*std::get_if<bool>(&r) == false);
            CHECK(!bfs::exists(path_tmp));
        }
    }
}
