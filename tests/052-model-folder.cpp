#include "catch.hpp"
#include "test-utils.h"
#include "model/file_info.h"
#include "model/block_iterator.h"
#include "model/cluster.h"

using namespace syncspirit;
using namespace syncspirit::test;
using namespace syncspirit::model;

namespace bfs = boost::filesystem;

TEST_CASE("folder, update local files", "[model]") {
    db::Device db_d1;
    db_d1.set_id(test::device_id2sha256("KHQNO2S-5QSILRK-YX4JZZ4-7L77APM-QNVGZJT-EKU7IFI-PNEPBMY-4MXFMQD"));
    auto d1 = model::device_ptr_t(new model::device_t(db_d1, 2));

    db::Folder db_f;
    db_f.set_id("2");
    db_f.set_label("f2-label");
    db_f.set_path("/some/path");
    auto folder = model::folder_ptr_t(new model::folder_t(db_f, 1));

    cluster_ptr_t cluster = new cluster_t(d1);
    auto folders = model::folders_map_t();
    folders.put(folder);
    cluster->assign_folders(std::move(folders));
    folder->assign_cluster(cluster.get());
    folder->assign_device(d1);

    db::FolderInfo db_folderinfo;
    auto folder_info = folder_info_ptr_t(new folder_info_t(db_folderinfo, d1.get(), folder.get(), 3));
    folder->add(folder_info);

    SECTION("deleted file (in cluster) is not present (on disk)") {
        auto db_file = db::FileInfo();
        db_file.set_name("my-file.txt");
        db_file.set_sequence(5);
        db_file.set_type(proto::FileInfoType::FILE);
        db_file.set_deleted(true);
        auto file = model::file_info_ptr_t(new file_info_t(db_file, folder_info.get()));
        folder_info->add(file);

        auto local_file_map = model::local_file_map_t(bfs::path("/some/path"));
        folder->update(local_file_map);
        CHECK(file->is_dirty() == false);
    }

    SECTION("updating blocks") {
        db::BlockInfo db_b1;
        db_b1.set_hash("h1");
        db_b1.set_size(5);
        auto b1 = model::block_info_ptr_t(new model::block_info_t(db_b1, 1));

        db::BlockInfo db_b2;
        db_b2.set_hash("h2");
        db_b2.set_size(5);
        auto b2 = model::block_info_ptr_t(new model::block_info_t(db_b2, 2));

        db::BlockInfo db_b3;
        db_b3.set_hash("h3");
        db_b3.set_size(5);
        auto b3 = model::block_info_ptr_t(new model::block_info_t(db_b3, 3));

        cluster->get_blocks().put(b1);
        cluster->get_blocks().put(b2);

        auto db_file = db::FileInfo();
        db_file.set_name("my-file.txt");
        db_file.set_sequence(5);
        db_file.set_type(proto::FileInfoType::FILE);
        db_file.add_blocks_keys(1ul);
        db_file.add_blocks_keys(2ul);
        auto file = model::file_info_ptr_t(new file_info_t(db_file, folder_info.get()));
        folder_info->add(file);

        REQUIRE(file->get_blocks().size() == 2);

        model::local_file_t local_file;

        SECTION("fs blocks match db blocks") {
            local_file.blocks.emplace_back(b1);
            local_file.blocks.emplace_back(b2);
            auto local_file_map = model::local_file_map_t(bfs::path("/some/path"));
            local_file_map.map[bfs::path("my-file.txt")] = local_file;

            folder->update(local_file_map);
            CHECK(!file->is_dirty());
            CHECK(!file->is_incomplete());
            auto &blocks = file->get_blocks();
            CHECK(blocks.size() == 2);
            CHECK(blocks[0] == b1);
            CHECK(blocks[1] == b2);
        }

        SECTION("incomplete temporally file, equal blocks sizes") {
            local_file.blocks.emplace_back(b1);
            local_file.blocks.emplace_back(b3);
            local_file.temp = true;
            auto local_file_map = model::local_file_map_t(bfs::path("/some/path"));
            local_file_map.map[bfs::path("my-file.txt")] = local_file;

            folder->update(local_file_map);
            CHECK(file->is_dirty());
            CHECK(file->is_incomplete());
            auto &blocks = file->get_blocks();
            CHECK(blocks.size() == 2);
            CHECK(blocks[0] == b1);
            CHECK(!blocks[1]);
        }

        SECTION("incomplete temporally file, non-equal blocks sizes") {
            local_file.blocks.emplace_back(b1);
            local_file.blocks.emplace_back(b1);
            local_file.blocks.emplace_back(b1);
            local_file.temp = true;
            auto local_file_map = model::local_file_map_t(bfs::path("/some/path"));
            local_file_map.map[bfs::path("my-file.txt")] = local_file;

            folder->update(local_file_map);
            CHECK(file->is_dirty());
            CHECK(file->is_incomplete());
            auto &blocks = file->get_blocks();
            CHECK(blocks.size() == 2);
            CHECK(blocks[0] == b1);
            CHECK(!blocks[1]);
        }
    }
}

