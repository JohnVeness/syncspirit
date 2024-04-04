// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2019-2022 Ivan Baidakou

#include "platform.h"
#include <stdexcept>

using namespace syncspirit::utils;

void platform_t::startup() {
#if defined(WIN32) || defined(_WIN32) || defined(__WIN32)
    auto wVersionRequested = MAKEWORD(2, 2);
    WSADATA wsaData;

    auto err = WSAStartup(wVersionRequested, &wsaData);
    if (err != 0) {
        std::string msg = "WSAStartup failed with error: ";
        msg += std::to_string(err);
        throw std::runtime_error(msg);
    }
#endif
}

void platform_t::shutdhown() noexcept {}
