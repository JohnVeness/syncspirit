#include "controller_actor.h"
#include "names.h"
#include "../utils/error_code.h"
#include <spdlog/spdlog.h>

using namespace syncspirit::net;

// auto folder = cluster->opt_for_synch(device);

namespace {
namespace resource {
r::plugin::resource_id_t peer = 0;
}

} // namespace

controller_actor_t::controller_actor_t(config_t &config)
    : r::actor_base_t{config}, cluster{config.cluster}, device{config.device}, peer{config.peer},
      peer_addr{config.peer_addr}, sync_state{sync_state_t::none} {}

void controller_actor_t::configure(r::plugin::plugin_base_t &plugin) noexcept {
    r::actor_base_t::configure(plugin);
    plugin.with_casted<r::plugin::address_maker_plugin_t>([&](auto &p) {
        std::string id = "controller/";
        id += peer->device_id.get_short();
        p.set_identity(id, false);
    });
    plugin.with_casted<r::plugin::registry_plugin_t>(
        [&](auto &p) { p.discover_name(names::db, db, false).link(true); });
    plugin.with_casted<r::plugin::link_client_plugin_t>([&](auto &p) { p.link(peer_addr, false); });
    plugin.with_casted<r::plugin::starter_plugin_t>([&](auto &p) {
        p.subscribe_actor(&controller_actor_t::on_forward);
        p.subscribe_actor(&controller_actor_t::on_store_folder_info);
    });
}

void controller_actor_t::on_start() noexcept {
    r::actor_base_t::on_start();
    spdlog::trace("{}, on_start", identity);
    send<payload::start_reading_t>(peer_addr, get_address());
}

void controller_actor_t::shutdown_start() noexcept {
    /*
    if (sync_state != sync_state_t::none) {
        resources->acquire(resource::peer);
    }
    */
    send<payload::termination_t>(peer_addr, shutdown_reason);
    r::actor_base_t::shutdown_start();
}

bool controller_actor_t::on_unlink(const r::address_ptr_t &peer_addr) noexcept {
    auto it = peers_map.find(peer_addr);
    if (it != peers_map.end()) {
        auto &device = it->second;
        spdlog::debug("{}, on_unlink with {}", identity, device->device_id);
        peers_map.erase(it);
        if (peers_map.empty()) {
            sync_state = sync_state_t::none;
        }
        resources->release(resource::peer);
        return false;
    }
    return r::actor_base_t::on_unlink(peer_addr);
}

void controller_actor_t::on_forward(message::forwarded_message_t &message) noexcept {
    std::visit([this](auto &msg) { on_message(msg); }, message.payload);
}

void controller_actor_t::on_store_folder_info(message::store_folder_info_response_t &message) noexcept {
    auto &ee = message.payload.ee;
    auto &fi = message.payload.req->payload.request_payload.folder_info;
    auto &label = fi->get_folder()->label();
    auto &device_id = fi->get_device()->device_id;
    if (ee) {
        spdlog::warn("{}, on_store_folder_info {} / {} failed : {}", identity, label, device_id, ee->message());
        return do_shutdown(ee);
    }
    spdlog::trace("{}, on_store_folder_info {} / {} ", identity, label, device_id);
}

void controller_actor_t::on_message(proto::message::Index &message) noexcept {
    auto &folder_id = message->folder();
    auto folder = cluster->get_folders().by_id(folder_id);
    if (!folder) {
        spdlog::warn("{}, unknown folder {}", identity, folder_id);
        auto ec = utils::make_error_code(utils::protocol_error_code_t::unknown_folder);
        std::string context = fmt::format("folder '{}'", folder_id);
        auto ee = r::make_error(context, ec);
        return do_shutdown(ee);
    }
    auto folder_info = folder->update(*message, peer);
    if (folder_info) {
        auto timeout = init_timeout / 2;
        request<payload::store_folder_info_request_t>(db, std::move(folder_info)).send(timeout);
    }
}

void controller_actor_t::on_message(proto::message::IndexUpdate &message) noexcept { std::abort(); }

void controller_actor_t::on_message(proto::message::Request &message) noexcept { std::abort(); }

void controller_actor_t::on_message(proto::message::Response &message) noexcept { std::abort(); }

void controller_actor_t::on_message(proto::message::DownloadProgress &message) noexcept { std::abort(); }
