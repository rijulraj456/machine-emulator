// Copyright 2019 Cartesi Pte. Ltd.
//
// This file is part of the machine-emulator. The machine-emulator is free
// software: you can redistribute it and/or modify it under the terms of the GNU
// Lesser General Public License as published by the Free Software Foundation,
// either version 3 of the License, or (at your option) any later version.
//
// The machine-emulator is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License
// for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with the machine-emulator. If not, see http://www.gnu.org/licenses/.
//

#ifndef STEP_STATE_ACCESS_H
#define STEP_STATE_ACCESS_H

/// \file
/// \brief State access implementation that logs all accesses

#include <cassert>
#include <string>
#include <iomanip>
#include <sstream>
#include <memory>
#include <boost/container/static_vector.hpp>

#ifdef DUMP_HIST
#include <unordered_map>
#endif

#include "i-state-access.h"
#include "machine.h"
#include "shadow.h"
#include "clint.h"
#include "htif.h"
#include "access-log.h"
#include "machine-merkle-tree.h"
#include "pma.h"
#include "strict-aliasing.h"

namespace cartesi {

/// \details The step_state_access logs all access to the machine state.
class step_state_access: public i_state_access<step_state_access> {
public:

    class mock_machine_state {
    public:

        mock_machine_state(void) { }

        void set_brk(void) { }
        bool get_brk(void) const { return true; }
        void or_brk_with_mip_mie(void) { }
        void or_brk_with_iflags_H(void) { }
        void or_brk_with_iflags_Y(void) { }
        void set_brk_from_all(void) { }
        void assert_no_brk(void) { }

#ifdef DUMP_COUNTERS
        machine_statistics stats;
#endif

#ifdef DUMP_HIST
    std::unordered_map<std::string, uint64_t> insn_hist;
#endif
    };

private:

    ///< Access log generated by step
    const std::vector<access> &m_accesses;
    ///< Whether to verify proofs in access log
    bool m_verify_proofs;
    ///< Next access
    unsigned m_next_access;
    ///< Dehash source to use
    i_dhd_source_ptr m_source;
    ///< Add to indices reported in errors
    int m_one_based;
    ///< Root hash before next access
    machine_merkle_tree::hash_type m_root_hash;
    ///< Hasher needed to verify proofs
    machine_merkle_tree::hasher_type m_hasher;
    ///< Local storage for mock pma entries reconstructed from accesses
    boost::container::static_vector<pma_entry, PMA_MAX> m_mock_pmas;

    ///< Mock machine state
    mock_machine_state m_naked_state;

public:

