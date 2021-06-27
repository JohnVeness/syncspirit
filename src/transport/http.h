#pragma once
#include "base.h"
#include "stream.h"
#include <boost/beast.hpp>

namespace syncspirit::transport {

namespace http = boost::beast::http;

struct http_interface_t {
    using rx_buff_t = boost::beast::flat_buffer;
    using response_t = http::response<http::string_body>;
    virtual void async_read(rx_buff_t &rx_buff, response_t &response, io_fn_t &on_read,
                            error_fn_t &on_error) noexcept = 0;
    inline virtual ~http_interface_t() {}
};

struct http_base_t : model::arc_base_t<http_base_t>, http_interface_t, stream_interface_t {};

using http_sp_t = model::intrusive_ptr_t<http_base_t>;

http_sp_t initiate_http(transport_config_t &config) noexcept;

} // namespace syncspirit::transport
