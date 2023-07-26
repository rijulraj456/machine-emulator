// Copyright Cartesi and individual authors (see AUTHORS)
// SPDX-License-Identifier: LGPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Lesser General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
// PARTICULAR PURPOSE. See the GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License along
// with this program (see COPYING). If not, see <https://www.gnu.org/licenses/>.
//

#ifndef UARCH_REPLAY_MACHINE_H
#define UARCH_REPLAY_MACHINE_H

/// \file
/// \brief State access implementation that replays recorded state accesses

#include <boost/container/static_vector.hpp>
#include <cassert>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>

#include "i-uarch-state-access.h"
#include "shadow-state.h"
#include "uarch-bridge.h"

namespace cartesi {

class uarch_replay_state_access : public i_uarch_state_access<uarch_replay_state_access> {
    ///< Access log generated by step
    const std::vector<access> &m_accesses;
    ///< Whether to verify proofs in access log
    bool m_verify_proofs;
    ///< Next access
    unsigned m_next_access;
    ///< Add to indices reported in errors
    int m_one_based;
    ///< Root hash before next access
    machine_merkle_tree::hash_type m_root_hash;
    ///< Hasher needed to verify proofs
    machine_merkle_tree::hasher_type m_hasher;
    ///< Local storage for mock pma entries reconstructed from accesses
    boost::container::static_vector<pma_entry, PMA_MAX> m_mock_pmas;

public:
    /// \brief Constructor from log of word accesses.
    /// \param accesses Reference to word access vector.
    explicit uarch_replay_state_access(const access_log &log, bool verify_proofs, bool one_based) :
        m_accesses(log.get_accesses()),
        m_verify_proofs(verify_proofs),
        m_next_access{0},
        m_one_based{one_based},
        m_root_hash{},
        m_hasher{},
        m_mock_pmas{} {
        if (m_verify_proofs && !log.get_log_type().has_proofs()) {
            throw std::invalid_argument{"log has no proofs"};
        }
        if (!m_accesses.empty() && m_verify_proofs) {
            const auto &access = m_accesses.front();
            if (!access.get_proof().has_value()) {
                throw std::invalid_argument{"initial access has no proof"};
            }
            m_root_hash = access.get_proof().value().get_root_hash();
        }
    }

    /// \brief No copy constructor
    uarch_replay_state_access(const uarch_replay_state_access &) = delete;
    /// \brief No copy assignment
    uarch_replay_state_access &operator=(const uarch_replay_state_access &) = delete;
    /// \brief No move constructor
    uarch_replay_state_access(uarch_replay_state_access &&) = delete;
    /// \brief No move assignment
    uarch_replay_state_access &operator=(uarch_replay_state_access &&) = delete;
    /// \brief Default destructor
    ~uarch_replay_state_access() = default;

    void finish(void) {
        if (m_next_access != m_accesses.size()) {
            throw std::invalid_argument{"too many word accesses in log"};
        }
    }

    void get_root_hash(machine_merkle_tree::hash_type &hash) const {
        hash = m_root_hash;
    }

private:
    auto access_to_report(void) const {
        return m_next_access + m_one_based;
    }

    static void roll_hash_up_tree(machine_merkle_tree::hasher_type &hasher,
        const machine_merkle_tree::proof_type &proof, machine_merkle_tree::hash_type &rolling_hash) {
        for (int log2_size = proof.get_log2_target_size(); log2_size < proof.get_log2_root_size(); ++log2_size) {
            int bit = (proof.get_target_address() & (UINT64_C(1) << log2_size)) != 0;
            const auto &sibling_hash = proof.get_sibling_hash(log2_size);
            hasher.begin();
            if (bit) {
                hasher.add_data(sibling_hash.data(), sibling_hash.size());
                hasher.add_data(rolling_hash.data(), rolling_hash.size());
            } else {
                hasher.add_data(rolling_hash.data(), rolling_hash.size());
                hasher.add_data(sibling_hash.data(), sibling_hash.size());
            }
            hasher.end(rolling_hash);
        }
    }

    static void get_hash(machine_merkle_tree::hasher_type &hasher, const unsigned char *data, size_t len,
        machine_merkle_tree::hash_type &hash) {
        if (len <= 8) {
            assert(len == 8);
            hasher.begin();
            hasher.add_data(data, len);
            hasher.end(hash);
        } else {
            assert((len & 1) == 0);
            len = len / 2;
            machine_merkle_tree::hash_type left;
            get_hash(hasher, data, len, left);
            get_hash(hasher, data + len, len, hash);
            hasher.begin();
            hasher.add_data(left.data(), left.size());
            hasher.add_data(hash.data(), hash.size());
            hasher.end(hash);
        }
    }

    static void get_hash(machine_merkle_tree::hasher_type &hasher, const access_data &data,
        machine_merkle_tree::hash_type &hash) {
        get_hash(hasher, data.data(), data.size(), hash);
    }

