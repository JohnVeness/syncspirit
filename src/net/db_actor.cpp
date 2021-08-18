#include "db_actor.h"
#include "names.h"
#include "spdlog/spdlog.h"
#include <cstddef>
#include <string_view>
#include "../db/utils.h"
#include "../db/error_code.h"
#include "../db/transaction.h"

namespace syncspirit::net {

namespace {
namespace resource {
r::plugin::resource_id_t db = 0;
}

} // namespace

db_actor_t::db_actor_t(config_t &config)
    : r::actor_base_t{config}, env{nullptr}, db_dir{config.db_dir}, device{config.device}, generator(rd()) {
    auto r = mdbx_env_create(&env);
    if (r != MDBX_SUCCESS) {
        spdlog::critical("{}, mbdx environment creation error ({}): {}", r, mdbx_strerror(r), names::db);
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
    plugin.with_casted<r::plugin::address_maker_plugin_t>([&](auto &p) { p.set_identity(names::db, false); });
    plugin.with_casted<r::plugin::registry_plugin_t>([&](auto &p) { p.register_name(names::db, get_address()); });
    plugin.with_casted<r::plugin::starter_plugin_t>([&](auto &p) {
        open();
        p.subscribe_actor(&db_actor_t::on_cluster_load);
        p.subscribe_actor(&db_actor_t::on_store_ignored_device);
        p.subscribe_actor(&db_actor_t::on_store_device);
        p.subscribe_actor(&db_actor_t::on_store_ignored_folder);
        p.subscribe_actor(&db_actor_t::on_store_new_folder);
        p.subscribe_actor(&db_actor_t::on_store_folder);
        p.subscribe_actor(&db_actor_t::on_store_folder_info);
    });
}

void db_actor_t::open() noexcept {
    resources->acquire(resource::db);
    auto flags = MDBX_WRITEMAP | MDBX_NOTLS | MDBX_COALESCE | MDBX_LIFORECLAIM;
    auto r = mdbx_env_open(env, db_dir.c_str(), flags, 0664);
    if (r != MDBX_SUCCESS) {
        spdlog::error("{}, open, mbdx open environment error ({}): {}", identity, r, mdbx_strerror(r));
        resources->release(resource::db);
        return do_shutdown(make_error(db::make_error_code(r)));
    }
    auto txn = db::make_transaction(db::transaction_type_t::RO, env);
    if (!txn) {
        spdlog::error("{}, open, cannot create transaction {}", identity, txn.error().message());
        resources->release(resource::db);
        return do_shutdown(make_error(db::make_error_code(r)));
    }

    auto db_ver = db::get_version(txn.value());
    if (!db_ver) {
        spdlog::error("{}, open, cannot get db version :: {}", identity, db_ver.error().message());
        resources->release(resource::db);
        return do_shutdown(make_error(db::make_error_code(r)));
    }
    auto version = db_ver.value();
    spdlog::debug("got db version: {}, expected : {} ", version, db::version);

    txn = db::make_transaction(db::transaction_type_t::RW, env);
    if (!txn) {
        spdlog::error("{}, open, cannot create transaction {}", identity, txn.error().message());
        resources->release(resource::db);
        return do_shutdown(make_error(db::make_error_code(r)));
    }
    if (db_ver.value() != db::version) {
        auto r = db::migrate(version, device, txn.value());
        if (!r) {
            spdlog::error("{}, open, cannot migrate db {}", identity, r.error().message());
            resources->release(resource::db);
            return do_shutdown(make_error(r.error()));
        }
        spdlog::info("{}, open, successufully migrated db: {} -> {} ", identity, version, db::version);
    }
    resources->release(resource::db);
}

void db_actor_t::on_start() noexcept {
    r::actor_base_t::on_start();
    spdlog::trace("{}, on_start", identity);
}

void db_actor_t::on_cluster_load(message::load_cluster_request_t &message) noexcept {
    auto txn_opt = db::make_transaction(db::transaction_type_t::RO, env);
    if (!txn_opt) {
        return reply_with_error(message, make_error(txn_opt.error()));
    }
    auto &txn = txn_opt.value();

    auto devices_opt = db::load_devices(txn);
    if (!devices_opt) {
        return reply_with_error(message, make_error(devices_opt.error()));
    }

    bool local_fix = false;
    auto &devices = devices_opt.value();
    for (auto &it : devices) {
        auto &d = it.second;
        if (d->get_id() == device->get_id()) {
            local_fix = true;
            device->set_db_key(d->get_db_key());
            devices.remove(d);
            devices.put(device);
            break;
        }
    }
    if (!local_fix) {
        auto ec = db::make_error_code(db::error_code::local_device_not_found);
        return reply_with_error(message, make_error(ec));
    }

    auto blocks_opt = db::load_block_infos(txn);
    if (!blocks_opt) {
        return reply_with_error(message, make_error(blocks_opt.error()));
    }
    auto &blocks = blocks_opt.value();
    spdlog::trace("{}, on_cluster_load, blocks count =  {}", identity, blocks.size());

    auto ignored_devices_opt = db::load_ignored_devices(txn);
    if (!ignored_devices_opt) {
        return reply_with_error(message, make_error(ignored_devices_opt.error()));
    }
    auto &ignored_devices = ignored_devices_opt.value();

    auto ignored_folders_opt = db::load_ignored_folders(txn);
    if (!ignored_folders_opt) {
        return reply_with_error(message, make_error(ignored_folders_opt.error()));
    }
    auto &ignored_folders = ignored_folders_opt.value();

    auto folders_opt = db::load_folders(txn_opt.value());
    if (!folders_opt) {
        return reply_with_error(message, make_error(folders_opt.error()));
    }
    auto &folders = folders_opt.value();

    auto folder_infos_opt = db::load_folder_infos(devices, folders, txn);
    if (!folder_infos_opt) {
        return reply_with_error(message, make_error(folder_infos_opt.error()));
    }
    auto folder_infos = folder_infos_opt.value();

    /* should be assigned earlier before file_infos are loaded */
    auto cluster = model::cluster_ptr_t(new model::cluster_t(device));
    cluster->assign_folders(folders);
    cluster->assign_blocks(std::move(blocks));

    auto file_infos_opt = db::load_file_infos(folders, txn);
    if (!file_infos_opt) {
        return reply_with_error(message, make_error(file_infos_opt.error()));
    }
    auto &file_infos = file_infos_opt.value();

    // correctly link
    for (auto &it : file_infos) {
        auto &fi = it.second;
        auto folder = fi->get_folder();
        spdlog::trace("{}, on_cluster_load: {}/{}, seq = {}", identity, folder->label(), fi->get_name(),
                      fi->get_sequence());
        folder->add(fi);
    }

    for (auto &it : folder_infos) {
        auto &fi = it.second;
        auto folder = fi->get_folder();
        spdlog::trace("{}, on_cluster_load: {} on {}, max seq = {}", identity, folder->label(),
                      fi->get_device()->device_id, fi->get_max_sequence());
        folder->add(fi);
    }

    if (auto my_d = devices.by_id(device->get_id()); !my_d) {
        devices.put(device);
    }

    reply_to(message, std::move(cluster), std::move(devices), std::move(ignored_devices), std::move(ignored_folders));
}

void db_actor_t::on_store_ignored_device(message::store_ignored_device_request_t &message) noexcept {
    auto txn_opt = db::make_transaction(db::transaction_type_t::RW, env);
    if (!txn_opt) {
        return reply_with_error(message, make_error(txn_opt.error()));
    }
    auto &txn = txn_opt.value();
    auto &peer_device = message.payload.request_payload.device;
    auto r = db::store_ignored_device(peer_device, txn);
    if (!r) {
        reply_with_error(message, make_error(r.error()));
        return;
    }

    r = txn.commit();
    if (!r) {
        reply_with_error(message, make_error(r.error()));
        return;
    }

    reply_to(message);
}

void db_actor_t::on_store_device(message::store_device_request_t &message) noexcept {
    auto txn_opt = db::make_transaction(db::transaction_type_t::RW, env);
    if (!txn_opt) {
        return reply_with_error(message, make_error(txn_opt.error()));
    }
    auto &txn = txn_opt.value();
    auto &peer_device = message.payload.request_payload.device;
    auto r = db::store_device(peer_device, txn);
    if (!r) {
        reply_with_error(message, make_error(r.error()));
        return;
    }

    r = txn.commit();
    if (!r) {
        reply_with_error(message, make_error(r.error()));
        return;
    }

    reply_to(message);
}

void db_actor_t::on_store_ignored_folder(message::store_ignored_folder_request_t &message) noexcept {
    auto txn_opt = db::make_transaction(db::transaction_type_t::RW, env);
    if (!txn_opt) {
        return reply_with_error(message, make_error(txn_opt.error()));
    }
    auto &txn = txn_opt.value();
    auto &folder = message.payload.request_payload.folder;
    auto r = db::store_ignored_folder(folder, txn);
    if (!r) {
        reply_with_error(message, make_error(r.error()));
        return;
    }

    r = txn.commit();
    if (!r) {
        reply_with_error(message, make_error(r.error()));
        return;
    }

    reply_to(message);
}

void db_actor_t::on_store_new_folder(message::store_new_folder_request_t &message) noexcept {
    auto txn_opt = db::make_transaction(db::transaction_type_t::RW, env);
    if (!txn_opt) {
        return reply_with_error(message, make_error(txn_opt.error()));
    }
    auto &txn = txn_opt.value();
    auto &db_folder = message.payload.request_payload.folder;
    auto folder = model::folder_ptr_t(new model::folder_t(db_folder));

    auto r = db::store_folder(folder, txn);
    if (!r) {
        reply_with_error(message, make_error(r.error()));
        return;
    }

    // local
    auto key_option = txn.next_sequence();
    if (!key_option) {
        reply_with_error(message, make_error(key_option.error()));
        return;
    }
    auto key_local = key_option.value();
    db::FolderInfo db_fi_local;
    db_fi_local.set_index_id(distribution(generator));
    db_fi_local.set_max_sequence(0);
    auto src_local = device.get();
    auto fi_local = model::folder_info_ptr_t(new model::folder_info_t(db_fi_local, src_local, folder.get(), key_local));

    r = db::store_folder_info(fi_local, txn);
    if (!r) {
        reply_with_error(message, make_error(r.error()));
        return;
    }

    // remote
    auto fi_source = model::folder_info_ptr_t{};
    auto &src_index = message.payload.request_payload.source_index;
    auto &source = message.payload.request_payload.source;
    if (src_index) {
        assert(source);
        db::FolderInfo db_fi_source;
        db_fi_source.set_index_id(src_index);
        db_fi_source.set_max_sequence(0); // always start afresh
        auto src = message.payload.request_payload.source.get();

        key_option = txn.next_sequence();
        if (!key_option) {
            reply_with_error(message, make_error(key_option.error()));
            return;
        }
        auto key_source = key_option.value();
        fi_source = model::folder_info_ptr_t(new model::folder_info_t(db_fi_source, src, folder.get(), key_source));

        r = db::store_folder_info(fi_source, txn);
        if (!r) {
            reply_with_error(message, make_error(r.error()));
            return;
        }
    }

    r = txn.commit();
    if (!r) {
        reply_with_error(message, make_error(r.error()));
        return;
    }

    folder->add(fi_local);
    if (fi_source) {
        folder->add(fi_source);
    }

    reply_to(message, std::move(folder));
}

void db_actor_t::on_store_folder(message::store_folder_request_t &message) noexcept {
    auto txn_opt = db::make_transaction(db::transaction_type_t::RW, env);
    if (!txn_opt) {
        return reply_with_error(message, make_error(txn_opt.error()));
    }
    auto &txn = txn_opt.value();
    auto &folder = message.payload.request_payload.folder;
    auto r = db::store_folder(folder, txn);
    if (!r) {
        reply_with_error(message, make_error(r.error()));
        return;
    }
    folder->unmark_dirty();

    for (auto &it : folder->get_folder_infos()) {
        auto &fi = it.second;
        if (!fi->is_dirty()) {
            continue;
        }
        r = db::store_folder_info(fi, txn);
        spdlog::trace("{}, on_store_folder folder_info = {} max seq = {}", identity, fi->get_db_key(),
                      fi->get_max_sequence());
        if (!r) {
            reply_with_error(message, make_error(r.error()));
            return;
        }
        fi->unmark_dirty();
    }

    model::block_infos_map_t dirty_blocks;
    for (auto &it : folder->get_file_infos()) {
        auto &fi = it.second;
        if (!fi->is_dirty()) {
            continue;
        }
        for (auto &bit : fi->get_blocks()) {
            if (!bit->is_dirty()) {
                continue;
            }
            r = db::store_block_info(bit, txn);
            if (!r) {
                reply_with_error(message, make_error(r.error()));
                return;
            }
            bit->unmark_dirty();
        }
    }

    for (auto &it : folder->get_file_infos()) {
        auto &fi = it.second;
        if (!fi->is_dirty()) {
            continue;
        }
        r = db::store_file_info(it.second, txn);
        if (!r) {
            reply_with_error(message, make_error(r.error()));
            return;
        }
        fi->unmark_dirty();
    }

    r = txn.commit();
    if (!r) {
        reply_with_error(message, make_error(r.error()));
        return;
    }

    reply_to(message);
}

void db_actor_t::on_store_folder_info(message::store_folder_info_request_t &message) noexcept {
    auto &fi = message.payload.request_payload.folder_info;
    spdlog::trace("{}, on_store_folder_info folder_info = {}", identity, fi->get_db_key());
    if (!fi->is_dirty()) {
        spdlog::warn("{}, folder_info = {}  (from {}) is not dirty, no need to save", identity, fi->get_db_key(),
                     fi->get_folder()->label());
        reply_to(message);
        return;
    }

    auto txn_opt = db::make_transaction(db::transaction_type_t::RW, env);
    if (!txn_opt) {
        return reply_with_error(message, make_error(txn_opt.error()));
    }
    auto &txn = txn_opt.value();
    auto r = db::store_folder_info(fi, txn);
    if (!r) {
        reply_with_error(message, make_error(r.error()));
        return;
    }

    r = txn.commit();
    if (!r) {
        reply_with_error(message, make_error(r.error()));
        return;
    }

    fi->unmark_dirty();
    reply_to(message);
}

} // namespace syncspirit::net
