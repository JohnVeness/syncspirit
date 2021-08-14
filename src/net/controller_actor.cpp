#include "controller_actor.h"
#include "names.h"
#include "../utils/error_code.h"
#include "../ui/messages.hpp"
#include <spdlog/spdlog.h>
#include <fstream>

using namespace syncspirit;
using namespace syncspirit::net;
namespace bfs = boost::filesystem;

template <typename Message> struct typed_folder_updater_t final : controller_actor_t::folder_updater_t {
    Message msg;

    typed_folder_updater_t(model::device_ptr_t &peer_, Message &&message_) {
        peer = peer_;
        msg = std::move(message_);
    }
    const std::string &id() noexcept override { return (*msg).folder(); }

    void update(model::folder_t &folder) noexcept override { return folder.update(*msg, peer); };
};

namespace {
namespace resource {
r::plugin::resource_id_t peer = 0;
}
} // namespace

controller_actor_t::controller_actor_t(config_t &config)
    : r::actor_base_t{config}, cluster{config.cluster}, device{config.device}, peer{config.peer},
      peer_addr{config.peer_addr}, request_timeout{config.request_timeout}, peer_cluster_config{std::move(
                                                                                config.peer_cluster_config)},
      ignored_folders{config.ignored_folders}, sync_state{sync_state_t::none} {
    log = utils::get_logger("net.controller_actor");
}

void controller_actor_t::configure(r::plugin::plugin_base_t &plugin) noexcept {
    r::actor_base_t::configure(plugin);
    plugin.with_casted<r::plugin::address_maker_plugin_t>([&](auto &p) {
        std::string id = "controller/";
        id += peer->device_id.get_short();
        p.set_identity(id, false);
    });
    plugin.with_casted<r::plugin::registry_plugin_t>([&](auto &p) {
        p.discover_name(names::db, db, false).link(true);
        p.discover_name(names::fs, fs, false).link(true);
    });
    plugin.with_casted<r::plugin::link_client_plugin_t>([&](auto &p) { p.link(peer_addr, false); });
    plugin.with_casted<r::plugin::starter_plugin_t>([&](auto &p) {
        p.subscribe_actor(&controller_actor_t::on_forward);
        p.subscribe_actor(&controller_actor_t::on_store_folder);
        p.subscribe_actor(&controller_actor_t::on_store_folder_info);
        p.subscribe_actor(&controller_actor_t::on_new_folder);
        p.subscribe_actor(&controller_actor_t::on_ready);
        p.subscribe_actor(&controller_actor_t::on_block);
        p.subscribe_actor(&controller_actor_t::on_write);
    });
}

void controller_actor_t::on_start() noexcept {
    r::actor_base_t::on_start();
    log->trace("{}, on_start", identity);
    send<payload::start_reading_t>(peer_addr, get_address());
    update(*peer_cluster_config);
    peer_cluster_config.reset();
    ready();
    log->info("{} is ready/online", identity);
}

void controller_actor_t::update(proto::ClusterConfig &config) noexcept {
    log->trace("{}, update", identity);
    auto unknown_folders = cluster->update(config);
    for (auto &folder : unknown_folders) {
        if (!ignored_folders->by_key(folder.id())) {
            for (int i = 0; i < folder.devices_size(); ++i) {
                auto &d = folder.devices(i);
                if (d.id() == peer->get_id()) {
                    auto &dest = supervisor->get_address();
                    send<ui::payload::new_folder_notify_t>(dest, folder, peer, d.index_id());
                }
            }
        }
    }
    file_iterator.reset();
    block_iterator.reset();
}

void controller_actor_t::ready() noexcept {
    if (!(substate & READY)) {
        send<payload::ready_signal_t>(get_address());
        substate = substate | READY;
    }
}

void controller_actor_t::shutdown_start() noexcept {
    send<payload::termination_t>(peer_addr, shutdown_reason);
    r::actor_base_t::shutdown_start();
}

