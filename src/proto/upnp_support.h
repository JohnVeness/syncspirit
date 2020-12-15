#pragma once
#include "../utils/uri.h"
#include "../model/upnp.h"
#include <fmt/fmt.h>
#include <string>
#include <vector>
#include <boost/outcome.hpp>

namespace syncspirit::proto {

namespace outcome = boost::outcome_v2;

struct upnp_fields {
    static const char *st;
    static const char *man;
    static const char *mx;
    static const char *usn;
};

extern const char *upnp_addr;
constexpr std::uint16_t upnp_port = 1900;

outcome::result<model::discovery_result> parse(const char *data, std::size_t bytes) noexcept;

outcome::result<void> make_discovery_request(fmt::memory_buffer &buff, std::uint32_t max_wait) noexcept;

outcome::result<void> make_description_request(fmt::memory_buffer &buff, const utils::URI &uri) noexcept;

outcome::result<model::igd_result> parse_igd(const char *data, std::size_t bytes) noexcept;

outcome::result<void> make_external_ip_request(fmt::memory_buffer &buff, const utils::URI &uri) noexcept;

outcome::result<std::string> parse_external_ip(const char *data, std::size_t bytes) noexcept;

outcome::result<void> make_mapping_request(fmt::memory_buffer &buff, const utils::URI &uri, std::uint16_t external_port,
                                           const std::string &internal_ip, std::uint16_t internal_port) noexcept;

outcome::result<void> make_unmapping_request(fmt::memory_buffer &buff, const utils::URI &uri,
                                             std::uint16_t external_port) noexcept;

outcome::result<bool> parse_mapping(const char *data, std::size_t bytes) noexcept;

outcome::result<bool> parse_unmapping(const char *data, std::size_t bytes) noexcept;

} // namespace syncspirit::proto
