#pragma once

#include "../configuration.h"
#include "messages.h"
#include <boost/asio.hpp>

namespace syncspirit {
namespace net {

struct global_discovery_actor_config_t : r::actor_config_t {
    tcp::endpoint endpoint;
    utils::URI announce_url;
    std::string cert_file;
    std::string key_file;
    std::uint32_t rx_buff_size;
    std::uint32_t io_timeout;
    std::uint32_t reannounce_after;
};

template <typename Actor> struct global_discovery_actor_config_builder_t : r::actor_config_builder_t<Actor> {
    using builder_t = typename Actor::template config_builder_t<Actor>;
    using parent_t = r::actor_config_builder_t<Actor>;
    using parent_t::parent_t;

    builder_t &&endpoint(const tcp::endpoint &value) &&noexcept {
        parent_t::config.endpoint = value;
        return std::move(*static_cast<typename parent_t::builder_t *>(this));
    }

    builder_t &&announce_url(const utils::URI &value) &&noexcept {
        parent_t::config.announce_url = value;
        return std::move(*static_cast<typename parent_t::builder_t *>(this));
    }

    builder_t &&cert_file(const std::string &value) &&noexcept {
        parent_t::config.cert_file = value;
        return std::move(*static_cast<typename parent_t::builder_t *>(this));
    }

    builder_t &&key_file(const std::string &value) &&noexcept {
        parent_t::config.key_file = value;
        return std::move(*static_cast<typename parent_t::builder_t *>(this));
    }

    builder_t &&rx_buff_size(const std::uint32_t value) &&noexcept {
        parent_t::config.rx_buff_size = value;
        return std::move(*static_cast<typename parent_t::builder_t *>(this));
    }

    builder_t &&io_timeout(const std::uint32_t value) &&noexcept {
        parent_t::config.io_timeout = value;
        return std::move(*static_cast<typename parent_t::builder_t *>(this));
    }

    builder_t &&reannounce_after(const std::uint32_t value) &&noexcept {
        parent_t::config.io_reannounce_after = value;
        return std::move(*static_cast<typename parent_t::builder_t *>(this));
    }
};

struct global_discovery_actor_t : public r::actor_base_t {
    using config_t = global_discovery_actor_config_t;
    template <typename Actor> using config_builder_t = global_discovery_actor_config_builder_t<Actor>;

    explicit global_discovery_actor_t(config_t &cfg);

    void on_start() noexcept override;
    // void shutdown_start() noexcept override;
    void configure(r::plugin::plugin_base_t &plugin) noexcept override;
    void on_announce(message::http_response_t &message) noexcept;

  private:
    using rx_buff_t = payload::http_request_t::rx_buff_ptr_t;

    r::address_ptr_t http_client;
    tcp::endpoint endpoint;
    utils::URI announce_url;
    ssl_context_ptr_t ssl_context;
    rx_buff_t rx_buff;
    std::uint32_t rx_buff_size;
    std::uint32_t io_timeout;
    std::uint32_t reannounce_after;
};

} // namespace net
} // namespace syncspirit
