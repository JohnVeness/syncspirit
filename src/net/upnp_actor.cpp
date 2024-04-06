// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2019-2022 Ivan Baidakou

#include "upnp_actor.h"
#include "proto/upnp_support.h"
#include "utils/error_code.h"
#include "utils/format.hpp"
#include "names.h"
#include "model/messages.h"
#include "model/diff/modify/update_contact.h"

using namespace syncspirit::net;
using namespace syncspirit::utils;
using namespace syncspirit::proto;

namespace {
namespace resource {
r::plugin::resource_id_t external_port = 0;
r::plugin::resource_id_t http_req = 1;
} // namespace resource
} // namespace

upnp_actor_t::upnp_actor_t(config_t &cfg)
    : r::actor_base_t{cfg}, cluster{cfg.cluster}, main_url{cfg.descr_url}, rx_buff_size{cfg.rx_buff_size},
      external_port(cfg.external_port), debug{cfg.debug} {
    log = utils::get_logger("net.upnp");
}

void upnp_actor_t::configure(r::plugin::plugin_base_t &plugin) noexcept {
    r::actor_base_t::configure(plugin);
    plugin.with_casted<r::plugin::address_maker_plugin_t>([&](auto &p) {
        p.set_identity("upnp", false);
        addr_description = p.create_address();
        addr_external_ip = p.create_address();
        addr_mapping = p.create_address();
        addr_unmapping = p.create_address();
        addr_validate = p.create_address();
    });
    plugin.with_casted<r::plugin::starter_plugin_t>([&](auto &p) {
        p.subscribe_actor(&upnp_actor_t::on_igd_description, addr_description);
        p.subscribe_actor(&upnp_actor_t::on_external_ip, addr_external_ip);
        p.subscribe_actor(&upnp_actor_t::on_mapping_port, addr_mapping);
        p.subscribe_actor(&upnp_actor_t::on_unmapping_port, addr_unmapping);
        p.subscribe_actor(&upnp_actor_t::on_validate, addr_validate);
    });
    plugin.with_casted<r::plugin::registry_plugin_t>([&](auto &p) {
        p.discover_name(names::http10, http_client, true).link(true);
        p.discover_name(names::coordinator, coordinator).link();
    });
    plugin.with_casted<r::plugin::link_client_plugin_t>([&](auto &p) {
        p.on_unlink([&](auto &req) {
            if (resources->has(resource::external_port)) {
                unlink_request = &req;
                return true;
            } else {
                return false;
            }
        });
    });
}

void upnp_actor_t::on_start() noexcept {
    LOG_TRACE(log, "{}, on_start", identity);
    r::actor_base_t::on_start();
    rx_buff = std::make_shared<payload::http_request_t::rx_buff_t>();

    fmt::memory_buffer tx_buff;
    auto res = make_description_request(tx_buff, main_url);
    if (!res) {
        auto &ec = res.error();
        LOG_TRACE(log, "{}, error making description request :: {}", identity, ec.message());
        return do_shutdown(make_error(ec));
    }
    make_request(addr_description, main_url, std::move(tx_buff), true);
}

void upnp_actor_t::make_request(const r::address_ptr_t &addr, utils::URI &uri, fmt::memory_buffer &&tx_buff,
                                bool get_local_address) noexcept {
    resources->acquire(resource::http_req);
    auto timeout = shutdown_timeout * 8 / 9;
    http_request = request_via<payload::http_request_t>(http_client, addr, uri, std::move(tx_buff), rx_buff,
                                                        rx_buff_size, get_local_address)
                       .send(timeout);
}

void upnp_actor_t::request_finish() noexcept {
    resources->release(resource::http_req);
    http_request.reset();
}