TEST_CASE("network update", "[model]") {
    std::uint64_t key = 0;
    db::Device db_d;
    db_d.set_id(test::device_id2sha256("KHQNO2S-5QSILRK-YX4JZZ4-7L77APM-QNVGZJT-EKU7IFI-PNEPBMY-4MXFMQD"));
    auto device = model::device_ptr_t(new model::device_t(db_d, ++key));

    db::Folder db_f;
    db_f.set_id("2");
    db_f.set_label("f2-label");
    db_f.set_path("/some/path");
    auto folder = model::folder_ptr_t(new model::folder_t(db_f, ++key));

    cluster_ptr_t cluster = new cluster_t(device);
    auto folders = model::folders_map_t();
    folders.put(folder);
    cluster->assign_folders(std::move(folders));
    folder->assign_cluster(cluster.get());
    folder->assign_device(device);

    db::FolderInfo db_folderinfo;
    auto folder_info = folder_info_ptr_t(new folder_info_t(db_folderinfo, device.get(), folder.get(), ++key));

    db::BlockInfo db_b1;
    db_b1.set_hash("hash-1");
    db_b1.set_size(5);
    auto b1 = model::block_info_ptr_t(new model::block_info_t(db_b1, ++key));

    db::BlockInfo db_b2;
    db_b2.set_hash("hash-2");
    db_b2.set_size(5);
    auto b2 = model::block_info_ptr_t(new model::block_info_t(db_b2, ++key));

    cluster->get_blocks().put(b1);

    auto db_file = db::FileInfo();
    db_file.set_name("my-file.txt");
    db_file.set_sequence(++key);
    db_file.set_type(proto::FileInfoType::FILE);
    db_file.add_blocks_keys(b1->get_db_key());
    auto file = model::file_info_ptr_t(new file_info_t(db_file, folder_info.get()));
    folder_info->add(file);

    CHECK(cluster->get_blocks().size() == 1);

    proto::IndexUpdate iu;
    iu.set_folder(folder->id());
    auto fi = iu.add_files();

    SECTION("file has been deleted") {
        fi->set_name(std::string(file->get_name()));
        fi->set_type(proto::FileInfoType::FILE);
        fi->set_sequence(++key);
        fi->set_deleted(true);

        folder_info->update(iu, device);
        CHECK(folder_info->get_file_infos().size() == 1);
        CHECK(cluster->get_blocks().size() == 0);
        CHECK(cluster->get_deleted_blocks().size() == 1);
        CHECK(cluster->get_deleted_blocks().by_id(b1->get_hash()));
    }

    SECTION("file content has been changed") {
        fi->set_name(std::string(file->get_name()));
        fi->set_type(proto::FileInfoType::FILE);
        fi->set_sequence(++key);
        auto block = fi->add_blocks();
        block->set_hash(b2->get_hash());
        block->set_size(b2->get_size());

        folder_info->update(iu, device);
        CHECK(folder_info->get_file_infos().size() == 1);
        CHECK(cluster->get_blocks().size() == 1);
        CHECK(cluster->get_blocks().by_id(b2->get_hash()));
        CHECK(cluster->get_deleted_blocks().size() == 1);
        CHECK(cluster->get_deleted_blocks().by_id(b1->get_hash()));
    }
}
