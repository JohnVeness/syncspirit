#pragma once
#include "messages.h"

#include <deque>
#include <optional>
#include <boost/outcome.hpp>
#include <boost/iostreams/device/mapped_file.hpp>
#include <boost/iostreams/stream.hpp>
#include <memory>
#include <fstream>

namespace syncspirit {
namespace fs {

namespace bio = boost::iostreams;

namespace payload {

struct scan_t {
    using file_map_t = payload::scan_response_t::file_map_t;
    using file_t = bio::mapped_file_source;
    using file_ptr_t = std::unique_ptr<file_t>;

    struct next_block_t {
        bfs::path path;
        std::size_t block_size;
        std::size_t file_size;
        std::size_t block_index;
        file_ptr_t file;
    };

    using next_block_option_t = std::optional<next_block_t>;

    bfs::path root;
    r::address_ptr_t reply_to;
    model::block_infos_map_t blocks_map;
    std::deque<bfs::path> scan_dirs;
    std::deque<bfs::path> files_queue;
    file_map_t file_map;
    std::optional<next_block_t> next_block;
};

} // namespace payload

namespace message {

using scan_t = r::message_t<payload::scan_t>;

}

} // namespace fs
} // namespace syncspirit