void upnp_actor_t::on_igd_description(message::http_response_t &msg) noexcept {
    LOG_TRACE(log, "{}, on_igd_description, state = {}", identity, (int)state);
    request_finish();

    auto &ee = msg.payload.ee;
    if (ee) {
        auto inner = utils::make_error_code(utils::error_code_t::igd_description_failed);
        LOG_WARN(log, "{}, get IGD description: {}", identity, ee->message());
        return do_shutdown(make_error(inner, ee));
    }
    if (state > r::state_t::OPERATIONAL) {
        return;
    }

    local_address = msg.payload.res->local_addr.value();
    auto &body = msg.payload.res->response.body();
    if (debug) {
        LOG_DEBUG(log, "igd description reply: {}\n", std::string_view(body.data(), body.size()));
    }
    auto igd_result = parse_igd(body.data(), body.size());
    if (!igd_result) {
        auto &ec = igd_result.error();
        LOG_WARN(log, "{}, can't get IGD result: {}", identity, ec.message());
        std::string xml(body);
        LOG_DEBUG(log, "xml:\n{0}\n", xml);
        return do_shutdown(make_error(ec));
    }

    rx_buff->consume(msg.payload.res->bytes);
    auto &igd = igd_result.value();
    std::string control_url = fmt::format("http://{0}:{1}{2}", main_url.host, main_url.port, igd.control_path);
    std::string descr_url = fmt::format("http://{0}:{1}{2}", main_url.host, main_url.port, igd.description_path);
    LOG_DEBUG(log, "{}, IGD control url: {}, description url: {}", identity, control_url, descr_url);

    auto url_option = utils::parse(control_url.c_str());
    if (!url_option) {
        LOG_ERROR(log, "{}, can't parse IGD url {}", identity, control_url);
        auto ec = utils::make_error_code(utils::error_code_t::unparsable_control_url);
        return do_shutdown(make_error(ec));
    }
    igd_control_url = url_option.value();

    fmt::memory_buffer tx_buff;
    auto res = make_external_ip_request(tx_buff, igd_control_url);
    if (!res) {
        auto &ec = res.error();
        LOG_TRACE(log, "{}, error making external ip address request :: {}", identity, ec.message());
        return do_shutdown(make_error(ec));
    }
    make_request(addr_external_ip, igd_control_url, std::move(tx_buff));
}

void upnp_actor_t::on_external_ip(message::http_response_t &msg) noexcept {
    LOG_TRACE(log, "{}, on_external_ip", identity);
    request_finish();

    auto &ee = msg.payload.ee;
    if (ee) {
        LOG_WARN(log, "{}, get external IP address: {}", identity, ee->message());
        auto inner = utils::make_error_code(utils::error_code_t::external_ip_failed);
        return do_shutdown(make_error(inner, ee));
    }
    if (state > r::state_t::OPERATIONAL) {
        return;
    }

    auto &body = msg.payload.res->response.body();
    if (debug) {
        LOG_DEBUG(log, "external ip reply: {}\n", std::string_view(body.data(), body.size()));
    }
    auto ip_addr_result = parse_external_ip(body.data(), body.size());
    if (!ip_addr_result) {
        auto &ec = ip_addr_result.error();
        LOG_WARN(log, "{}, can't get external IP address: {}", identity, ec.message());
        std::string xml(body);
        LOG_DEBUG(log, "xml:\n{0}\n", xml);
        return do_shutdown(make_error(ec));
    }
    auto &ip_addr = ip_addr_result.value();
    LOG_DEBUG(log, "{}, external IP addr: {}", identity, ip_addr);
    rx_buff->consume(msg.payload.res->bytes);

    sys::error_code io_ec;
    external_addr = asio::ip::address::from_string(ip_addr, io_ec);
    if (ee) {
        LOG_WARN(log, "{}, can't external IP address '{}' is incorrect: {}", identity, ip_addr, io_ec.message());
        return do_shutdown(make_error(io_ec));
    }

    auto local_port = 0;
    for (auto &uri : cluster->get_device()->get_uris()) {
        if (uri.port) {
            local_port = uri.port;
            break;
        }
    }
    LOG_DEBUG(log, "{}, going to map {}:{} => {}:{}", identity, ip_addr, external_port, local_address, local_port);

    fmt::memory_buffer tx_buff;
    auto res = make_mapping_request(tx_buff, igd_control_url, external_port, local_address.to_string(), local_port);
    if (!res) {
        auto &ec = res.error();
        LOG_TRACE(log, "{}, error making port mapping request :: {}", identity, ec.message());
        return do_shutdown(make_error(ec));
    }
    make_request(addr_mapping, igd_control_url, std::move(tx_buff));
}

void upnp_actor_t::on_mapping_port(message::http_response_t &msg) noexcept {
    LOG_TRACE(log, "{}, on_mapping_port", identity);
    request_finish();

    bool ok = false;
    auto &ee = msg.payload.ee;
    if (ee) {
        LOG_WARN(log, "{}, unsuccessfull port mapping: {}", ee->message(), identity);
        auto inner = utils::make_error_code(utils::error_code_t::portmapping_failed);
        return do_shutdown(make_error(inner, ee));
    } else if (state > r::state_t::OPERATIONAL) {
        return;
    }

    auto &body = msg.payload.res->response.body();
    if (debug) {
        LOG_DEBUG(log, "mapping port reply: {}\n", std::string_view(body.data(), body.size()));
    }
    auto result = parse_mapping(body.data(), body.size());
    if (!result) {
        LOG_WARN(log, "{}, can't parse port mapping reply : {}", identity, result.error().message());
        std::string xml(body);
        LOG_DEBUG(log, "xml:\n{0}\n", xml);
    } else {
        rx_buff->consume(msg.payload.res->bytes);
        if (!result.value()) {
            LOG_WARN(log, "{}, unsuccessfull port mapping", identity);
            LOG_DEBUG(log, "mapping port reply: {}\n", std::string_view(body.data(), body.size()));
        } else {
            LOG_DEBUG(log, "{}, port mapping succeeded", identity);
            ok = true;
        }
    }

    if (ok) {
        fmt::memory_buffer tx_buff;
        auto res = make_mapping_validation_request(tx_buff, igd_control_url, external_port);
        if (!res) {
            auto &ec = res.error();
            LOG_TRACE(log, "{}, error making port mapping validation request :: {}", identity, ec.message());
            return do_shutdown(make_error(ec));
        }
        make_request(addr_validate, igd_control_url, std::move(tx_buff));
    }
}

