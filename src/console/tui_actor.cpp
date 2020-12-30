#include "tui_actor.h"
#include "sink.h"
#include "utils.h"
#include <spdlog/spdlog.h>
#include "../net/names.h"
#include "config_activity.h"
#include "default_activity.h"
#include "local_peer_activity.h"

using namespace syncspirit::console;

namespace {
namespace resource {
r::plugin::resource_id_t tty = 0;
}
} // namespace

const char *tui_actor_t::progress = "|/-\\";

tui_actor_t::tui_actor_t(config_t &cfg)
    : r::actor_base_t{cfg}, strand{static_cast<ra::supervisor_asio_t *>(cfg.supervisor)->get_strand()},
      mutex{cfg.mutex}, prompt{cfg.prompt}, tui_config{cfg.tui_config} {
    if (!console::install_signal_handlers()) {
        spdlog::critical("signal handlers cannot be installed");
        throw std::runtime_error("signal handlers cannot be installed");
    }

    progress_last = strlen(progress);
    tty = std::make_unique<tty_t::element_type>(strand.context(), STDIN_FILENO);
    push_activity(std::make_unique<default_activity_t>(*this, activity_type_t::DEFAULT));
}

void tui_actor_t::on_start() noexcept {
    spdlog::debug("tui_actor_t::on_start (addr = {})", (void *)address.get());
    r::actor_base_t::on_start();
    start_timer();
    do_read();
}

void tui_actor_t::shutdown_start() noexcept {
    spdlog::debug("tui_actor_t::shutdown_start (addr = {})", (void *)address.get());
    r::actor_base_t::shutdown_start();
    supervisor->do_shutdown();
    if (coordinator) {
        send<r::payload::shutdown_trigger_t>(coordinator, coordinator);
    }
    if (resources->has(resource::tty)) {
        tty->cancel();
    }
}

void tui_actor_t::configure(r::plugin::plugin_base_t &plugin) noexcept {
    r::actor_base_t::configure(plugin);
    plugin.with_casted<r::plugin::registry_plugin_t>([&](auto &p) {
        p.discover_name(net::names::coordinator, coordinator, true).link().callback([&](auto phase, auto &ec) {
            if (!ec && phase == r::plugin::registry_plugin_t::phase_t::linking) {
                auto p = get_plugin(r::plugin::starter_plugin_t::class_identity);
                auto plugin = static_cast<r::plugin::starter_plugin_t *>(p);
                plugin->subscribe_actor(&tui_actor_t::on_discovery, coordinator);
                auto timeout = init_timeout / 2;
                request<ui::payload::config_request_t>(coordinator).send(timeout);
            }
        });
    });
    plugin.with_casted<r::plugin::starter_plugin_t>([&](auto &p) {
        p.subscribe_actor(&tui_actor_t::on_config);
        p.subscribe_actor(&tui_actor_t::on_config_save);
    });
}

void tui_actor_t::start_timer() noexcept {
    auto interval = r::pt::milliseconds{tui_config.refresh_interval};
    timer_id = r::actor_base_t::start_timer(interval, *this, &tui_actor_t::on_timer);
}

void tui_actor_t::do_read() noexcept {
    if (state < r::state_t::SHUTTING_DOWN) {
        resources->acquire(resource::tty);
        auto fwd = ra::forwarder_t(*this, &tui_actor_t::on_read, &tui_actor_t::on_read_error);
        asio::mutable_buffer buff(input, 1);
        asio::async_read(*tty, buff, std::move(fwd));
    }
}

void tui_actor_t::on_read(size_t) noexcept {
    resources->release(resource::tty);
    input[1] = 0;
    auto k = input[0];
    if (!activities.front()->handle(k)) {
        if (k == tui_config.key_quit) {
            action_quit();
        } else if (k == tui_config.key_more_logs) {
            action_more_logs();
        } else if (k == tui_config.key_less_logs) {
            action_less_logs();
        } else if (k == tui_config.key_config) {
            action_config();
        } else if (k == 27) { /* escape */
            action_esc();
        }
    }
    do_read();
}