controller_actor_t::ImmediateResult controller_actor_t::process_immediately() noexcept {
    assert(current_file);
    auto path = current_file->get_path();
    auto parent = path.parent_path();
    sys::error_code ec;
    if (current_file->is_deleted()) {
        if (bfs::exists(path, ec)) {
            log->debug("{} removing {}", identity, path.string());
            auto ok = bfs::remove_all(path);
            if (!ok) {
                log->warn("{}, error removing {} : {}", identity, path.string(), ec.message());
                do_shutdown(make_error(ec));
                return ImmediateResult::ERROR;
            }
        }
        log->trace("{}, {} already abscent, noop", identity, path.string());
        current_file->mark_sync();
        return ImmediateResult::DONE;
    } else if (current_file->is_file() && current_file->get_size() == 0) {
        log->trace("{}, creating empty file {}", identity, path.string());
        if (!bfs::exists(parent)) {
            bfs::create_directories(parent, ec);
            if (ec) {
                log->warn("{}, error creating path {} : {}", identity, parent.string(), ec.message());
                do_shutdown(make_error(ec));
                return ImmediateResult::ERROR;
            }
        }
        std::ofstream out;
        out.exceptions(out.failbit | out.badbit);
        try {
            out.open(path.string());
        } catch (const std::ios_base::failure &e) {
            do_shutdown(make_error(e.code()));
            log->warn("{}, error creating {} : {}", identity, path.string(), e.code().message());
            return ImmediateResult::ERROR;
        }
        current_file->mark_sync();
        return ImmediateResult::DONE;
    } else if (current_file->is_dir()) {
        log->trace("{}, creating dir {}", identity, path.string());
        if (!bfs::exists(path)) {
            bfs::create_directories(path, ec);
            if (ec) {
                log->warn("{}, error creating path {} : {}", identity, parent.string(), ec.message());
                do_shutdown(make_error(ec));
                return ImmediateResult::ERROR;
            }
        }
        current_file->mark_sync();
        return ImmediateResult::DONE;
    } else if (current_file->is_link()) {
        auto target = bfs::path(current_file->get_link_target());
        log->trace("{}, creating symlink {} -> {}", identity, path.string(), target.string());
        if (!bfs::exists(parent)) {
            bfs::create_directories(parent, ec);
            if (ec) {
                log->warn("{}, error creating parent path {} : {}", identity, parent.string(), ec.message());
                do_shutdown(make_error(ec));
                return ImmediateResult::ERROR;
            }
        }
        bfs::create_symlink(target, path, ec);
        if (ec) {
            log->warn("{}, error symlinking {} -> {} {} : {}", identity, path.string(), target.string(), ec.message());
            do_shutdown(make_error(ec));
            return ImmediateResult::ERROR;
        }
        current_file->mark_sync();
        return ImmediateResult::DONE;
        current_file->mark_sync();
        return ImmediateResult::DONE;
    }
    return ImmediateResult::NON_IMMEDIATE;
}

void controller_actor_t::on_ready(message::ready_signal_t &message) noexcept {
    log->trace("{}, on_ready", identity);
    substate = substate & ~READY;
    if ((substate & BLOCK) || (state != r::state_t::OPERATIONAL)) {
        return;
    }

    if (!file_iterator && !block_iterator) {
        file_iterator = cluster->iterate_files(peer);
        if (!file_iterator) {
            log->trace("{}, nothing more to sync", identity);
            return;
        }
    }

    if (block_iterator) {
        assert(current_file);
        auto cluster_block = block_iterator.next();
        auto existing_block = cluster_block.block->local_file();
        if (existing_block) {
            log->trace("{}, cloning block {} from {} to {} as block {}", identity, existing_block.file_info->get_name(),
                       existing_block.block_index, current_file->get_name(), cluster_block.block_index);
            current_file->clone_block(*existing_block.file_info, existing_block.block_index, cluster_block.block_index);
            ready();
        } else {
            request_block(cluster_block);
        }
        if (!block_iterator) {
            log->trace("{}, there are no more blocks for {}", identity, current_file->get_full_name());
            current_file.reset();
        }
        return;
    }

    current_file = file_iterator.next();
    auto ir = process_immediately();
    if (ir == ImmediateResult::ERROR) {
        return;
    }
    if (ir == ImmediateResult::NON_IMMEDIATE) {
        log->trace("{}, going to sync {}", identity, current_file->get_full_name());
        block_iterator = current_file->iterate_blocks();
    }
    ready();
}

void controller_actor_t::request_block(const model::block_location_t &block) noexcept {
    log->trace("{} request_block, file = {}, block index = {}, sz = {}", identity, current_file->get_full_name(),
               block.block_index, block.block->get_size());
    request<payload::block_request_t>(peer_addr, current_file, model::block_info_ptr_t{block.block}, block.block_index)
        .send(request_timeout);
    substate = substate | BLOCK;
}

