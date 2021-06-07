// Copyright 2021 Cartesi Pte. Ltd.
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

#ifndef BACK_MERKLE_TREE_H
#define BACK_MERKLE_TREE_H

#include <limits>
#include "keccak-256-hasher.h"
#include "pristine-merkle-tree.h"
#include "merkle-tree-proof.h"

namespace cartesi {

/// \brief Incremental way of maintaining a Merkle tree for a stream of
/// leaf hashes
/// \details This is surprisingly efficient in both time and space.
/// Adding the next leaf takes O(log(n)) in the worst case, but is
/// this is amortized to O(1) time when adding n leaves.
/// Obtaining the proof for the current leaf takes theta(log(n)) time.
/// Computing the tree root hash also takes theta(log(n)) time.
/// The class only ever stores log(n) hashes (1 for each tree level).
class back_merkle_tree {
public:

    /// \brief Hasher class.
    using hasher_type = keccak_256_hasher;

    /// \brief Storage for a hash.
    using hash_type = hasher_type::hash_type;

    /// \brief Storage for a hash.
    using address_type = uint64_t;

    /// \brief Storage for the proof of a word value.
    using proof_type = merkle_tree_proof<hash_type, address_type>;

    /// \brief Constructor
    /// \param log2_root_size Log<sub>2</sub> of root node
    /// \param log2_leaf_size Log<sub>2</sub> of leaf node
    /// \param log2_word_size Log<sub>2</sub> of word
    back_merkle_tree(int log2_root_size, int log2_leaf_size,
        int log2_word_size):
        m_log2_root_size{log2_root_size},
        m_log2_leaf_size{log2_leaf_size},
        m_leaf_count{0}, m_max_leaves{address_type{1} << (log2_root_size-log2_leaf_size)},
        m_context(std::max(1,log2_root_size-log2_leaf_size+1)),
        m_pristine_hashes{log2_root_size, log2_word_size} {
        if (log2_root_size < 0) {
            throw std::out_of_range{"log2_root_size is negative"};
        }
        if (log2_leaf_size < 0) {
            throw std::out_of_range{"log2_leaf_size is negative"};
        }
        if (log2_word_size < 0) {
            throw std::out_of_range{"log2_word_size is negative"};
        }
        if (log2_leaf_size > log2_root_size) {
            throw std::out_of_range{
                "log2_leaf_size is greater than log2_root_size"};
        }
        if (log2_word_size > log2_leaf_size) {
            throw std::out_of_range{
                "log2_word_size is greater than log2_word_size"};
        }
        if (log2_root_size >= std::numeric_limits<address_type>::digits) {
            throw std::out_of_range{"tree is too large for address type"};
        }
    }

    /// \brief Appends a new hash to the tree
    /// \param hash Hash of leaf data
    /// \details
    /// Consider the tree down to the leaf level.
    /// The tree is only complete after 2^(log2_root_size-log2_leaf_size) leaves
    /// have been added.
    /// Before that, when leaf_count leaves have been added, we assume the rest
    /// of the leaves are filled with zeros (i.e., they are pristine).
    /// The trick is that we do not need to store the hashes of all leaf_count
    /// leaves already added to the stream.
    /// This is because, whenever a subtree is complete, all we need is its
    /// root hash.
    /// The complete subtrees are disjoint, abutting, and appear in decreasing
    /// size.
    /// In fact, there is exactly one complete subtree for each bit set in
    /// leaf_count.
    /// We only need log2_root_size-log2_leaf_size+1 bits to represent
    /// leaf_count.
    /// So our context is a vector with log2_root_size-log2_leaf_size+1 entries,
    /// where entry i contains the hash for a complete subtree of
    /// size 2^i leaves.
    /// We will only use the entries i if the corresponding bit is set
    /// in leaf_count.
    /// Adding a new leaf hash exactly like adding 1 to leaf_count.
    /// We scan from least to most significant bit in leaf_count.
    /// We start with the right = leaf_hash and i = 0.
    /// If the bit i is set in leaf_count, we replace
    /// context[i] = hash(context[i], right) and move up a bit.
    /// If the bit is not set, we simply store context[i] = right and break
    /// In other words, we can update the context in
    /// log time (log2_root_size-log2_leaf_size)
    void push_back(const hash_type &leaf_hash) {
        hasher_type h;
        hash_type right = leaf_hash;
        if (m_leaf_count >= m_max_leaves) {
            throw std::out_of_range{"too many leaves"};
        }
        int depth = m_log2_root_size-m_log2_leaf_size;
        for (int i = 0; i <= depth; ++i) {
            if (m_leaf_count & (address_type{1} << i)) {
                const auto &left = m_context[i];
                get_concat_hash(h, left, right, right);
            } else {
                m_context[i] = right;
                break;
            }
        }
        ++m_leaf_count;
    }

