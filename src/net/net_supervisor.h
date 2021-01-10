#pragma once

#include "../model/device.h"
#include "../model/cluster.h"
#include "../ui/messages.hpp"
#include "messages.h"
#include <boost/asio.hpp>
#include <rotor/asio.hpp>
#include <unordered_map>
#include <boost/outcome.hpp>

namespace syncspirit {
namespace net {

namespace outcome = boost::outcome_v2;

struct net_supervisor_config_t : ra::supervisor_config_asio_t {
    config::main_t app_config;
};

template <typename Supervisor>
struct net_supervisor_config_builder_t : ra::supervisor_config_asio_builder_t<Supervisor> {
    using builder_t = typename Supervisor::template config_builder_t<Supervisor>;
    using parent_t = ra::supervisor_config_asio_builder_t<Supervisor>;
    using parent_t::parent_t;

    builder_t &&app_config(const config::main_t &value) &&noexcept {
        parent_t::config.app_config = value;
        return std::move(*static_cast<typename parent_t::builder_t *>(this));
    }
};

struct net_supervisor_t : public ra::supervisor_asio_t {
    using parent_t = ra::supervisor_asio_t;
    using config_t = net_supervisor_config_t;

    template <typename Actor> using config_builder_t = net_supervisor_config_builder_t<Actor>;

    explicit net_supervisor_t(config_t &config);
    void shutdown_start() noexcept override;
    void configure(r::plugin::plugin_base_t &plugin) noexcept override;
    void on_child_shutdown(actor_base_t *actor, const std::error_code &ec) noexcept override;
    void on_start() noexcept override;

  private:
    using create_folder_req_ptr_t = r::intrusive_ptr_t<ui::message::create_folder_request_t>;
    using discovery_map_t = std::set<rotor::request_id_t>;
    using folder_requests_t = std::unordered_map<r::request_id_t, create_folder_req_ptr_t>;

    void on_ssdp(message::ssdp_notification_t &message) noexcept;
    void on_announce(message::announce_notification_t &message) noexcept;
    void on_port_mapping(message::port_mapping_notification_t &message) noexcept;
    void on_discovery(message::discovery_response_t &req) noexcept;
    void on_discovery_notify(message::discovery_notify_t &message) noexcept;
    void on_connect(message::connect_response_t &message) noexcept;
    void on_disconnect(message::disconnect_notify_t &message) noexcept;
    void on_connection(message::connection_notify_t &message) noexcept;
    void on_auth(message::auth_request_t &message) noexcept;
    void on_config_request(ui::message::config_request_t &message) noexcept;
    void on_config_save(ui::message::config_save_request_t &message) noexcept;
    void on_create_folder(ui::message::create_folder_request_t &message) noexcept;
    void on_make_index(message::make_index_id_response_t &message) noexcept;
    void on_load_folder(message::load_folder_response_t &message) noexcept;

    void discover(model::device_ptr_t &device) noexcept;
    void launch_children() noexcept;
    void launch_ssdp() noexcept;
    void persist_data() noexcept;
    void update_devices() noexcept;
    outcome::result<void> save_config(const config::main_t &new_cfg) noexcept;

    config::main_t app_config;
    r::address_ptr_t ssdp_addr;
    r::address_ptr_t peers_addr;
    r::address_ptr_t db_addr;
    r::address_ptr_t controller_addr;
    r::address_ptr_t global_discovery_addr;
    r::address_ptr_t local_discovery_addr;
    std::uint32_t ssdp_attempts = 0;
    model::device_ptr_t device;
    model::cluster_ptr_t cluster;
    utils::key_pair_t ssl_pair;
    discovery_map_t discovery_map;
    model::devices_map_t devices;
    folder_requests_t folder_requests;
};

} // namespace net
} // namespace syncspirit