bool controller_actor_t::on_unlink(const r::address_ptr_t &peer_addr) noexcept {
    auto it = peers_map.find(peer_addr);
    if (it != peers_map.end()) {
        auto &device = it->second;
        log->debug("{}, on_unlink with {}", identity, device->device_id);
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

void controller_actor_t::on_store_folder(message::store_folder_response_t &message) noexcept {
    auto &ee = message.payload.ee;
    auto &folder = message.payload.req->payload.request_payload.folder;
    auto &label = folder->label();
    if (ee) {
        log->warn("{}, on_store_folder {} failed : {}", identity, label, ee->message());
        return do_shutdown(ee);
    }
    log->trace("{}, on_store_folder_info, folder = '{}'", identity, label);
    ready();
}

void controller_actor_t::on_new_folder(message::store_new_folder_notify_t &message) noexcept {
    auto &folder = message.payload.folder;
    log->trace("{}, on_new_folder, folder = '{}'", identity, folder->label());
    auto cluster_update = cluster->get(peer);
    using payload_t = std::decay_t<decltype(cluster_update)>;
    auto update = std::make_unique<payload_t>(std::move(cluster_update));
    send<payload::cluster_config_t>(peer_addr, std::move(update));
}

void controller_actor_t::on_store_folder_info(message::store_folder_info_response_t &message) noexcept {
    auto &ee = message.payload.ee;
    auto &fi = message.payload.req->payload.request_payload.folder_info;
    auto label = fi->get_folder()->label();
    log->trace("{}, on_store_folder_info (max seq = {}) {}/{}", identity, fi->get_max_sequence(), label,
               fi->get_db_key());
    if (ee) {
        log->warn("{}, on_store_folder_info {} failed : {}", identity, label, ee->message());
        return do_shutdown(ee);
    }
}

void controller_actor_t::on_message(proto::message::ClusterConfig &message) noexcept { update(*message); }

void controller_actor_t::on_message(proto::message::Index &message) noexcept {
    update(typed_folder_updater_t(peer, std::move(message)));
}

void controller_actor_t::on_message(proto::message::IndexUpdate &message) noexcept {
    update(typed_folder_updater_t(peer, std::move(message)));
}

void controller_actor_t::on_message(proto::message::Request &message) noexcept { std::abort(); }

void controller_actor_t::on_message(proto::message::DownloadProgress &message) noexcept { std::abort(); }

void controller_actor_t::update(folder_updater_t &&updater) noexcept {
    auto &folder_id = updater.id();
    auto folder = cluster->get_folders().by_id(folder_id);
    if (current_file && current_file->get_folder()->id() == folder_id) {
        log->trace("{}, resetting iterators on folder {}", identity, folder->label());
        file_iterator.reset();
        block_iterator.reset();
    }
    if (!folder) {
        log->warn("{}, unknown folder {}", identity, folder_id);
        auto ec = utils::make_error_code(utils::protocol_error_code_t::unknown_folder);
        std::string context = fmt::format("folder '{}'", folder_id);
        auto ee = r::make_error(context, ec);
        return do_shutdown(ee);
    }
    updater.update(*folder);
    auto updated = folder->is_dirty();
    log->debug("{}, folder {}/{} has been updated = {}", identity, folder_id, folder->label(), updated);
    if (updated) {
        auto timeout = init_timeout / 2;
        request<payload::store_folder_request_t>(db, std::move(folder)).send(timeout);
    }
}

void controller_actor_t::on_block(message::block_response_t &message) noexcept {
    using request_t = fs::payload::write_request_t;
    substate = substate & ~BLOCK;
    auto ee = message.payload.ee;
    if (ee) {
        log->warn("{}, can't receive block : {}", identity, ee->message());
        return do_shutdown(ee);
    }
    auto &payload = message.payload.req->payload.request_payload;
    auto &file = payload.file;
    auto &data = message.payload.res.data;
    auto block_index = payload.block_index;

#if 0
    static const constexpr size_t SZ = SHA256_DIGEST_LENGTH;
    char digest_buff[SZ];
    utils::digest(data.data(), data.size(), digest_buff);
    std::string_view digest(digest_buff, SZ);
    if (digest != message.payload.req->payload.request_payload.block->get_hash()) {
        auto ec = utils::make_error_code(utils::protocol_error_code_t::digest_mismatch);
        auto name = file->get_full_name();
        std::string context = fmt::format("block {} from {}", block_index, name);
        auto ee = r::make_error(context, ec);
        log->warn("{}, on block, digest mismatch: {}", identity, name, context);
        return do_shutdown(ee);
    }
#endif

    bool final = file->get_blocks().size() == block_index + 1;
    auto path = file->get_path();
    // request another block while the current is going to be flushed to disk
    auto request_id = request<request_t>(fs, path, std::move(data), final).send(init_timeout);
    file->mark_local_available(payload.block_index);
    ready();
    responses_map.emplace(request_id, &message);
}

void controller_actor_t::on_write(fs::message::write_response_t &message) noexcept {
    auto &ee = message.payload.ee;
    if (ee) {
        log->warn("{}, on_write failed : {}", identity, ee->message());
        return do_shutdown(ee);
    }
    auto it = responses_map.find(message.payload.request_id());
    assert(it != responses_map.end());
    auto block_res = it->second;
    responses_map.erase(it);
    auto &p = block_res->payload.req->payload.request_payload;
    auto &file = p.file;
    if (file->get_status() == model::file_status_t::sync) {
        auto folder = file->get_folder();
        auto fi = folder->get_folder_info(device);
        auto seq = fi->get_max_sequence();
        auto new_seq = file->get_sequence();
        if (new_seq > seq) {
            log->trace("{}, updated max sequence '{}' on local device: {} -> {}", identity, folder->label(), seq,
                       new_seq);
            fi->update_max_sequence(new_seq);
        }
        request<payload::store_folder_info_request_t>(db, fi).send(init_timeout);
    }
    ready();
}
