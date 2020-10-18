#pragma once

#include "base.h"

namespace syncspirit::transport {

struct tcp_t : base_t {
    tcp_t(const transport_config_t &config) noexcept;
    void async_connect(const resolved_hosts_t &hosts, connect_fn_t &on_connect, error_fn_t &on_error) noexcept override;
    void async_handshake(handshake_fn_t &on_handshake, error_fn_t &on_error) noexcept override;
    void async_write(asio::const_buffer buff, const io_fn_t &on_write, error_fn_t &on_error) noexcept override;
    void cancel() noexcept override;

  protected:
    tcp::socket sock;
};

struct http_t : tcp_t, http_base_t {
    using tcp_t::tcp_t;

    void async_read(rx_buff_t &rx_buff, response_t &response, const io_fn_t &on_read,
                    error_fn_t &on_error) noexcept override;
};

} // namespace syncspirit::transport