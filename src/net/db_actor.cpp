// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2019-2024 Ivan Baidakou

#include "db_actor.h"
#include "names.h"
#include "utils/platform.h"
#include "db/prefix.h"
#include "db/utils.h"
#include "db/error_code.h"
#include "model/diff/load/blocks.h"
#include "model/diff/load/close_transaction.h"
#include "model/diff/load/devices.h"
#include "model/diff/load/file_infos.h"
#include "model/diff/load/folder_infos.h"
#include "model/diff/load/folders.h"
#include "model/diff/load/ignored_devices.h"
#include "model/diff/load/ignored_folders.h"
#include "model/diff/load/load_cluster.h"
#include "model/diff/load/unknown_folders.h"
#include "model/diff/modify/create_folder.h"
#include "model/diff/modify/clone_file.h"
#include "model/diff/modify/finish_file_ack.h"
#include "model/diff/modify/local_update.h"
#include "model/diff/modify/share_folder.h"
#include "model/diff/modify/unshare_folder.h"
#include "model/diff/modify/update_peer.h"
#include "model/diff/peer/cluster_remove.h"
#include "model/diff/peer/update_folder.h"
#include "model/diff/cluster_visitor.h"
#include <string_view>

#ifdef WIN32_LEAN_AND_MEAN
#include <malloc.h>
#else
#include <alloca.h>
#endif

