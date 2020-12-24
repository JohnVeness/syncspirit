#include "transaction.h"
#include "error_code.h"
#include "prefix.h"
#include <boost/endian/conversion.hpp>

namespace syncspirit::db {

std::uint32_t version{1};

namespace be = boost::endian;

namespace misc {
static const constexpr std::string_view db_version = "db_version";
}

outcome::result<uint32_t> get_version(transaction_t &txn) noexcept {
    auto key = prefixer_t<prefix::misc>::make(misc::db_version);
    MDBX_val value;
    auto r = mdbx_get(txn.txn, txn.dbi, key, &value);
    if (r != MDBX_SUCCESS) {
        if (r == MDBX_NOTFOUND) {
            return outcome::success(0);
        }
        return make_error_code(r);
    }

    if (value.iov_len != sizeof(std::uint32_t)) {
        return make_error_code(error_code::db_version_size_mismatch);
    }

    std::uint32_t version;
    memcpy(&version, value.iov_base, sizeof(std::uint32_t));
    be::big_to_native_inplace(version);
    return version;
}

static outcome::result<void> migrate0(transaction_t &txn) noexcept {
    auto key = prefixer_t<prefix::misc>::make(misc::db_version);
    MDBX_val value;
    auto db_ver = be::native_to_big(version);
    value.iov_base = &db_ver;
    value.iov_len = sizeof(db_ver);
    auto r = mdbx_put(txn.txn, txn.dbi, key, &value, MDBX_UPSERT);
    if (r != MDBX_SUCCESS) {
        return make_error_code(r);
    }
    return outcome::success();
}

static outcome::result<void> do_migrate(uint32_t from, transaction_t &txn) noexcept {
    switch (from) {
    case 0:
        return migrate0(txn);
    default:
        assert(0 && "impossibe migration to future version");
        std::terminate();
    }
}

outcome::result<void> migrate(uint32_t from, transaction_t &txn) noexcept {
    while (from != version) {
        auto r = do_migrate(from, txn);
        if (!r)
            return r;
        r = txn.commit();
        if (!r)
            return r;
        ++from;
    }
    return outcome::success();
}

} // namespace syncspirit::db
