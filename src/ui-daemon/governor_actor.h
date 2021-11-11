#pragma once

#include <rotor.hpp>
#include "command.h"
#include "../net/messages.h"
#include "../ui/messages.hpp"
#include "../utils/log.h"
#include "../model/cluster.h"
#include "model/diff/diff_visitor.h"

namespace syncspirit::daemon {

namespace r = rotor;

struct governor_actor_config_t : r::actor_config_t {
    Commands commands;
};

template <typename Actor> struct governor_actor_config_builder_t : r::actor_config_builder_t<Actor> {
    using builder_t = typename Actor::template config_builder_t<Actor>;
    using parent_t = r::actor_config_builder_t<Actor>;
    using parent_t::parent_t;

    builder_t &&commands(Commands &&value) &&noexcept {
        parent_t::config.commands = std::move(value);
        return std::move(*static_cast<typename parent_t::builder_t *>(this));
    }
};

struct governor_actor_t : public r::actor_base_t, private model::diff::diff_visitor_t {
    using config_t = governor_actor_config_t;
    template <typename Actor> using config_builder_t = governor_actor_config_builder_t<Actor>;

    explicit governor_actor_t(config_t &cfg);

    void configure(r::plugin::plugin_base_t &plugin) noexcept override;
    void on_start() noexcept override;
    void shutdown_start() noexcept override;

    r::address_ptr_t coordinator;
    model::cluster_ptr_t cluster;
    Commands commands;
    utils::logger_t log;

  private:
    void process() noexcept;
    void on_model_update(net::message::model_update_t &message) noexcept;
    void on_model_response(net::message::model_response_t& reply) noexcept;

    outcome::result<void> operator()(const model::diff::load::load_cluster_t &) noexcept override;
};

} // namespace syncspirit::daemon
