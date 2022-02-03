#pragma once

#include "../cluster_diff.h"

namespace syncspirit::model::diff::modify {

struct share_folder_t final : cluster_diff_t {

    share_folder_t(std::string_view peer_device_, std::string_view folder_id_, uint64_t index_ = 0) noexcept
        : peer_id{peer_device_}, folder_id{folder_id_}, index{index_} {}

    outcome::result<void> apply_impl(cluster_t &) const noexcept override;
    outcome::result<void> visit(cluster_visitor_t &) const noexcept override;

    std::string peer_id;
    std::string folder_id;
    uint64_t index;
};

} // namespace syncspirit::model::diff::modify