namespace syncspirit::net {

namespace {
namespace resource {
r::plugin::resource_id_t db = 0;
}
} // namespace

db_actor_t::db_actor_t(config_t &config)
    : r::actor_base_t{config}, env{nullptr}, db_dir{config.db_dir}, db_config{config.db_config},
      cluster{config.cluster} {
    log = utils::get_logger("net.db");
    auto r = mdbx_env_create(&env);
    if (r != MDBX_SUCCESS) {
        LOG_CRITICAL(log, "{}, mbdx environment creation error ({}): {}", log->name(), r, mdbx_strerror(r));
        throw std::runtime_error(std::string(mdbx_strerror(r)));
    }
}

db_actor_t::~db_actor_t() {
    if (env) {
        mdbx_env_close(env);
    }
}

void db_actor_t::configure(r::plugin::plugin_base_t &plugin) noexcept {
    r::actor_base_t::configure(plugin);
    plugin.with_casted<r::plugin::address_maker_plugin_t>([&](auto &p) { p.set_identity("net::db", false); });
    plugin.with_casted<r::plugin::registry_plugin_t>([&](auto &p) {
        p.discover_name(names::coordinator, coordinator, false).link(false).callback([&](auto phase, auto &ee) {
            if (!ee && phase == r::plugin::registry_plugin_t::phase_t::linking) {
                auto p = get_plugin(r::plugin::starter_plugin_t::class_identity);
                auto plugin = static_cast<r::plugin::starter_plugin_t *>(p);
                plugin->subscribe_actor(&db_actor_t::on_model_update, coordinator);
            }
        });
    });
    plugin.with_casted<r::plugin::starter_plugin_t>([&](auto &p) {
        open();
        p.subscribe_actor(&db_actor_t::on_cluster_load);
    });
}

void db_actor_t::open() noexcept {
    resources->acquire(resource::db);
    auto &my_device = cluster->get_device();
    auto upper_limit = db_config.upper_limit;
    /* enable automatic size management */
    LOG_INFO(log, "{}, open, db upper limit = {}", identity, upper_limit);
    auto r = mdbx_env_set_geometry(env, -1, -1, upper_limit, -1, -1, -1);
    if (r != MDBX_SUCCESS) {
        LOG_ERROR(log, "{}, open, mbdx set geometry error ({}): {}", identity, r, mdbx_strerror(r));
        resources->release(resource::db);
        return do_shutdown(make_error(db::make_error_code(r)));
    }

    auto flags = MDBX_WRITEMAP | MDBX_COALESCE | MDBX_LIFORECLAIM | MDBX_EXCLUSIVE | MDBX_NOTLS | MDBX_SAFE_NOSYNC;
    r = mdbx_env_open(env, db_dir.c_str(), flags, 0664);
    if (r != MDBX_SUCCESS) {
        LOG_ERROR(log, "{}, open, mbdx open environment error ({}): {}", identity, r, mdbx_strerror(r));
        resources->release(resource::db);
        return do_shutdown(make_error(db::make_error_code(r)));
    }
    auto txn = db::make_transaction(db::transaction_type_t::RO, env);
    if (!txn) {
        LOG_ERROR(log, "{}, open, cannot create transaction {}", identity, txn.error().message());
        resources->release(resource::db);
        return do_shutdown(make_error(db::make_error_code(r)));
    }

    auto db_ver = db::get_version(txn.value());
    if (!db_ver) {
        LOG_ERROR(log, "{}, open, cannot get db version :: {}", identity, db_ver.error().message());
        resources->release(resource::db);
        return do_shutdown(make_error(db::make_error_code(r)));
    }
    auto version = db_ver.value();
    LOG_DEBUG(log, "got db version: {}, expected : {} ", version, db::version);

    if (!txn) {
        LOG_ERROR(log, "{}, open, cannot create transaction {}", identity, txn.error().message());
        resources->release(resource::db);
        return do_shutdown(make_error(db::make_error_code(r)));
    }
    if (db_ver.value() != db::version) {
        txn = db::make_transaction(db::transaction_type_t::RW, txn.value());
        auto r = db::migrate(version, my_device, txn.value());
        if (!r) {
            LOG_ERROR(log, "{}, open, cannot migrate db {}", identity, r.error().message());
            resources->release(resource::db);
            return do_shutdown(make_error(r.error()));
        }
        LOG_INFO(log, "{}, open, successfully migrated db: {} -> {} ", identity, version, db::version);
    }
    resources->release(resource::db);
}

auto db_actor_t::get_txn() noexcept -> outcome::result<db::transaction_t *> {
    if (!txn_holder) {
        auto txn = db::make_transaction(db::transaction_type_t::RW, env);
        if (!txn) {
            return txn.assume_error();
        }
        txn_holder.reset(new db::transaction_t(std::move(txn.assume_value())));
        uncommitted = 0;
    }
    return txn_holder.get();
}

auto db_actor_t::commit(bool force) noexcept -> outcome::result<void> {
    assert(txn_holder);
    if (force) {
        LOG_INFO(log, "{}, committing tx", identity);
        auto r = txn_holder->commit();
        txn_holder.reset();
        return r;
    }
    if (++uncommitted >= db_config.uncommitted_threshold) {
        LOG_INFO(log, "{}, committing tx", identity);
        auto r = txn_holder->commit();
        txn_holder.reset();
        return r;
    }
    return outcome::success();
}

void db_actor_t::on_start() noexcept {
    r::actor_base_t::on_start();
    LOG_TRACE(log, "{}, on_start", identity);
}

void db_actor_t::shutdown_finish() noexcept {
    if (txn_holder) {
        auto r = commit(true);
        if (!r) {
            auto &err = r.assume_error();
            LOG_ERROR(log, "{}, cannot commit tx: {}", identity, err.message());
        }
        txn_holder.reset();
    }
    auto r = mdbx_env_close(env);
    if (r != MDBX_SUCCESS) {
        LOG_ERROR(log, "{}, open, mbdx close error ({}): {}", identity, r, mdbx_strerror(r));
    }
    env = nullptr;
    r::actor_base_t::shutdown_finish();
}

void db_actor_t::on_cluster_load(message::load_cluster_request_t &request) noexcept {
    LOG_TRACE(log, "{}, on_cluster_load", identity);
    using namespace model::diff;
    using container_t = aggregate_t::diffs_t;

    auto txn_opt = db::make_transaction(db::transaction_type_t::RO, env);
    if (!txn_opt) {
        return reply_with_error(request, make_error(txn_opt.error()));
    }
    auto &txn = txn_opt.value();

    auto devices_opt = db::load(db::prefix::device, txn);
    if (!devices_opt) {
        return reply_with_error(request, make_error(devices_opt.error()));
    }

    auto blocks_opt = db::load(db::prefix::block_info, txn);
    if (!blocks_opt) {
        return reply_with_error(request, make_error(blocks_opt.error()));
    }

    auto ignored_devices_opt = db::load(db::prefix::ignored_device, txn);
    if (!ignored_devices_opt) {
        return reply_with_error(request, make_error(ignored_devices_opt.error()));
    }

    auto ignored_folders_opt = db::load(db::prefix::ignored_folder, txn);
    if (!ignored_folders_opt) {
        return reply_with_error(request, make_error(ignored_folders_opt.error()));
    }

    auto folders_opt = db::load(db::prefix::folder, txn);
    if (!folders_opt) {
        return reply_with_error(request, make_error(folders_opt.error()));
    }

    auto folder_infos_opt = db::load(db::prefix::folder_info, txn);
    if (!folder_infos_opt) {
        return reply_with_error(request, make_error(folder_infos_opt.error()));
    }

    auto file_infos_opt = db::load(db::prefix::file_info, txn);
    if (!file_infos_opt) {
        return reply_with_error(request, make_error(file_infos_opt.error()));
    }

    auto unknown_folders_opt = db::load(db::prefix::unknown_folder, txn);
    if (!unknown_folders_opt) {
        return reply_with_error(request, make_error(unknown_folders_opt.error()));
    }

    container_t container;
    container.emplace_back(new load::devices_t(std::move(devices_opt.value())));
    container.emplace_back(new load::blocks_t(std::move(blocks_opt.value())));
    container.emplace_back(new load::ignored_devices_t(std::move(ignored_devices_opt.value())));
    container.emplace_back(new load::ignored_folders_t(std::move(ignored_folders_opt.value())));
    container.emplace_back(new load::folders_t(std::move(folders_opt.value())));
    container.emplace_back(new load::folder_infos_t(std::move(folder_infos_opt.value())));
    container.emplace_back(new load::file_infos_t(std::move(file_infos_opt.value())));
    container.emplace_back(new load::unknown_folders_t(std::move(unknown_folders_opt.value())));
    container.emplace_back(new load::close_transaction_t(std::move(txn)));

    cluster_diff_ptr_t r = cluster_diff_ptr_t(new load::load_cluster_t(std::move(container)));

    reply_to(request, r);
}

void db_actor_t::on_model_update(model::message::model_update_t &message) noexcept {
    LOG_TRACE(log, "{}, on_model_update", identity);
    auto &diff = *message.payload.diff;
    auto r = diff.visit(*this, nullptr);
    if (!r) {
        auto ee = make_error(r.assume_error());
        LOG_ERROR(log, "{}, on_model_update error: {}", identity, r.assume_error().message());
        do_shutdown(ee);
    }
}

auto db_actor_t::operator()(const model::diff::peer::cluster_update_t &, void *) noexcept -> outcome::result<void> {
    if (cluster->is_tainted()) {
        return outcome::success();
    }

    auto &unknown = cluster->get_unknown_folders();
    if (unknown.empty()) {
        return outcome::success();
    }

    auto txn_opt = get_txn();
    if (!txn_opt) {
        return txn_opt.assume_error();
    }
    auto &txn = *txn_opt.assume_value();
    for (auto &uf : unknown) {
        auto key = uf->get_key();
        auto data = uf->serialize();

        auto r = db::save({key, data}, txn);
        if (!r) {
            return r.assume_error();
        }
    }
    return commit(true);
}

auto db_actor_t::operator()(const model::diff::modify::create_folder_t &diff, void *) noexcept
    -> outcome::result<void> {
    if (cluster->is_tainted()) {
        return outcome::success();
    }
    auto &folder_id = diff.item.id();
    auto folder = cluster->get_folders().by_id(folder_id);
    assert(folder);
    auto f_key = folder->get_key();
    auto f_data = folder->serialize();

    auto txn_opt = get_txn();
    if (!txn_opt) {
        return txn_opt.assume_error();
    }
    auto &txn = *txn_opt.assume_value();

    auto r = db::save({f_key, f_data}, txn);
    if (!r) {
        return r.assume_error();
    }

    auto folder_info = folder->get_folder_infos().by_device(*cluster->get_device());
    auto fi_key = folder_info->get_key();
    auto fi_data = folder_info->serialize();
    r = db::save({fi_key, fi_data}, txn);
    if (!r) {
        return r.assume_error();
    }

    return commit(true);
}

auto db_actor_t::operator()(const model::diff::modify::share_folder_t &diff, void *) noexcept -> outcome::result<void> {
    if (cluster->is_tainted()) {
        return outcome::success();
    }

    auto peer = cluster->get_devices().by_sha256(diff.peer_id);
    assert(peer);
    auto folder = cluster->get_folders().by_id(diff.folder_id);
    assert(folder);

    auto txn_opt = get_txn();
    if (!txn_opt) {
        return txn_opt.assume_error();
    }
    auto &txn = *txn_opt.assume_value();

    auto folder_info = folder->get_folder_infos().by_device(*peer);
    assert(folder_info);

    auto fi_key = folder_info->get_key();
    auto fi_data = folder_info->serialize();
    auto r = db::save({fi_key, fi_data}, txn);
    if (!r) {
        return r.assume_error();
    }

    return commit(true);
}

auto db_actor_t::operator()(const model::diff::modify::unshare_folder_t &diff, void *) noexcept
    -> outcome::result<void> {
    if (cluster->is_tainted()) {
        return outcome::success();
    }

    auto txn_opt = get_txn();
    if (!txn_opt) {
        return txn_opt.assume_error();
    }
    auto &txn = *txn_opt.assume_value();

    {
        auto r = db::remove(diff.folder_info_key, txn);
        if (!r) {
            return r.assume_error();
        }
    }
    for (auto &key : diff.removed_files) {
        auto r = db::remove(key, txn);
        if (!r) {
            return r.assume_error();
        }
    }
    for (auto &key : diff.removed_blocks) {
        auto r = db::remove(key, txn);
        if (!r) {
            return r.assume_error();
        }
    }

    return commit(true);
}

auto db_actor_t::operator()(const model::diff::modify::update_peer_t &diff, void *) noexcept -> outcome::result<void> {
    if (cluster->is_tainted()) {
        return outcome::success();
    }

    auto &device_id = diff.peer_id;
    auto device = cluster->get_devices().by_sha256(device_id);
    assert(device);

    auto key = device->get_key();
    auto data = device->serialize();

    auto txn_opt = get_txn();
    if (!txn_opt) {
        return txn_opt.assume_error();
    }
    auto &txn = *txn_opt.assume_value();

    auto r = db::save({key, data}, txn);
    if (!r) {
        return r.assume_error();
    }

    return commit(true);
}

auto db_actor_t::operator()(const model::diff::modify::clone_file_t &diff, void *) noexcept -> outcome::result<void> {
    if (cluster->is_tainted()) {
        return outcome::success();
    }

    auto folder = cluster->get_folders().by_id(diff.folder_id);
    auto folder_info = folder->get_folder_infos().by_device(*cluster->get_device());
    auto file = folder_info->get_file_infos().by_name(diff.file.name());

    auto txn_opt = get_txn();
    if (!txn_opt) {
        return txn_opt.assume_error();
    }
    auto &txn = *txn_opt.assume_value();

    {
        auto key = file->get_key();
        auto data = file->serialize();
        auto r = db::save({key, data}, txn);
        if (!r) {
            return r.assume_error();
        }
    }

    bool save_fi = diff.identical || diff.create_new_file;
    if (save_fi) {
        auto key = folder_info->get_key();
        auto data = folder_info->serialize();

        auto r = db::save({key, data}, txn);
        if (!r) {
            return r.assume_error();
        }
    }

    return commit(false);
}

auto db_actor_t::operator()(const model::diff::modify::finish_file_ack_t &diff, void *) noexcept
    -> outcome::result<void> {
    if (cluster->is_tainted()) {
        return outcome::success();
    }

    auto folder = cluster->get_folders().by_id(diff.folder_id);
    auto folder_info = folder->get_folder_infos().by_device(*cluster->get_device());
    auto file = folder_info->get_file_infos().by_name(diff.file_name);

    auto txn_opt = get_txn();
    if (!txn_opt) {
        return txn_opt.assume_error();
    }
    auto &txn = *txn_opt.assume_value();

    {
        auto key = file->get_key();
        auto data = file->serialize();
        auto r = db::save({key, data}, txn);
        if (!r) {
            return r.assume_error();
        }
    }

    {
        auto key = folder_info->get_key();
        auto data = folder_info->serialize();

        auto r = db::save({key, data}, txn);
        if (!r) {
            return r.assume_error();
        }
    }

    return commit(false);
}

auto db_actor_t::operator()(const model::diff::modify::local_update_t &diff, void *) noexcept -> outcome::result<void> {
    if (cluster->is_tainted()) {
        return outcome::success();
    }

    auto folder = cluster->get_folders().by_id(diff.folder_id);
    auto folder_info = folder->get_folder_infos().by_device(*cluster->get_device());
    auto file = folder_info->get_file_infos().by_name(diff.file.name());

    auto txn_opt = get_txn();
    if (!txn_opt) {
        return txn_opt.assume_error();
    }
    auto &txn = *txn_opt.assume_value();

    {
        auto key = folder_info->get_key();
        auto data = folder_info->serialize();

        auto r = db::save({key, data}, txn);
        if (!r) {
            return r.assume_error();
        }
    }

    {
        auto key = file->get_key();
        auto data = file->serialize();
        auto r = db::save({key, data}, txn);
        if (!r) {
            return r.assume_error();
        }
    }

    auto &blocks_map = cluster->get_blocks();
    for (const auto &hash : diff.new_blocks) {
        auto block = blocks_map.get(hash);
        auto key = block->get_key();
        auto data = block->serialize();
        auto r = db::save({key, data}, txn);
        if (!r) {
            return r.assume_error();
        }
    }
    for (const auto &hash : diff.removed_blocks) {
        auto data = (char *)alloca(hash.size() + 1);
        data[0] = (char)(db::prefix::block_info);
        std::copy(hash.begin(), hash.end(), data + 1);
        auto key = std::string_view(data, hash.size() + 1);
        auto r = db::remove(key, txn);
        if (!r) {
            return r.assume_error();
        }
    }
    return commit(true);
}

auto db_actor_t::operator()(const model::diff::peer::cluster_remove_t &diff, void *) noexcept -> outcome::result<void> {
    if (cluster->is_tainted()) {
        return outcome::success();
    }

    auto txn_opt = get_txn();
    if (!txn_opt) {
        return txn_opt.assume_error();
    }
    auto &txn = *txn_opt.assume_value();

    for (auto &key : diff.removed_folder_infos) {
        auto r = db::remove(key, txn);
        if (!r) {
            return r.assume_error();
        }
    }
    for (auto &key : diff.removed_files) {
        auto r = db::remove(key, txn);
        if (!r) {
            return r.assume_error();
        }
    }
    for (auto &key : diff.removed_blocks) {
        auto r = db::remove(key, txn);
        if (!r) {
            return r.assume_error();
        }
    }
    for (auto &key : diff.removed_unknown_folders) {
        auto r = db::remove(key, txn);
        if (!r) {
            return r.assume_error();
        }
    }

    return commit(true);
}

auto db_actor_t::operator()(const model::diff::peer::update_folder_t &diff, void *) noexcept -> outcome::result<void> {
    if (cluster->is_tainted()) {
        return outcome::success();
    }

    auto txn_opt = get_txn();
    if (!txn_opt) {
        return txn_opt.assume_error();
    }
    auto &txn = *txn_opt.assume_value();

    auto folder = cluster->get_folders().by_id(diff.folder_id);
    auto folder_info = folder->get_folder_infos().by_device_id(diff.peer_id);

    auto fi_key = folder_info->get_key();
    auto fi_data = folder_info->serialize();
    auto r = db::save({fi_key, fi_data}, txn);
    if (!r) {
        return r.assume_error();
    }

    auto &blocks_map = cluster->get_blocks();
    for (const auto &b : diff.blocks) {
        auto block = blocks_map.get(b.hash());
        auto key = block->get_key();
        auto data = block->serialize();
        auto r = db::save({key, data}, txn);
        if (!r) {
            return r.assume_error();
        }
    }

    auto &files_map = folder_info->get_file_infos();
    for (const auto &f : diff.files) {
        auto file = files_map.by_name(f.name());
        LOG_TRACE(log, "{}, saving {}, seq = {}", identity, file->get_full_name(), file->get_sequence());
        auto key = file->get_key();
        auto data = file->serialize();
        auto r = db::save({key, data}, txn);
        if (!r) {
            return r.assume_error();
        }
    }

    return commit(true);
}

} // namespace syncspirit::net