    /// \brief Checks a logged word read and advances log.
    /// \param paligned Physical address in the machine state,
    /// aligned to 64-bits.
    /// \param text Textual description of the access.
    /// \returns Value read.
    uint64_t check_read_word(uint64_t paligned, const char *text) {
        return get_word_access_data(check_read(paligned, 3, text));
    }

    /// \brief Checks a logged read and advances log.
    /// \param paligned Physical address in the machine state,
    /// aligned to the access size.
    /// \param log2_size Log2 of access size.
    /// \param text Textual description of the access.
    /// \returns Value read.
    const access_data &check_read(uint64_t paligned, int log2_size, const char *text) {
        if (log2_size < 3 || log2_size > 63) {
            throw std::invalid_argument{"invalid access size"};
        }
        if ((paligned & ((UINT64_C(1) << log2_size) - 1)) != 0) {
            throw std::invalid_argument{"access address not aligned to size"};
        }
        if (m_next_access >= m_accesses.size()) {
            throw std::invalid_argument{"too few accesses in log"};
        }
        const auto &access = m_accesses[m_next_access];
        if (access.get_type() != access_type::read) {
            throw std::invalid_argument{"expected access " + std::to_string(access_to_report()) + " to read " + text};
        }
        if (access.get_log2_size() != log2_size) {
            throw std::invalid_argument{"expected access " + std::to_string(access_to_report()) + " to read 2^" +
                std::to_string(log2_size) + " bytes from " + text};
        }
        if (access.get_read().size() != UINT64_C(1) << log2_size) {
            throw std::invalid_argument{"expected read access data" + std::to_string(access_to_report()) +
                " to contain 2^" + std::to_string(log2_size) + " bytes"};
        }
        if (access.get_address() != paligned) {
            std::ostringstream err;
            err << "expected access " << access_to_report() << " to read " << text << " at address 0x" << std::hex
                << paligned << "(" << std::dec << paligned << ")";
            throw std::invalid_argument{err.str()};
        }
        if (m_verify_proofs) {
            if (!access.get_proof().has_value()) {
                throw std::invalid_argument{"read access " + std::to_string(access_to_report()) + " has no proof"};
            }
            const auto &proof = access.get_proof().value();
            if (proof.get_target_address() != access.get_address()) {
                throw std::invalid_argument{
                    "mismatch in read access " + std::to_string(access_to_report()) + " address and its proof address"};
            }
            if (m_root_hash != proof.get_root_hash()) {
                throw std::invalid_argument{
                    "mismatch in read access " + std::to_string(access_to_report()) + " root hash"};
            }
            machine_merkle_tree::hash_type rolling_hash;
            get_hash(m_hasher, access.get_read(), rolling_hash);
            if (rolling_hash != proof.get_target_hash()) {
                throw std::invalid_argument{
                    "value in read access " + std::to_string(access_to_report()) + " does not match target hash"};
            }
            roll_hash_up_tree(m_hasher, proof, rolling_hash);
            if (rolling_hash != proof.get_root_hash()) {
                throw std::invalid_argument{
                    "word value in read access " + std::to_string(access_to_report()) + " fails proof"};
            }
        }
        m_next_access++;
        return access.get_read();
    }

    /// \brief Checks a logged word write and advances log.
    /// \param paligned Physical address in the machine state,
    /// aligned to a 64-bit word.
    /// \param word Word value to write.
    /// \param text Textual description of the access.
    /// \returns Value read.
    void check_write_word(uint64_t paligned, uint64_t word, const char *text) {
        access_data val;
        set_word_access_data(word, val);
        check_write(paligned, val, 3, text);
    }

