#pragma once

#include <memory>
#include <functional>
#include <optional>
#include <memory>
#include <rotor/asio.hpp>
#include <boost/beast.hpp>
#include "spdlog/spdlog.h"
#include "../model/device_id.h"
#include "../utils/tls.h"
#include "../utils/uri.h"

namespace syncspirit::transport {

namespace asio = boost::asio;
namespace sys = boost::system;
namespace http = boost::beast::http;
namespace ssl = asio::ssl;

using tcp = asio::ip::tcp;

using strand_t = asio::io_context::strand;
using resolver_t = tcp::resolver;
using resolved_hosts_t = resolver_t::results_type;
using resolved_item_t = resolved_hosts_t::iterator;

using connect_fn_t = std::function<void(resolved_item_t)>;
using error_fn_t = std::function<void(const sys::error_code &)>;
using handshake_fn_t =
    std::function<void(bool valid, X509 *peer, const tcp::endpoint &, const model::device_id_t *peer_device)>;
using io_fn_t = std::function<void(std::size_t)>;

struct ssl_junction_t {
    model::device_id_t peer;
    const utils::key_pair_t *me;
    bool sni_extension;
};

using ssl_option_t = std::optional<ssl_junction_t>;

struct transport_config_t {
    ssl_option_t ssl_junction;
    utils::URI uri;
    rotor::asio::supervisor_asio_t &supervisor;
    std::optional<tcp::socket> sock;
};

struct base_t {
    virtual ~base_t();
    virtual void async_connect(const resolved_hosts_t &hosts, connect_fn_t &on_connect,
                               error_fn_t &on_error) noexcept = 0;
    virtual void async_handshake(handshake_fn_t &on_handshake, error_fn_t &on_error) noexcept = 0;
    virtual void async_send(asio::const_buffer buff, const io_fn_t &on_write, error_fn_t &on_error) noexcept = 0;
    virtual void async_recv(asio::mutable_buffer buff, const io_fn_t &on_read, error_fn_t &on_error) noexcept = 0;
    virtual void cancel() noexcept = 0;
    virtual asio::ip::address local_address(sys::error_code &ec) noexcept = 0;

    const model::device_id_t &peer_identity() noexcept { return actual_peer; }

  protected:
    base_t(rotor::asio::supervisor_asio_t &supervisor_) noexcept;
    rotor::asio::supervisor_asio_t &supervisor;
    strand_t &strand;
    model::device_id_t actual_peer;
    bool cancelling;

    template <typename Socket>
    void async_connect_impl(Socket &sock, const resolved_hosts_t &hosts, connect_fn_t &on_connect,
                            error_fn_t &on_error) noexcept {
        asio::async_connect(sock, hosts.begin(), hosts.end(), [this, on_connect, on_error](auto &ec, auto addr) {
            if (ec) {
                strand.post([ec = ec, on_error, this]() {
                    if (ec == asio::error::operation_aborted) {
                        cancelling = false;
                    }
                    on_error(ec);
                    supervisor.do_process();
                });
                return;
            }
            strand.post([addr = addr, on_connect, this]() {
                on_connect(addr);
                supervisor.do_process();
            });
        });
    }

    template <typename Socket>
    void async_send_impl(Socket &sock, asio::const_buffer buff, const io_fn_t &on_write,
                         error_fn_t &on_error) noexcept {
        asio::async_write(sock, buff, [&, on_write, on_error, this](auto &ec, auto bytes) {
            if (ec) {
                strand.post([ec = ec, on_error, this]() {
                    if (ec == asio::error::operation_aborted) {
                        cancelling = false;
                    }
                    on_error(ec);
                    supervisor.do_process();
                });
                return;
            }
            strand.post([bytes = bytes, on_write, this]() {
                on_write(bytes);
                supervisor.do_process();
            });
        });
    }

    template <typename Socket>
    void async_recv_impl(Socket &sock, asio::mutable_buffer buff, const io_fn_t &on_write,
                         error_fn_t &on_error) noexcept {
        sock.async_read_some(buff, [&, on_write, on_error, this](auto &ec, auto bytes) {
            if (ec) {
                strand.post([ec = ec, on_error, this]() {
                    if (ec == asio::error::operation_aborted) {
                        cancelling = false;
                    }
                    on_error(ec);
                    supervisor.do_process();
                });
                return;
            }
            strand.post([bytes = bytes, on_write, this]() {
                on_write(bytes);
                supervisor.do_process();
            });
        });
    }

    template <typename Socket> void cancel_impl(Socket &sock) noexcept {
        sys::error_code ec;
        if (!cancelling) {
            cancelling = true;
            sock.cancel(ec);
            if (ec) {
                spdlog::error("base_t::cancel() :: {}", ec.message());
            }
        }
    }
};

using transport_sp_t = std::unique_ptr<base_t>;
transport_sp_t initiate(transport_config_t &config) noexcept;

struct http_base_t {
    using rx_buff_t = boost::beast::flat_buffer;
    using response_t = http::response<http::string_body>;

    http_base_t(rotor::asio::supervisor_asio_t &sup) noexcept;
    virtual ~http_base_t();
    virtual void async_read(rx_buff_t &rx_buff, response_t &response, const io_fn_t &on_read,
                            error_fn_t &on_error) noexcept = 0;

  protected:
    rotor::asio::supervisor_asio_t &supervisor;
    bool in_progess = false;

    template <typename Socket>
    void async_read_impl(Socket &sock, strand_t &strand, rx_buff_t &rx_buff, response_t &response,
                         const io_fn_t &on_read, error_fn_t &on_error) noexcept {
        http::async_read(sock, rx_buff, response, [&, on_read, on_error, this](auto ec, auto bytes) {
            if (ec) {
                strand.post([ec = ec, on_error, this]() {
                    in_progess = false;
                    on_error(ec);
                    supervisor.do_process();
                });
                return;
            }
            strand.post([bytes = bytes, on_read, this]() {
                in_progess = false;
                on_read(bytes);
                supervisor.do_process();
            });
        });
        in_progess = true;
    }
};

} // namespace syncspirit::transport
