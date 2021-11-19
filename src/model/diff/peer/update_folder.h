#pragma once

#include <memory>
#include "../cluster_diff.h"
#include "model/cluster.h"
#include "bep.pb.h"

namespace syncspirit::model::diff::peer {

struct update_folder_t final : cluster_diff_t {
    using files_t = std::vector<proto::FileInfo>;
    using blocks_t = std::vector<proto::BlockInfo>;

    static outcome::result<cluster_diff_ptr_t> create(const cluster_t &cluster, const model::device_t& source, const proto::Index& message) noexcept;
    static outcome::result<cluster_diff_ptr_t> create(const cluster_t &cluster, const model::device_t& source, const proto::IndexUpdate& message) noexcept;

    outcome::result<void> apply_impl(cluster_t &) const noexcept override;
    outcome::result<void> visit(diff_visitor_t &) const noexcept override;


    update_folder_t(std::string_view folder_id, std::string_view peer_id, files_t files, blocks_t blocks) noexcept;
    std::string folder_id;
    std::string peer_id;
    files_t files;
    blocks_t blocks;
};

}