    /// \brief Checks a logged write and advances log.
    /// \param paligned Physical address in the machine state,
    /// aligned to the access size.
    /// \param val Value to write.
    /// \param log2_size Log2 of access size.
    /// \param text Textual description of the access.
    void check_write(uint64_t paligned, const access_data &val, int log2_size, const char *text) {
        if (log2_size < 3 || log2_size > 63) {
            throw std::invalid_argument{"invalid access size"};
        }
        if ((paligned & ((UINT64_C(1) << log2_size) - 1)) != 0) {
            throw std::invalid_argument{"access address not aligned to size"};
        }
        if (m_next_access >= m_accesses.size()) {
            throw std::invalid_argument{"too few word accesses in log"};
        }
        const auto &access = m_accesses[m_next_access];
        if (access.get_type() != access_type::write) {
            throw std::invalid_argument{"expected access " + std::to_string(access_to_report()) + " to write " + text};
        }
        if (access.get_log2_size() != log2_size) {
            throw std::invalid_argument{"expected access " + std::to_string(access_to_report()) + " to write 2^" +
                std::to_string(log2_size) + " bytes from " + text};
        }
        if (access.get_read().size() != UINT64_C(1) << log2_size) {
            throw std::invalid_argument{"expected overwritten data" + std::to_string(access_to_report()) +
                " to contain 2^" + std::to_string(log2_size) + " bytes"};
        }
        if (access.get_written().size() != UINT64_C(1) << log2_size) {
            throw std::invalid_argument{"expected written data" + std::to_string(access_to_report()) +
                " to contain 2^" + std::to_string(log2_size) + " bytes"};
        }
        if (access.get_address() != paligned) {
            std::ostringstream err;
            err << "expected access " << access_to_report() << " to write " << text << " at address 0x" << std::hex
                << paligned << "(" << std::dec << paligned << ")";
            throw std::invalid_argument{err.str()};
        }
        if (m_verify_proofs) {
            if (!access.get_proof().has_value()) {
                throw std::invalid_argument{"write access " + std::to_string(access_to_report()) + " has no proof"};
            }
            const auto &proof = access.get_proof().value();
            if (proof.get_target_address() != access.get_address()) {
                throw std::invalid_argument{"mismatch in write access " + std::to_string(access_to_report()) +
                    " address and its proof address"};
            }
            if (m_root_hash != proof.get_root_hash()) {
                throw std::invalid_argument{
                    "mismatch in write access " + std::to_string(access_to_report()) + " root hash"};
            }
            machine_merkle_tree::hash_type rolling_hash;
            get_hash(m_hasher, access.get_read(), rolling_hash);
            if (rolling_hash != proof.get_target_hash()) {
                throw std::invalid_argument{
                    "value before write access " + std::to_string(access_to_report()) + " does not match target hash"};
            }
            roll_hash_up_tree(m_hasher, proof, rolling_hash);
            if (rolling_hash != proof.get_root_hash()) {
                throw std::invalid_argument{
                    "value before write access " + std::to_string(access_to_report()) + " fails proof"};
            }
            if (access.get_written() != val) {
                throw std::invalid_argument{
                    "value written in access " + std::to_string(access_to_report()) + " does not match log"};
            }
            get_hash(m_hasher, access.get_written(), m_root_hash);
            roll_hash_up_tree(m_hasher, proof, m_root_hash);
        }
        m_next_access++;
    }

    friend i_uarch_state_access<uarch_replay_state_access>;

    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    void do_push_bracket(bracket_type type, const char *text) {
        (void) type;
        (void) text;
    }

    int do_make_scoped_note(const char *text) { // NOLINT(readability-convert-member-functions-to-static)
        (void) text;
        return 0;
    }

    uint64_t do_read_x(int reg) {
        return check_read_word(shadow_state_get_uarch_x_abs_addr(reg), "uarch.x");
    }

    void do_write_x(int reg, uint64_t val) {
        assert(reg != 0);
        check_write_word(shadow_state_get_uarch_x_abs_addr(reg), val, "uarch.x");
    }

    uint64_t do_read_pc() {
        return check_read_word(shadow_state_get_csr_abs_addr(shadow_state_csr::uarch_pc), "uarch.pc");
    }

    void do_write_pc(uint64_t val) {
        check_write_word(shadow_state_get_csr_abs_addr(shadow_state_csr::uarch_pc), val, "uarch.pc");
    }

    uint64_t do_read_cycle() {
        return check_read_word(shadow_state_get_csr_abs_addr(shadow_state_csr::uarch_cycle), "uarch.uarch_cycle");
    }

    void do_write_cycle(uint64_t val) {
        check_write_word(shadow_state_get_csr_abs_addr(shadow_state_csr::uarch_cycle), val, "uarch.cycle");
    }

    bool do_read_halt_flag() {
        return check_read_word(shadow_state_get_csr_abs_addr(shadow_state_csr::uarch_halt_flag), "uarch.halt_flag");
    }

    void do_set_halt_flag() {
        check_write_word(shadow_state_get_csr_abs_addr(shadow_state_csr::uarch_halt_flag), true, "uarch.halt_flag");
    }

    void do_reset_halt_flag() {
        check_write_word(shadow_state_get_csr_abs_addr(shadow_state_csr::uarch_halt_flag), false, "uarch.halt_flag");
    }

    uint64_t do_read_word(uint64_t paddr) {
        assert((paddr & (sizeof(uint64_t) - 1)) == 0);
        // Get the name of the state register identified by this address
        const auto *name = uarch_bridge::get_register_name(paddr);
        if (!name) {
            // this is a regular memory access
            name = "memory";
        }
        return check_read_word(paddr, name);
    }

    void do_write_word(uint64_t paddr, uint64_t data) {
        assert((paddr & (sizeof(uint64_t) - 1)) == 0);
        // Get the name of the state register identified by this address
        const auto *name = uarch_bridge::get_register_name(paddr);
        if (!name) {
            // this is a regular memory access
            name = "memory";
        }
        check_write_word(paddr, data, name);
    }
};

} // namespace cartesi

#endif
