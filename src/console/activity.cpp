#include "activity.h"
#include "tui_actor.h"

using namespace syncspirit::console;

void activity_t::forget() noexcept { actor.postpone_activity(); }

bool activity_t::operator==(const activity_t &other) const noexcept { return type == other.type; }