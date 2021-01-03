#include "tcp.h"
#include "spdlog/spdlog.h"

using namespace syncspirit::transport;

http_t::http_t(const transport_config_t &config) noexcept : tcp_t{config}, http_base_t(config.supervisor) {}

tcp_t::tcp_t(const transport_config_t &config) noexcept : base_t{config.supervisor}, sock(strand.context()) {
    assert(!config.ssl_junction);
}

void tcp_t::async_connect(const resolved_hosts_t &hosts, connect_fn_t &on_connect, error_fn_t &on_error) noexcept {
    async_connect_impl(sock, hosts, on_connect, on_error);
}

void tcp_t::async_handshake(handshake_fn_t &on_handshake, error_fn_t &on_error) noexcept {
    sys::error_code ec;
    auto endpoint = sock.remote_endpoint(ec);
    if (ec) {
        strand.post([on_error, ec, this]() {
            on_error(ec);
            supervisor.do_process();
        });
        return;
    }
    strand.post([on_handshake, endpoint, this]() {
        on_handshake(true, nullptr, endpoint, nullptr);
        supervisor.do_process();
    });
}

void tcp_t::async_send(asio::const_buffer buff, const io_fn_t &on_write, error_fn_t &on_error) noexcept {
    async_send_impl(sock, buff, on_write, on_error);
}

void tcp_t::async_recv(asio::mutable_buffer buff, const io_fn_t &on_read, error_fn_t &on_error) noexcept {
    async_recv_impl(sock, buff, on_read, on_error);
}

void tcp_t::cancel() noexcept { cancel_impl(sock); }

asio::ip::address tcp_t::local_address(sys::error_code &ec) noexcept {
    auto endpoint = sock.local_endpoint(ec);
    if (!ec) {
        return endpoint.address();
    }
    return {};
}

void http_t::async_read(rx_buff_t &rx_buff, response_t &response, const io_fn_t &on_read,
                        error_fn_t &on_error) noexcept {
    async_read_impl(sock, strand, rx_buff, response, on_read, on_error);
}