    /// \brief Constructor from log of word accesses.
    /// \param accesses Reference to word access vector.
    step_state_access(const access_log &log, bool verify_proofs,
        i_dhd_source_ptr source, bool one_based):
        m_accesses(log.get_accesses()),
        m_verify_proofs(verify_proofs),
        m_next_access{0},
        m_source{source},
        m_one_based{one_based} {
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
    step_state_access(const step_state_access &) = delete;
    /// \brief No move constructor
    step_state_access(step_state_access &&) = delete;
    /// \brief No copy assignment
    step_state_access& operator=(const step_state_access &) = delete;
    /// \brief No move assignment
    step_state_access& operator=(step_state_access &&) = delete;

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

    const auto &do_get_naked_state(void) const {
        return m_naked_state;
    }

    auto &do_get_naked_state(void) {
        return m_naked_state;
    }

    static void roll_hash_up_tree(machine_merkle_tree::hasher_type &hasher,
        const machine_merkle_tree::proof_type &proof,
        machine_merkle_tree::hash_type &rolling_hash) {
        for (int log2_size = proof.get_log2_target_size();
            log2_size < proof.get_log2_root_size();
            ++log2_size) {
           int bit = (proof.get_target_address() & (UINT64_C(1) << log2_size))
               != 0;
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

    static void get_hash(machine_merkle_tree::hasher_type &hasher,
        const unsigned char *data, size_t len, machine_merkle_tree::hash_type &hash) {
        if (len <= 8) {
            assert(len == 8);
            hasher.begin();
            hasher.add_data(data, len);
            hasher.end(hash);
        } else {
            assert((len & 1) == 0);
            len = len/2;
            machine_merkle_tree::hash_type left;
            get_hash(hasher, data, len, left);
            get_hash(hasher, data+len, len, hash);
            hasher.begin();
            hasher.add_data(left.data(), left.size());
            hasher.add_data(hash.data(), hash.size());
            hasher.end(hash);
        }
    }

    static void get_hash(machine_merkle_tree::hasher_type &hasher,
        const access_data &data, machine_merkle_tree::hash_type &hash) {
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
    const access_data &check_read(uint64_t paligned, int log2_size,
        const char *text) {
        if (log2_size < 3 || log2_size > 63) {
            throw std::invalid_argument{"invalid access size"};
        }
        if ((paligned & ((UINT64_C(1) << log2_size)-1)) != 0) {
            throw std::invalid_argument{"access address not aligned to size"};
        }
        if (m_next_access >= m_accesses.size()) {
            throw std::invalid_argument{"too few accesses in log"};
        }
        const auto &access = m_accesses[m_next_access];
        if (access.get_type() != access_type::read) {
            throw std::invalid_argument{"expected access " +
                std::to_string(access_to_report()) +
                " to read " + text};
        }
        if (access.get_log2_size() != log2_size) {
            throw std::invalid_argument{"expected access " +
                std::to_string(access_to_report()) + " to read 2^" +
                std::to_string(log2_size) + " bytes from " + text};
        }
        if (access.get_read().size() != UINT64_C(1) << log2_size) {
            throw std::invalid_argument{"expected read access data" +
                std::to_string(access_to_report()) + " to contain 2^" +
                std::to_string(log2_size) + " bytes"};
        }
        if (access.get_address() != paligned) {
            std::ostringstream err;
            err << "expected access " << access_to_report() <<
                " to read " << text << " at address 0x" << std::hex <<
                paligned << "(" << std::dec << paligned << ")";
            throw std::invalid_argument{err.str()};
        }
        if (m_verify_proofs) {
            if (!access.get_proof().has_value()) {
                throw std::invalid_argument{"read access " +
                    std::to_string(access_to_report()) +
                    " has no proof"};
            }
            const auto &proof = access.get_proof().value();
            if (proof.get_target_address() != access.get_address()) {
                throw std::invalid_argument{"mismatch in read access " +
                    std::to_string(access_to_report()) +
                    " address and its proof address"};
            }
            if (m_root_hash != proof.get_root_hash()) {
                throw std::invalid_argument{"mismatch in read access " +
                    std::to_string(access_to_report()) + " root hash"};
            }
            machine_merkle_tree::hash_type rolling_hash;
            get_hash(m_hasher, access.get_read(), rolling_hash);
            if (rolling_hash != proof.get_target_hash()) {
                throw std::invalid_argument{"value in read access " +
                    std::to_string(access_to_report()) +
                    " does not match target hash"};
            }
            roll_hash_up_tree(m_hasher, proof, rolling_hash);
            if (rolling_hash != proof.get_root_hash()) {
                throw std::invalid_argument{"word value in read access " +
                    std::to_string(access_to_report()) + " fails proof"};
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
    void check_write(uint64_t paligned, const access_data &val,
        int log2_size, const char *text) {
        if (log2_size < 3 || log2_size > 63) {
            throw std::invalid_argument{"invalid access size"};
        }
        if ((paligned & ((UINT64_C(1) << log2_size)-1)) != 0) {
            throw std::invalid_argument{"access address not aligned to size"};
        }
        if (m_next_access >= m_accesses.size()) {
            throw std::invalid_argument{"too few word accesses in log"};
        }
        const auto &access = m_accesses[m_next_access];
        if (access.get_type() != access_type::write) {
            throw std::invalid_argument{"expected access " +
                std::to_string(access_to_report()) + " to write " + text};
        }
        if (access.get_log2_size() != log2_size) {
            throw std::invalid_argument{"expected access " +
                std::to_string(access_to_report()) + " to write 2^" +
                std::to_string(log2_size) + " bytes from " + text};
        }
        if (access.get_read().size() != UINT64_C(1) << log2_size) {
            throw std::invalid_argument{"expected overwritten data" +
                std::to_string(access_to_report()) + " to contain 2^" +
                std::to_string(log2_size) + " bytes"};
        }
        if (access.get_written().size() != UINT64_C(1) << log2_size) {
            throw std::invalid_argument{"expected written data" +
                std::to_string(access_to_report()) + " to contain 2^" +
                std::to_string(log2_size) + " bytes"};
        }
        if (access.get_address() != paligned) {
            std::ostringstream err;
            err << "expected access " << access_to_report() << " to write "
                << text << " at address 0x" << std::hex << paligned <<
                "(" << std::dec << paligned << ")";
            throw std::invalid_argument{err.str()};
        }
        if (m_verify_proofs) {
            if (!access.get_proof().has_value()) {
                throw std::invalid_argument{"write access " +
                    std::to_string(access_to_report()) +
                    " has no proof"};
            }
            const auto &proof = access.get_proof().value();
            if (proof.get_target_address() != access.get_address()) {
                throw std::invalid_argument{"mismatch in write access " +
                    std::to_string(access_to_report()) +
                    " address and its proof address"};
            }
            if (m_root_hash != proof.get_root_hash()) {
                throw std::invalid_argument{"mismatch in write access " +
                    std::to_string(access_to_report()) + " root hash"};
            }
            machine_merkle_tree::hash_type rolling_hash;
            get_hash(m_hasher, access.get_read(), rolling_hash);
            if (rolling_hash != proof.get_target_hash()) {
                throw std::invalid_argument{"value before write access " +
                    std::to_string(access_to_report()) +
                    " does not match target hash"};
            }
            roll_hash_up_tree(m_hasher, proof, rolling_hash);
            if (rolling_hash != proof.get_root_hash()) {
                throw std::invalid_argument{"value before write access " +
                    std::to_string(access_to_report()) + " fails proof"};
            }
            if (access.get_written() != val) {
                throw std::invalid_argument{"value written in access " +
                    std::to_string(access_to_report()) + " does not match log"};
            }
            get_hash(m_hasher, access.get_written(), m_root_hash);
            roll_hash_up_tree(m_hasher, proof, m_root_hash);
        }
        m_next_access++;
    }

    // Declare interface as friend to it can forward calls to the
    // "overriden" methods.
    friend i_state_access<step_state_access>;

    void do_push_bracket(bracket_type type, const char *text) {
        (void) type; (void) text;
    }

    int do_make_scoped_note(const char *text) {
        (void) text;
        return 0;
    }

    uint64_t do_read_x(int reg) {
        return check_read_word(PMA_SHADOW_START +
            shadow_get_x_rel_addr(reg), "x");
    }

    void do_write_x(int reg, uint64_t val) {
        assert(reg != 0);
        check_write_word(PMA_SHADOW_START +
            shadow_get_x_rel_addr(reg), val, "x");
    }

    uint64_t do_read_pc(void) {
        return check_read_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::pc), "pc");
    }

    void do_write_pc(uint64_t val) {
        check_write_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::pc), val, "pc");
    }

	uint64_t do_read_minstret(void) {
		return check_read_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::minstret), "minstret");
	}

	void do_write_minstret(uint64_t val) {
		check_write_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::minstret), val, "minstret");
	}

