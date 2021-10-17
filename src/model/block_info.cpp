#include "block_info.h"
#include "file_info.h"
#include "structs.pb.h"
#include "../db/prefix.h"
#include <spdlog.h>

namespace syncspirit::model {

static const constexpr char prefix = (char)(db::prefix::block_info);

block_info_t::block_info_t(std::string_view key, std::string_view data) noexcept {
    assert(key.length() == data_length);
    assert(key[0] == prefix);
    std::copy(key.begin(), key.end(), hash);

    db::BlockInfo block;
    auto ok = block.ParseFromArray(data.data(), data.size());
    assert(ok);
    (void)ok;
    weak_hash = block.weak_hash();
    size = block.size();
}

block_info_t::block_info_t(const proto::BlockInfo &block) noexcept : weak_hash{block.weak_hash()}, size{block.size()} {
    auto &h = block.hash();
    assert(h.length() <= digest_length);
    std::copy(h.begin(), h.end(), hash + 1);
    hash[0] = prefix;
    auto left = digest_length - h.length();
    if (left) {
        std::fill_n(hash + 1 + h.length(), left, 0);
    }

    mark_dirty();
}

std::string block_info_t::serialize() noexcept {
    db::BlockInfo r;
    r.set_weak_hash(weak_hash);
    r.set_size(size);
    return r.SerializeAsString();
}

void block_info_t::link(file_info_t *file_info, size_t block_index) noexcept {
    file_blocks.emplace_back(this, file_info, block_index);
}

auto block_info_t::unlink(file_info_t *file_info, bool deletion) noexcept -> removed_incides_t {
    removed_incides_t r;
    for (auto it = file_blocks.begin(); it != file_blocks.end();) {
        auto &fb = *it;
        if (fb.matches(this, file_info)) {
            r.push_back(fb.block_index());
            it = file_blocks.erase(it);
        } else {
            ++it;
        }
    }
    assert(!r.empty() && "at least one block has been removed");
    if (deletion && file_blocks.empty()) {
        mark_deleted();
    }
    return r;
}

void block_info_t::mark_local_available(file_info_t *file_info) noexcept {
    auto predicate = [&](file_block_t &block) { return block.matches(this, file_info); };
    auto it = std::find_if(file_blocks.begin(), file_blocks.end(), predicate);
    assert(it != file_blocks.end());
    it->mark_locally_available();
}

file_block_t block_info_t::local_file() noexcept {
    for (auto &b : file_blocks) {
        if (b.is_locally_available()) {
            return b;
        }
    }
    return {};
}

#if 0
static void del_me() {
    block_infos_map_t map;
    auto block = block_info_ptr_t(new block_info_t("a", "b"));
    map.put(block);
    map.get("a");
}
#endif


template<> std::string_view get_index<0>(const block_info_ptr_t& item) noexcept { return item->get_key(); }

} // namespace syncspirit::model