void tui_actor_t::on_read_error(const sys::error_code &ec) noexcept {
    resources->release(resource::tty);
    if (ec != asio::error::operation_aborted) {
        spdlog::error("tui_actor_t::on_read_error, stdin reading error :: {}", ec.message());
        do_shutdown();
    }
}

void tui_actor_t::on_timer(r::request_id_t, bool) noexcept {
    if (console::shutdown_flag) {
        return do_shutdown();
    }
    if (console::reset_term_flag) {
        console::term_prepare();
        tty->non_blocking(true);
        console::reset_term_flag = false;
    }

    flush_prompt();
    start_timer();
}

void tui_actor_t::set_prompt(const std::string &value) noexcept {
    prompt_buff = value;
    flush_prompt();
}

void tui_actor_t::push_activity(activity_ptr_t &&activity) noexcept {
    auto predicate = [&](auto &it) { return *it == *activity; };
    auto count = std::count_if(activities.begin(), activities.end(), predicate);
    if (count == 0) {
        ++activities_count;
        activities.push_front(std::move(activity));
        activities.front()->display();
    }
}

void tui_actor_t::postpone_activity() noexcept {
    if (activities_count > 1) {
        auto a = std::move(activities.front());
        activities.pop_front();
        activities.emplace_back(std::move(a));
        activities.front()->display();
    }
}

void tui_actor_t::discard_activity() noexcept {
    --activities_count;
    activities.pop_front();
    activities.front()->display();
}

void tui_actor_t::action_quit() noexcept {
    spdlog::info("tui_actor_t::action_quit");
    console::shutdown_flag = true;
}

void tui_actor_t::action_more_logs() noexcept {
    auto level = spdlog::default_logger_raw()->level();
    auto l = static_cast<int>(level);
    if (l > 0) {
        spdlog::set_level(static_cast<decltype(level)>(--l));
        spdlog::info("tui_actor_t::action_more_logs, applied ({})", l);
    }
}

void tui_actor_t::action_less_logs() noexcept {
    auto level = spdlog::default_logger_raw()->level();
    auto l = static_cast<int>(level);
    auto m = static_cast<int>(decltype(level)::critical);
    if (l < m) {
        spdlog::info("tui_actor_t::action_less_logs, applied ({})", l);
        spdlog::set_level(static_cast<decltype(level)>(++l));
    }
}

void tui_actor_t::action_esc() noexcept { activities.front()->forget(); }

void tui_actor_t::action_config() noexcept {
    push_activity(std::make_unique<config_activity_t>(*this, activity_type_t::CONFIG, app_config, app_config_orig));
}

void tui_actor_t::save_config() noexcept {
    auto timeout = init_timeout / 2;
    request<ui::payload::config_save_request_t>(coordinator, app_config).send(timeout);
}

void tui_actor_t::on_discovery(ui::message::discovery_notify_t &message) noexcept {
    push_activity(std::make_unique<local_peer_activity_t>(*this, activity_type_t::LOCAL_PEER, message));
}

void tui_actor_t::on_config(ui::message::config_response_t &message) noexcept {
    app_config_orig = app_config = message.payload.res;
}

void tui_actor_t::on_config_save(ui::message::config_save_response_t &message) noexcept {
    auto &ec = message.payload.ec;
    if (ec) {
        spdlog::error("tui_actor_t, cannot save config: {}", ec.message());
        return;
    }
    spdlog::trace("tui_actor_t::on_config_save");
    app_config_orig = app_config;
}

void tui_actor_t::ignore_device(const model::device_id_t &device_id) noexcept {
    app_config.ingored_devices.emplace(device_id.get_value());
    save_config();
}

void tui_actor_t::flush_prompt() noexcept {
    char c;
    if (progress_idx < progress_last) {
        c = progress[progress_idx];
        ++progress_idx;
    } else {
        c = progress[progress_idx = 0];
    }
    auto r = fmt::format("\r\033[2K[{}{}{}{}] {}", sink_t::bold, sink_t::cyan, std::string_view(&c, 1), sink_t::reset,
                         prompt_buff);
    std::lock_guard<std::mutex> lock(*mutex);
    *prompt = r;
    fwrite(prompt->data(), sizeof(char), prompt->size(), stdout);
    fflush(stdout);
}