#pragma once

#include "../configuration.h"
#include <type_traits>
#include <functional>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/smart_ptr/local_shared_ptr.hpp>
#include <boost/optional.hpp>
#include <rotor/asio/supervisor_asio.h>
#include "../utils/upnp_support.h"

namespace syncspirit {
namespace net {

namespace r = rotor;
namespace ra = rotor::asio;
namespace asio = boost::asio;
namespace sys = boost::system;
namespace pt = boost::posix_time;
namespace http = boost::beast::http;
using udp = asio::ip::udp;
using tcp = asio::ip::tcp;

struct upnp_actor_t : public r::actor_base_t {
    using udp_socket_t = asio::ip::udp::socket;
    using tcp_socket_t = asio::ip::tcp::socket;
    using tcp_socket_ptr_t = std::unique_ptr<asio::ip::tcp::socket>;
    using timer_t = asio::deadline_timer;
    using discovery_option_t = boost::optional<utils::discovery_result>;
    using resolve_results_t = asio::ip::tcp::resolver::results_type;
    using resolve_it_t = asio::ip::tcp::resolver::results_type::iterator;
    using http_response_t = boost::beast::http::response<boost::beast::http::string_body>;
    using http_response_option_t = boost::optional<http_response_t>;
    using response_callback_t = std::function<void(std::size_t)>;
    using callback_option_t = boost::optional<response_callback_t>;

    struct resp_description_t {
        std::size_t bytes;
    };
    struct resp_external_ip_t {
        std::size_t bytes;
    };

    upnp_actor_t(ra::supervisor_asio_t &sup, const config::upnp_config_t &cfg);

    virtual void on_initialize(r::message_t<r::payload::initialize_actor_t> &) noexcept override;
    virtual void on_start(r::message_t<r::payload::start_actor_t> &) noexcept override;
    virtual void on_shutdown(r::message_t<r::payload::shutdown_request_t> &) noexcept override;
    virtual void on_description(r::message_t<resp_description_t> &) noexcept;
    virtual void on_external_ip(r::message_t<resp_external_ip_t> &) noexcept;

    void trigger_shutdown() noexcept;
    void trigger_discovery() noexcept;

    void on_timeout_trigger() noexcept;
    void on_timeout_error(const sys::error_code &ec) noexcept;
    void on_discovery_sent(std::size_t bytes) noexcept;
    void on_udp_error(const sys::error_code &ec) noexcept;
    void on_discovery_received(std::size_t bytes) noexcept;
    void on_resolve_error(const sys::error_code &ec) noexcept;
    void on_resolve(resolve_results_t results) noexcept;
    void on_tcp_error(const sys::error_code &ec) noexcept;
    void on_connect(resolve_it_t endpoint) noexcept;
    void on_request_sent(std::size_t bytes) noexcept;
    void on_response_received(std::size_t bytes) noexcept;

    template <typename F> void trigger_request(F &&request_fn, response_callback_t &&cb) noexcept {
        auto [request_result, url] = request_fn();
        if (!request_result) {
            spdlog::warn("upnp_actor:: can't serialize request: {}", request_result.error().message());
            return trigger_shutdown();
        }
        if (callback_option) {
            spdlog::warn("upnp_actor:: responce callback already set");
            return trigger_shutdown();
        }
        callback_option = std::move(cb);
        request_url = url;
        response_option.reset(http_response_t{});

        spdlog::trace("upnp_actor going to resolve {}", url.full);
        auto fwd = ra::forwarder_t(*this, &upnp_actor_t::on_resolve, &upnp_actor_t::on_resolve_error);
        resolver.async_resolve(url.host, url.service, std::move(fwd));
        activities_flag |= RESOLVER_ACTIVE;
    }

  private:
    const static constexpr std::uint32_t SHUTDOWN_ACTIVE = 1 << 1;
    const static constexpr std::uint32_t TIMER_ACTIVE = 1 << 2;
    const static constexpr std::uint32_t UDP_ACTIVE = 1 << 3;
    const static constexpr std::uint32_t TCP_ACTIVE = 1 << 4;
    const static constexpr std::uint32_t RESOLVER_ACTIVE = 1 << 5;

    config::upnp_config_t cfg;
    asio::io_context::strand &strand;
    asio::io_context &io_context;
    asio::ip::tcp::resolver resolver;
    udp_socket_t udp_socket;
    tcp_socket_ptr_t tcp_socket;
    timer_t timer;
    std::uint32_t activities_flag;

    fmt::memory_buffer tx_buff;
    boost::beast::flat_buffer rx_buff;
    discovery_option_t discovery_option;
    utils::URI igd_url;
    utils::URI request_url;
    http_response_option_t response_option;
    callback_option_t callback_option;
};

} // namespace net
} // namespace syncspirit