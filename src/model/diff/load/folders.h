#pragma once

#include "common.h"

namespace syncspirit::model::diff::load {

struct folders_t final : cluster_diff_t {
    template <typename T> folders_t(T &&folders_) noexcept : folders{std::forward<T>(folders_)} {}

    outcome::result<void> apply_impl(cluster_t &) const noexcept override;

    container_t folders;
};

} // namespace syncspirit::model::diff::load