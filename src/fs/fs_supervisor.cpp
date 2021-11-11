#include "fs_supervisor.h"
#include "../net/names.h"
#include "../hasher/hasher_proxy_actor.h"
#include "scan_actor.h"
#include "file_actor.h"

using namespace syncspirit::fs;

namespace {
namespace resource {
r::plugin::resource_id_t model = 0;
}
} // namespace


fs_supervisor_t::fs_supervisor_t(config_t &cfg)
    : parent_t(cfg), fs_config{cfg.fs_config}, hasher_threads{cfg.hasher_threads} {
    log = utils::get_logger("fs.supervisor");
}

void fs_supervisor_t::configure(r::plugin::plugin_base_t &plugin) noexcept {
    parent_t::configure(plugin);
    plugin.with_casted<r::plugin::address_maker_plugin_t>([&](auto &p) { p.set_identity("fs::supervisor", false); });
    plugin.with_casted<r::plugin::registry_plugin_t>([&](auto &p) {
        p.discover_name(net::names::coordinator, coordinator, true).link(false).callback([&](auto phase, auto &ee) {
            if (!ee && phase == r::plugin::registry_plugin_t::phase_t::linking) {
                request<net::payload::model_request_t>(coordinator).send(init_timeout);
                resources->acquire(resource::model);
            }
        });
    });

    plugin.with_casted<r::plugin::starter_plugin_t>([&](auto &p) {
            p.subscribe_actor(&fs_supervisor_t::on_model_request);
    }, r::plugin::config_phase_t::PREINIT );
}

void fs_supervisor_t::launch() noexcept {
    auto &timeout = shutdown_timeout;
    create_actor<file_actor_t>().timeout(timeout).finish();
    auto hasher_addr = create_actor<hasher::hasher_proxy_actor_t>()
                           .hasher_threads(hasher_threads)
                           .name("fs::hasher_proxy")
                           .timeout(timeout)
                           .finish()
                           ->get_address();
    scan_actor = create_actor<scan_actor_t>()
                     .fs_config(fs_config)
                     .hasher_proxy(hasher_addr)
                     .requested_hashes_limit(hasher_threads * 2)
                     .timeout(timeout)
                     .finish();
}

void fs_supervisor_t::on_model_request(net::message::model_response_t &res) noexcept {
    LOG_TRACE(log, "{}, on_model_request", identity);
    resources->release(resource::model);
    auto ee = res.payload.ee;
    if (ee) {
        LOG_ERROR(log, "{}, cannot get model: {}", ee->message());
        return do_shutdown(ee);
    }
    cluster = std::move(res.payload.res.cluster);
    //launch();
}

void fs_supervisor_t::on_start() noexcept {
    LOG_TRACE(log, "{}, on_start", identity);
    r::actor_base_t::on_start();
}