void upnp_actor_t::on_unmapping_port(message::http_response_t &msg) noexcept {
    LOG_TRACE(log, "{}, on_unmapping_port", identity);
    request_finish();
    resources->release(resource::external_port);

    auto &ee = msg.payload.ee;
    if (ee) {
        LOG_WARN(log, "upnp_actor:: unsuccessfull port mapping: {}", ee->message());
        return;
    }
    auto &body = msg.payload.res->response.body();
    auto content = std::string_view(body.data(), body.size());
    if (debug) {
        LOG_DEBUG(log, "unmapping port reply: {}\n", content);
    }
    auto result = parse_unmapping(body.data(), body.size());
    if (!result) {
        LOG_WARN(log, "{}, can't parse port unmapping reply : {}", identity, result.error().message());
        LOG_DEBUG(log, "xml:\n{0}\n", content);
    } else if (!result.value()) {
        LOG_WARN(log, "{}, port unmapping failed", identity);
        LOG_DEBUG(log, "xml:\n{0}\n", content);
    } else {
        LOG_DEBUG(log, "{}, succesfully unmmaped external port {}", identity, external_port);
    }
    if (unlink_request) {
        auto p = get_plugin(r::plugin::link_client_plugin_t::class_identity);
        auto plugin = static_cast<r::plugin::link_client_plugin_t *>(p);
        plugin->forget_link(*unlink_request);
        unlink_request.reset();
    }
}

void upnp_actor_t::on_validate(message::http_response_t &msg) noexcept {
    LOG_TRACE(log, "{}, on_validate", identity);
    request_finish();

    auto &ee = msg.payload.ee;
    if (ee) {
        LOG_WARN(log, "upnp_actor:: unsuccessfull port mapping: {}", ee->message());
        return;
    }
    auto &body = msg.payload.res->response.body();
    auto content = std::string_view(body.data(), body.size());
    if (debug) {
        LOG_DEBUG(log, "validation port reply: {}\n", content);
    }
    bool ok = false;
    auto result = parse_mapping_validation(body.data(), body.size());
    if (!result) {
        LOG_WARN(log, "{}, can't parse port mapping validation reply : {}", identity, result.error().message());
        LOG_DEBUG(log, "xml:\n{0}\n", content);
    } else if (!result.value()) {
        LOG_WARN(log, "{}, port mapping validation failed", identity);
        LOG_DEBUG(log, "xml:\n{0}\n", content);
    } else {
        LOG_DEBUG(log, "{}, succesfully validate external port {} mapping", identity, external_port);
        ok = true;
    }

    if (ok) {
        resources->acquire(resource::external_port);
        using namespace model::diff;
        auto diff = model::diff::contact_diff_ptr_t{};
        diff = new modify::update_contact_t(*cluster, {external_addr.to_string(), local_address.to_string()});
        send<model::payload::contact_update_t>(coordinator, std::move(diff), this);
    }
}

void upnp_actor_t::shutdown_start() noexcept {
    LOG_TRACE(log, "{}, shutdown_start", identity);
    r::actor_base_t::shutdown_start();

    if (resources->has(resource::http_req)) {
        send<message::http_cancel_t::payload_t>(http_client, *http_request, get_address());
    }

    if (resources->has(resource::external_port)) {
        LOG_TRACE(log, "{}, going to unmap extenal port {}", identity, external_port);
        fmt::memory_buffer tx_buff;
        auto res = make_unmapping_request(tx_buff, igd_control_url, external_port);
        if (!res) {
            LOG_WARN(log, "{}, error making port mapping request :: {}", identity, res.error().message());
            resources->release(resource::external_port);
            return;
        }
        make_request(addr_unmapping, igd_control_url, std::move(tx_buff));
    }
}