    /// \brief Returns the root tree hash
    /// \returns Root tree hash
    /// \details
    /// We can produce the tree root hash from the context at any time, also
    /// in log time
    /// Ostensibly, we add pristine leaves until the leaf_count
    /// hits 2^(log2_root_size-log2_leaf_size)
    /// To do this in log time, we start by precomputing the hashes for all
    /// completely pristine subtree sizes
    /// If leaf_count is already 2^(log2_root_size-log2_leaf_size), we
    /// return context[i]
    /// Otherwise, we start with i = 0 and root = pristine[i+log2_leaf_size]
    /// (i.e., the invariant is that root contains the hash of the rightmost
    /// subtree whose log size is i + log2_leaf_size)
    /// If bit i is set, we set root = hash(context[i], root) and move up a bit
    /// (i.e., the subtree we are growing is to the right of what is
    /// in the context)
    /// If bit i is not set, we set
    /// root = hash(root, pristine[i+log2_leaf_size]) and move up a bit
    /// (i.e., to grow our subtree, we need to pad it on the right with
    /// a pristine subtree of the same size)
    hash_type get_root_hash(void) const {
        hasher_type h;
        assert(m_leaf_count <= m_max_leaves);
        int depth = m_log2_root_size-m_log2_leaf_size;
        if (m_leaf_count < m_max_leaves) {
            auto root = m_pristine_hashes.get_hash(m_log2_leaf_size);
            for (int i = 0; i < depth; ++i) {
                if (m_leaf_count & (address_type{1} << i)) {
                    const auto &left = m_context[i];
                    get_concat_hash(h, left, root, root);
                } else {
                    const auto &right = m_pristine_hashes.get_hash(
                        m_log2_leaf_size+i);
                    get_concat_hash(h, root, right, root);
                }
            }
            return root;
        } else {
            return m_context[depth];
        }
    }

    /// \brief Returns proof for the next pristine leaf
    /// \returns Proof for leaf at given index, or throws exception
    /// \details This is basically the same algorithm as
    /// back_merkle_tree::get_root_hash.
    proof_type get_next_leaf_proof(void) const {
        int depth = m_log2_root_size-m_log2_leaf_size;
        if (m_leaf_count >= m_max_leaves) {
            throw std::out_of_range{"tree is full"};
        }
        hasher_type h;
        proof_type proof{m_log2_root_size, m_log2_leaf_size};
        proof.set_target_address(m_leaf_count << m_log2_leaf_size);
        proof.set_target_hash(m_pristine_hashes.get_hash(m_log2_leaf_size));
        hash_type hash = m_pristine_hashes.get_hash(m_log2_leaf_size);
        for (int i = 0; i < depth; ++i) {
            if (m_leaf_count & (address_type{1} << i)) {
                const auto &left = m_context[i];
                proof.set_sibling_hash(left, m_log2_leaf_size+i);
                get_concat_hash(h, left, hash, hash);
            } else {
                const auto &right = m_pristine_hashes.get_hash(
                    m_log2_leaf_size+i);
                proof.set_sibling_hash(right, m_log2_leaf_size+i);
                get_concat_hash(h, hash, right, hash);
            }
        }
        proof.set_root_hash(hash);
#ifndef NDEBUG
        if (!proof.verify(h)) {
            throw std::runtime_error{"produced invalid proof"};
        }
#endif
        return proof;
    }

private:

    int m_log2_root_size;             ///< Log<sub>2</sub> of tree size
    int m_log2_leaf_size;             ///< Log<sub>2</sub> of leaf size
    address_type m_leaf_count;      ///< Number of leaves already added
    address_type m_max_leaves;        ///< Maximum number of leaves
    std::vector<hash_type> m_context; ///< Hashes of bits set in leaf_count
    pristine_merkle_tree m_pristine_hashes; ///< Hash of pristine subtrees of all sizes

};

} // namespace cartesi

#endif
