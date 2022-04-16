// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2019-2022 Ivan Baidakou

#pragma once

#include "generic_diff.hpp"
#include "syncspirit-export.h"

namespace syncspirit::model::diff {

namespace modify {
struct append_block_t;
struct blocks_availability_t;
struct clone_block_t;
} // namespace modify

template <> struct SYNCSPIRIT_API generic_visitor_t<tag::block> {
    virtual ~generic_visitor_t() = default;

    virtual outcome::result<void> operator()(const modify::append_block_t &) noexcept;
    virtual outcome::result<void> operator()(const modify::blocks_availability_t &) noexcept;
    virtual outcome::result<void> operator()(const modify::clone_block_t &) noexcept;
};

using block_visitor_t = generic_visitor_t<tag::block>;

} // namespace syncspirit::model::diff
