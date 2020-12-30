#pragma once

#include "mdbx.h"
#include <functional>
#include <boost/outcome.hpp>

namespace syncspirit {
namespace db {

namespace outcome = boost::outcome_v2;

struct transaction_t;

using tx_fn_t = std::function<bool(transaction_t &)>;

enum class transaction_type_t { RO, RW };

struct transaction_t {
    transaction_t() noexcept : txn{nullptr} {};
    transaction_t(transaction_t &&other) noexcept;
    ~transaction_t();

    transaction_t &operator=(transaction_t &&other) noexcept;

    outcome::result<void> commit() noexcept;

    MDBX_txn *txn;
    MDBX_dbi dbi;
    transaction_type_t type;

    static outcome::result<transaction_t> make(transaction_type_t type, MDBX_env *env_) noexcept;

  private:
    transaction_t(transaction_type_t type_, MDBX_txn *txn_, MDBX_dbi dbi_) noexcept
        : txn{txn_}, dbi{dbi_}, type{type_} {}
};

inline outcome::result<transaction_t> make_transaction(transaction_type_t type, MDBX_env *env_) noexcept {
    return transaction_t::make(type, env_);
}

} // namespace db
} // namespace syncspirit