	uint64_t do_read_mvendorid(void) {
		return check_read_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::mvendorid), "mvendorid");
	}

	uint64_t do_read_marchid(void) {
		return check_read_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::marchid), "marchid");
	}

	uint64_t do_read_mimpid(void) {
		return check_read_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::mimpid), "mimpid");
	}

	uint64_t do_read_mcycle(void) {
		return check_read_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::mcycle), "mcycle");
	}

	void do_write_mcycle(uint64_t val) {
		check_write_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::mcycle), val, "mcycle");
	}

	uint64_t do_read_mstatus(void) {
        return check_read_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::mstatus), "mstatus");
	}

	void do_write_mstatus(uint64_t val) {
        check_write_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::mstatus), val, "mstatus");
	}

	uint64_t do_read_mtvec(void) {
		return check_read_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::mtvec), "mtvec");
	}

	void do_write_mtvec(uint64_t val) {
		check_write_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::mtvec), val, "mtvec");
	}

	uint64_t do_read_mscratch(void) {
		return check_read_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::mscratch), "mscratch");
	}

	void do_write_mscratch(uint64_t val) {
		check_write_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::mscratch), val, "mscratch");
	}

	uint64_t do_read_mepc(void) {
		return check_read_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::mepc), "mepc");
	}

	void do_write_mepc(uint64_t val) {
		check_write_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::mepc), val, "mepc");
	}

	uint64_t do_read_mcause(void) {
		return check_read_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::mcause), "mcause");
	}

	void do_write_mcause(uint64_t val) {
		check_write_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::mcause), val, "mcause");
	}

	uint64_t do_read_mtval(void) {
		return check_read_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::mtval), "mtval");
	}

	void do_write_mtval(uint64_t val) {
		check_write_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::mtval), val, "mtval");
	}

	uint64_t do_read_misa(void) {
		return check_read_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::misa), "misa");
	}

	void do_write_misa(uint64_t val) {
		check_write_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::misa), val, "misa");
	}

	uint64_t do_read_mie(void) {
		return check_read_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::mie), "mie");
	}

	void do_write_mie(uint64_t val) {
		check_write_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::mie), val, "mie");
	}

	uint64_t do_read_mip(void) {
		return check_read_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::mip), "mip");
	}

	void do_write_mip(uint64_t val) {
		check_write_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::mip), val, "mip");
	}

	uint64_t do_read_medeleg(void) {
		return check_read_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::medeleg), "medeleg");
	}

	void do_write_medeleg(uint64_t val) {
		check_write_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::medeleg), val, "medeleg");
	}

	uint64_t do_read_mideleg(void) {
		return check_read_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::mideleg), "mideleg");
	}

	void do_write_mideleg(uint64_t val) {
		check_write_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::mideleg), val, "mideleg");
	}

	uint64_t do_read_mcounteren(void) {
		return check_read_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::mcounteren), "mcounteren");
	}

	void do_write_mcounteren(uint64_t val) {
		check_write_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::mcounteren), val, "mcounteren");
	}

	uint64_t do_read_stvec(void) {
		return check_read_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::stvec), "stvec");
	}

	void do_write_stvec(uint64_t val) {
		check_write_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::stvec), val, "stvec");
	}

	uint64_t do_read_sscratch(void) {
		return check_read_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::sscratch), "sscratch");
	}

	void do_write_sscratch(uint64_t val) {
		check_write_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::sscratch), val, "sscratch");
	}

	uint64_t do_read_sepc(void) {
		return check_read_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::sepc), "sepc");
	}

	void do_write_sepc(uint64_t val) {
		check_write_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::sepc), val, "sepc");
	}

	uint64_t do_read_scause(void) {
		return check_read_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::scause), "scause");
	}

	void do_write_scause(uint64_t val) {
		check_write_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::scause), val, "scause");
	}

	uint64_t do_read_stval(void) {
		return check_read_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::stval), "stval");
	}

	void do_write_stval(uint64_t val) {
		check_write_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::stval), val, "stval");
	}

	uint64_t do_read_satp(void) {
		return check_read_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::satp), "satp");
	}

	void do_write_satp(uint64_t val) {
		check_write_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::satp), val, "satp");
	}

	uint64_t do_read_scounteren(void) {
		return check_read_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::scounteren), "scounteren");
	}

	void do_write_scounteren(uint64_t val) {
		check_write_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::scounteren), val, "scounteren");
	}

	uint64_t do_read_ilrsc(void) {
		return check_read_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::ilrsc), "ilrsc");
	}

	void do_write_ilrsc(uint64_t val) {
		check_write_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::ilrsc), val, "ilrsc");
	}

    void do_set_iflags_H(void) {
        uint64_t iflags_addr = PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::iflags);
        auto old_iflags = check_read_word(iflags_addr, "iflags.H (superfluous)");
        auto new_iflags = old_iflags | IFLAGS_H_MASK;
        check_write_word(iflags_addr, new_iflags, "iflags.H");
    }

    bool do_read_iflags_H(void) {
        auto iflags = check_read_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::iflags), "iflags.H");
        return (iflags & IFLAGS_H_MASK) != 0;
    }

    void do_set_iflags_Y(void) {
        uint64_t iflags_addr = PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::iflags);
        auto old_iflags = check_read_word(iflags_addr, "iflags.Y (superfluous)");
        auto new_iflags = old_iflags | IFLAGS_Y_MASK;
        check_write_word(iflags_addr, new_iflags, "iflags.Y");
    }

    void do_reset_iflags_Y(void) {
        uint64_t iflags_addr = PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::iflags);
        auto old_iflags = check_read_word(iflags_addr, "iflags.Y (superfluous)");
        auto new_iflags = old_iflags & (~IFLAGS_Y_MASK);
        check_write_word(iflags_addr, new_iflags, "iflags.Y");
    }

    bool do_read_iflags_Y(void) {
        auto iflags = check_read_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::iflags), "iflags.Y");
        return (iflags & IFLAGS_Y_MASK) != 0;
    }

    uint8_t do_read_iflags_PRV(void) {
        auto iflags = check_read_word(PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::iflags), "iflags.PRV");
        return (iflags & IFLAGS_PRV_MASK) >> IFLAGS_PRV_SHIFT;
    }

    void do_write_iflags_PRV(uint8_t val) {
        uint64_t iflags_addr = PMA_SHADOW_START +
            shadow_get_csr_rel_addr(shadow_csr::iflags);
        auto old_iflags = check_read_word(iflags_addr, "iflags.PRV (superfluous)");
        auto new_iflags = (old_iflags & (~IFLAGS_PRV_MASK)) |
            ((static_cast<uint64_t>(val) << IFLAGS_PRV_SHIFT) & IFLAGS_PRV_MASK);
        check_write_word(iflags_addr, new_iflags, "iflags.PRV");
    }

    uint64_t do_read_clint_mtimecmp(void) {
		return check_read_word(PMA_CLINT_START +
            clint_get_csr_rel_addr(clint_csr::mtimecmp), "clint.mtimecmp");
    }

    void do_write_clint_mtimecmp(uint64_t val) {
        check_write_word(PMA_CLINT_START +
            clint_get_csr_rel_addr(clint_csr::mtimecmp), val, "clint.mtimecmp");
    }

    uint64_t do_read_dhd_tstart(void) {
		return check_read_word(PMA_DHD_START +
            dhd_get_csr_rel_addr(dhd_csr::tstart), "dhd.tstart");
    }

    void do_write_dhd_tstart(uint64_t val) {
        check_write_word(PMA_DHD_START +
            dhd_get_csr_rel_addr(dhd_csr::tstart), val, "dhd.tstart");
    }

    uint64_t do_read_dhd_tlength(void) {
		return check_read_word(PMA_DHD_START +
            dhd_get_csr_rel_addr(dhd_csr::tlength), "dhd.tlength");
    }

    void do_write_dhd_tlength(uint64_t val) {
        check_write_word(PMA_DHD_START +
            dhd_get_csr_rel_addr(dhd_csr::tlength), val, "dhd.tlength");
    }

    uint64_t do_read_dhd_dlength(void) {
		return check_read_word(PMA_DHD_START +
            dhd_get_csr_rel_addr(dhd_csr::dlength), "dhd.dlength");
    }

    void do_write_dhd_dlength(uint64_t val) {
        check_write_word(PMA_DHD_START +
            dhd_get_csr_rel_addr(dhd_csr::dlength), val, "dhd.dlength");
    }

    uint64_t do_read_dhd_hlength(void) {
		return check_read_word(PMA_DHD_START +
            dhd_get_csr_rel_addr(dhd_csr::hlength), "dhd.hlength");
    }

    void do_write_dhd_hlength(uint64_t val) {
        check_write_word(PMA_DHD_START +
            dhd_get_csr_rel_addr(dhd_csr::hlength), val, "dhd.hlength");
    }

    uint64_t do_read_dhd_h(int i) {
        return check_read_word(PMA_DHD_START +
            dhd_get_h_rel_addr(i), "dhd.h");
    }

    void do_write_dhd_h(int i, uint64_t val) {
        check_write_word(PMA_DHD_START +
            dhd_get_h_rel_addr(i), val, "dhd.h");
    }

    dhd_data do_dehash(const unsigned char* hash, uint64_t hlength,
        uint64_t &dlength) {
        if (!m_source) throw std::runtime_error("no dhd source");
        return m_source->dehash(hash, hlength, dlength);
    }

    uint64_t do_read_htif_fromhost(void) {
        return check_read_word(PMA_HTIF_START +
            htif::get_csr_rel_addr(htif::csr::fromhost), "htif.fromhost");
    }

    void do_write_htif_fromhost(uint64_t val) {
        check_write_word(PMA_HTIF_START +
            htif::get_csr_rel_addr(htif::csr::fromhost), val, "htif.fromhost");
    }

    uint64_t do_read_htif_tohost(void) {
        return check_read_word(PMA_HTIF_START +
            htif::get_csr_rel_addr(htif::csr::tohost), "htif.tohost");
    }

    void do_write_htif_tohost(uint64_t val) {
        check_write_word(PMA_HTIF_START +
            htif::get_csr_rel_addr(htif::csr::tohost), val, "htif.tohost");
    }

    uint64_t do_read_htif_ihalt(void) {
        return check_read_word(PMA_HTIF_START +
            htif::get_csr_rel_addr(htif::csr::ihalt), "htif.ihalt");
    }

    uint64_t do_read_htif_iconsole(void) {
        return check_read_word(PMA_HTIF_START +
            htif::get_csr_rel_addr(htif::csr::iconsole), "htif.iconsole");
    }

    uint64_t do_read_htif_iyield(void) {
        return check_read_word(PMA_HTIF_START +
            htif::get_csr_rel_addr(htif::csr::iyield), "htif.iyield");
    }

    uint64_t do_read_pma_istart(int i) {
        auto rel_addr = shadow_get_pma_rel_addr(i);
        return check_read_word(PMA_SHADOW_START + rel_addr, "pma.istart");
    }

    uint64_t do_read_pma_ilength(int i) {
        auto rel_addr = shadow_get_pma_rel_addr(i);
        return check_read_word(PMA_SHADOW_START + rel_addr +
            sizeof(uint64_t), "pma.ilength");
    }

    template <typename T>
    void do_read_memory_word(uint64_t paddr, const unsigned char *hpage,
        uint64_t hoffset, T *pval) {
        (void) hpage; (void) hoffset;
        uint64_t paligned = paddr & (~(sizeof(uint64_t)-1));
        uint64_t poffset = paddr & (sizeof(uint64_t)-1);
        uint64_t val64 = check_read_word(paligned, "memory");
        auto pval64 = reinterpret_cast<const unsigned char *>(&val64);
        assert((paddr & (sizeof(T)-1)) == 0);
        *pval = aliased_aligned_read<T>(pval64+poffset);
    }

    void do_write_memory(uint64_t paddr, const unsigned char *data,
        uint64_t log2_size) {
        uint64_t len = UINT64_C(1) << log2_size;
        access_data val(data, data+len);
        check_write(paddr, val, log2_size, "block write");
    }

    template <typename T>
    void do_write_memory_word(uint64_t paddr, unsigned char *hpage,
        uint64_t hoffset, T val) {
        (void) hpage; (void) hoffset;
        assert((paddr & (sizeof(T)-1)) == 0);
        if constexpr (sizeof(T) < sizeof(uint64_t)) {
            uint64_t paligned = paddr & (~(sizeof(uint64_t)-1));
            uint64_t val64 = check_read_word(paligned, "memory (superfluous)");
            unsigned char *pval64 = reinterpret_cast<unsigned char *>(&val64);
            uint64_t poffset = paddr & (sizeof(uint64_t)-1);
            aliased_aligned_write<T>(pval64+poffset, val);
            check_write_word(paligned, val64, "memory");
        } else {
            check_write_word(paddr, val, "memory");
        }
    }

    pma_entry &allocate_mock_pma_entry(pma_entry &&pma) {
        if (m_mock_pmas.size() == m_mock_pmas.capacity()) {
            throw std::invalid_argument{"too many PMA accesses"};
        }
        m_mock_pmas.push_back(std::move(pma));
        return m_mock_pmas.back();
    }

    pma_entry &error_flags(const std::string &what) {
        static pma_entry empty{};
        throw std::invalid_argument{"invalid flags in access "
            + std::to_string(access_to_report()) + " to PMA (" + what + ")"};
        return empty; // never reached
    }

    pma_entry &build_mock_memory_pma_entry(uint64_t start, uint64_t length,
        const pma_entry::flags &f) {
        if (f.DID != PMA_ISTART_DID::memory && f.DID != PMA_ISTART_DID::drive) {
            return error_flags("invalid DID " +
                std::to_string(static_cast<int>(f.DID)) + " for M");
        }
        return allocate_mock_pma_entry(
            make_mockd_memory_pma_entry(start, length).set_flags(f)
        );
    }

    pma_entry &build_mock_device_pma_entry(uint64_t start, uint64_t length,
        const pma_entry::flags &f) {
        switch (f.DID) {
            case PMA_ISTART_DID::shadow:
                return allocate_mock_pma_entry(
                    make_shadow_pma_entry(start, length).set_flags(f));
            case PMA_ISTART_DID::CLINT:
                return allocate_mock_pma_entry(
                    make_clint_pma_entry(start, length).set_flags(f));
            case PMA_ISTART_DID::HTIF:
                return allocate_mock_pma_entry(
                    make_htif_pma_entry(start, length).set_flags(f));
            case PMA_ISTART_DID::DHD:
                return allocate_mock_pma_entry(
                    make_dhd_pma_entry(start, length).set_flags(f));
            default:
                return error_flags("invalid DID " +
                    std::to_string(static_cast<int>(f.DID)) + " for IO");
        }
    }

    pma_entry &build_mock_empty_pma_entry(uint64_t start, uint64_t length,
        const pma_entry::flags &f) {
        return allocate_mock_pma_entry(
            make_empty_pma_entry(start, length).set_flags(f)
        );
    }

    void split_istart(uint64_t istart, uint64_t &start,
        bool &M, bool &IO, bool &E, pma_entry::flags &f) {
        M = (istart & PMA_ISTART_M_MASK) >> PMA_ISTART_M_SHIFT;
        IO = (istart & PMA_ISTART_IO_MASK) >> PMA_ISTART_IO_SHIFT;
        E = (istart & PMA_ISTART_E_MASK) >> PMA_ISTART_E_SHIFT;
        f.R = (istart & PMA_ISTART_R_MASK) >> PMA_ISTART_R_SHIFT;
        f.W = (istart & PMA_ISTART_W_MASK) >> PMA_ISTART_W_SHIFT;
        f.X = (istart & PMA_ISTART_X_MASK) >> PMA_ISTART_X_SHIFT;
        f.IR = (istart & PMA_ISTART_IR_MASK) >> PMA_ISTART_IR_SHIFT;
        f.IW = (istart & PMA_ISTART_IW_MASK) >> PMA_ISTART_IW_SHIFT;
        f.DID = static_cast<PMA_ISTART_DID>((istart & PMA_ISTART_DID_MASK) >>
           PMA_ISTART_DID_SHIFT);
        start = istart & PMA_ISTART_START_MASK;
    }

    pma_entry &build_mock_pma_entry(uint64_t istart, uint64_t ilength) {
        bool M, IO, E;
        pma_entry::flags f;
        uint64_t start;
        split_istart(istart, start, M, IO, E, f);
        if (M + IO + E != 1) { // one and only one must be set
            return error_flags("multiple M/IO/E set");
        }
        if (M) {
            return build_mock_memory_pma_entry(start, ilength, f);
        } else if (IO) {
            return build_mock_device_pma_entry(start, ilength, f);
        } else {
            return build_mock_empty_pma_entry(start, ilength, f);
        }
    }

    template <typename T>
    pma_entry &do_find_pma_entry(uint64_t paddr) {
        int i = 0;
        while (1) {
            auto istart = this->read_pma_istart(i);
            auto ilength = this->read_pma_ilength(i);
            if (ilength == 0)
                return this->build_mock_pma_entry(istart, ilength);
            uint64_t start = istart & PMA_ISTART_START_MASK;
            if (paddr >= start && paddr - start <= ilength - sizeof(T)) {
                return this->build_mock_pma_entry(istart, ilength);
            }
            i++;
        }
    }

};


/// \brief Type-trait preventing the use of TLB while
/// accessing memory in the state
template <>
struct avoid_tlb<step_state_access> {
    static constexpr bool value = true;
};

} // namespace cartesi

#endif
