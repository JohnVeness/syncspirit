#pragma once

#include "../config/main.h"
#include "../utils/log.h"
#include "../hasher/messages.h"
#include "messages.h"
#include "continuation.h"
#include <rotor.hpp>
#include <deque>

namespace syncspirit {
namespace fs {

struct scan_actor_config_t : r::actor_config_t {
    config::fs_config_t fs_config;
    r::address_ptr_t hasher_proxy;
    uint32_t requested_hashes_limit;
};

template <typename Actor> struct scan_actor_config_builder_t : r::actor_config_builder_t<Actor> {
    using builder_t = typename Actor::template config_builder_t<Actor>;
    using parent_t = r::actor_config_builder_t<Actor>;
    using parent_t::parent_t;

    builder_t &&fs_config(const config::fs_config_t &value) &&noexcept {
        parent_t::config.fs_config = value;
        return std::move(*static_cast<typename parent_t::builder_t *>(this));
    }

    builder_t &&hasher_proxy(r::address_ptr_t &value) &&noexcept {
        parent_t::config.hasher_proxy = value;
        return std::move(*static_cast<typename parent_t::builder_t *>(this));
    }

    builder_t &&requested_hashes_limit(uint32_t value) &&noexcept {
        parent_t::config.requested_hashes_limit = value;
        return std::move(*static_cast<typename parent_t::builder_t *>(this));
    }
};

struct scan_actor_t : public r::actor_base_t {
    using config_t = scan_actor_config_t;
    template <typename Actor> using config_builder_t = scan_actor_config_builder_t<Actor>;

    explicit scan_actor_t(config_t &cfg);

    void on_start() noexcept override;
    void shutdown_finish() noexcept override;
    void configure(r::plugin::plugin_base_t &plugin) noexcept override;

  private:
    using requests_t = std::deque<request_ptr_t>;
    using error_ptr_t = r::extended_error_ptr_t;

    void on_scan_request(message::scan_request_t &req) noexcept;
    void on_scan_cancel(message::scan_cancel_t &req) noexcept;

    void on_scan(message::scan_t &req) noexcept;
    void on_process(message::process_signal_t &) noexcept;
    void on_hash(hasher::message::digest_response_t &res) noexcept;

    void scan_dir(bfs::path &dir, payload::scan_t &payload) noexcept;
    void process_queue() noexcept;
    void reply(message::scan_t &req) noexcept;

    void calc_blocks(message::scan_t &req) noexcept;

    utils::logger_t log;
    requests_t queue;
    bool scan_cancelled = false;
    r::address_ptr_t hasher_proxy;
    config::fs_config_t fs_config;
    uint32_t requested_hashes_limit;
    uint32_t requested_hashes = 0;
};

} // namespace fs
} // namespace syncspirit
