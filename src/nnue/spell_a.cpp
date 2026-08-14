/*
  Spell-Stockfish, a Spell Chess engine derived from Stockfish
  Copyright (C) 2004-2026 The Stockfish developers (see AUTHORS file)

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// Spell-NNUE A implementation: SPLA loading, the SpellAv2 feature transformer
// and the spell-aware incremental accumulator machinery. Structure
// deliberately mirrors spell_v2.cpp so the two paths stay reviewable side by
// side; everything that differs follows from the flat feature set having no
// king bucket and no threat block.

#include "spell_a.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <fstream>
#include <memory>
#include <ostream>
#include <utility>
#include <vector>

#include "../bitboard.h"
#include "../misc.h"
#include "../position.h"
#include "../types.h"
#include "nnue_accumulator.h"
#include "nnue_common.h"
#include "nnue_feature_transformer.h"  // permute helpers and PackusEpi16Order
#include "simd.h"
#include "spell_v2.h"  // shared material x potions output bucket

namespace Stockfish::Eval::NNUE::SpellA {

using namespace SIMD;

namespace {

std::unique_ptr<NetworkA> theNet;
std::atomic<u32>          netGeneration{0};
bool                      failedFlag = false;
std::string               failedPathStr;
std::string               failedReasonStr;
std::string               fileNameStr;

}  // namespace

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

void FeatureTransformerA::permute_weights() {
    permute<16>(biases, FeatureTransformer::PackusEpi16Order);
    permute<16>(weights, FeatureTransformer::PackusEpi16Order);
}

IndexType FeatureTransformerA::global_delta_index(
  Color perspective, Color owner, SpellType spell, int oldHand, int oldCd, int newHand, int newCd) {
    const int relativeBase = int(owner != perspective) * GlobalDeltasPerColor;
    const int spellBase    = spell == SPELL_FREEZE ? 0 : FreezeCastDeltas + TickDeltas;
    const int castCount    = spell == SPELL_FREEZE ? FreezeCastDeltas : JumpCastDeltas;

    if (oldCd == 0 && newCd == SPELL_COOLDOWN && newHand + 1 == oldHand)
    {
        assert(oldHand >= 1 && oldHand <= castCount);
        return relativeBase + spellBase + oldHand - 1;
    }

    assert(newHand == oldHand && oldCd >= 1 && oldCd <= SPELL_COOLDOWN && newCd == oldCd - 1);
    const int tickKind = oldCd == 3 ? 0 : oldCd == 2 ? 1 : oldHand == 0 ? 2 : 3;
    return relativeBase + spellBase + castCount + tickKind;
}

void FeatureTransformerA::build_global_deltas() {
    auto active_globals = [](Color relativeOwner, SpellType spell, int hand, int cd) {
        ValueList<IndexType, 9> active;
        for (int k = 0; k < hand; ++k)
            active.push_back(SpellFeatureSet::make_global_index(
              WHITE, relativeOwner, SpellFeatureSet::slot_hand(spell) + k));
        for (int k = 0; k < cd; ++k)
            active.push_back(SpellFeatureSet::make_global_index(
              WHITE, relativeOwner, SpellFeatureSet::slot_cd(spell) + k));
        if (hand > 0 && cd == 0)
            active.push_back(SpellFeatureSet::make_global_index(
              WHITE, relativeOwner, SpellFeatureSet::slot_ready(spell)));
        return active;
    };

    auto build_delta = [&](Color relativeOwner, SpellType spell, int oldHand, int oldCd,
                           int newHand, int newCd) {
        const IndexType deltaIndex =
          global_delta_index(WHITE, relativeOwner, spell, oldHand, oldCd, newHand, newCd);
        auto&      delta       = globalDeltas[deltaIndex];
        const auto oldFeatures = active_globals(relativeOwner, spell, oldHand, oldCd);
        const auto newFeatures = active_globals(relativeOwner, spell, newHand, newCd);

        // SIMD accumulator arithmetic wraps in 16 bits. Build the composite
        // row with the same modular arithmetic so every quantized net stays
        // bit-identical, even at an extreme weight range.
        for (IndexType j = 0; j < HalfDimensions; ++j)
        {
            u16 sum = 0;
            for (const IndexType index : newFeatures)
                sum += u16(weights[index * HalfDimensions + j]);
            for (const IndexType index : oldFeatures)
                sum -= u16(weights[index * HalfDimensions + j]);
            std::memcpy(&delta.weights[j], &sum, sizeof(sum));
        }

        for (IndexType bucket = 0; bucket < SpellPSQTBuckets; ++bucket)
        {
            u32 sum = 0;
            for (const IndexType index : newFeatures)
                sum += u32(psqtWeights[index * SpellPSQTBuckets + bucket]);
            for (const IndexType index : oldFeatures)
                sum -= u32(psqtWeights[index * SpellPSQTBuckets + bucket]);
            std::memcpy(&delta.psqtWeights[bucket], &sum, sizeof(sum));
        }
    };

    for (Color relativeOwner : {WHITE, BLACK})
        for (SpellType spell : {SPELL_FREEZE, SPELL_JUMP})
        {
            const int maxHand = spell == SPELL_FREEZE ? FreezeCastDeltas : JumpCastDeltas;
            for (int hand = 1; hand <= maxHand; ++hand)
                build_delta(relativeOwner, spell, hand, 0, hand - 1, SPELL_COOLDOWN);

            build_delta(relativeOwner, spell, 0, 3, 0, 2);
            build_delta(relativeOwner, spell, 0, 2, 0, 1);
            build_delta(relativeOwner, spell, 0, 1, 0, 0);
            build_delta(relativeOwner, spell, 1, 1, 1, 0);
        }
}

bool FeatureTransformerA::read_parameters(std::istream& stream) {
    read_leb_128(stream, biases);
    read_leb_128(stream, weights);
    read_leb_128(stream, psqtWeights);

    permute_weights();
    build_global_deltas();

    return !stream.fail();
}

bool NetworkA::read_parameters(std::istream& stream, std::string& netDescription) {
    u32 version, hashValue, size;

    version   = read_little_endian<u32>(stream);
    hashValue = read_little_endian<u32>(stream);
    size      = read_little_endian<u32>(stream);
    if (!stream || version != Version || size > 1024 * 1024)
        return false;
    netDescription.resize(size);
    stream.read(&netDescription[0], size);
    if (stream.fail() || hashValue != hash)
        return false;

    u32 ftHeader = read_little_endian<u32>(stream);
    if (!stream || ftHeader != FeatureTransformerA::get_hash_value())
        return false;
    if (!featureTransformer.read_parameters(stream))
        return false;

    for (usize i = 0; i < SpellLayerStacks; ++i)
    {
        u32 header = read_little_endian<u32>(stream);
        if (!stream || header != NetworkArchitecture::get_hash_value())
            return false;
        if (!stacks[i].read_parameters(stream))
            return false;
    }

    return stream && stream.peek() == std::ios::traits_type::eof();
}

bool looks_like_a_net(const std::string& path) {
    std::ifstream stream(path_from_utf8(path), std::ios::binary);
    if (!stream)
        return false;
    const u32 version = read_little_endian<u32>(stream);
    return version == Version && stream.good();
}

bool load(const std::string& path) {
    std::ifstream stream(path_from_utf8(path), std::ios::binary);
    if (!stream)
    {
        failedFlag      = true;
        failedPathStr   = path;
        failedReasonStr = "file could not be opened";
        return false;
    }

    auto        candidate = std::make_unique<NetworkA>();
    std::string description;

    if (!candidate->read_parameters(stream, description))
    {
        failedFlag      = true;
        failedPathStr   = path;
        failedReasonStr = "invalid 0x53504C41 version, feature hash, payload, or file size";
        return false;
    }

    theNet     = std::move(candidate);
    failedFlag = false;
    failedReasonStr.clear();
    fileNameStr = path;
    netGeneration.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void unload() {
    if (theNet)
    {
        theNet.reset();
        netGeneration.fetch_add(1, std::memory_order_relaxed);
    }
    failedFlag = false;
    failedReasonStr.clear();
    fileNameStr.clear();
}

bool loaded() { return theNet != nullptr; }

u32 generation() { return netGeneration.load(std::memory_order_relaxed); }

bool load_failed() { return failedFlag && !loaded(); }

const std::string& failed_path() { return failedPathStr; }

const std::string& failed_reason() { return failedReasonStr; }

const std::string& file_name() { return fileNameStr; }

// ---------------------------------------------------------------------------
// Incremental accumulator machinery (A twin of spell_v2.cpp)
// ---------------------------------------------------------------------------

namespace {

// Enumerate the spell-feature diff between a Finny entry's spell snapshot and
// the current position, then refresh the snapshot. Bound per direction:
// gates 4 + frozen 18 + globals 30 = 52 (on top of <= 32 piece diffs).
void append_spell_diff(Color                       perspective,
                       Caches::Entry&              entry,
                       const Position&             pos,
                       SpellFeatureSet::IndexList& removed,
                       SpellFeatureSet::IndexList& added) {
    for (Color c : {WHITE, BLACK})
    {
        for (SpellType sp : {SPELL_FREEZE, SPELL_JUMP})
        {
            const Square oldG = Square(entry.gate[c][sp]);
            const Square newG = pos.spell_gate(c, sp);
            if (oldG != newG)
            {
                if (sp == SPELL_FREEZE)
                {
                    if (oldG != SQ_NONE)
                        removed.push_back(SpellFeatureSet::make_freeze_index(perspective, c, oldG));
                    if (newG != SQ_NONE)
                        added.push_back(SpellFeatureSet::make_freeze_index(perspective, c, newG));
                }
                else
                {
                    if (oldG != SQ_NONE)
                        removed.push_back(SpellFeatureSet::make_jump_index(perspective, c, oldG));
                    if (newG != SQ_NONE)
                        added.push_back(SpellFeatureSet::make_jump_index(perspective, c, newG));
                }
                entry.gate[c][sp] = u8(newG);
            }

            const int oldH = entry.hand[c][sp], newH = pos.spells_in_hand(c, sp);
            for (int k = newH; k < oldH; ++k)
                removed.push_back(SpellFeatureSet::make_global_index(
                  perspective, c, SpellFeatureSet::slot_hand(sp) + k));
            for (int k = oldH; k < newH; ++k)
                added.push_back(SpellFeatureSet::make_global_index(
                  perspective, c, SpellFeatureSet::slot_hand(sp) + k));

            const int oldCd = entry.cd[c][sp], newCd = pos.spell_cooldown(c, sp);
            for (int k = newCd; k < oldCd; ++k)
                removed.push_back(SpellFeatureSet::make_global_index(
                  perspective, c, SpellFeatureSet::slot_cd(sp) + k));
            for (int k = oldCd; k < newCd; ++k)
                added.push_back(SpellFeatureSet::make_global_index(
                  perspective, c, SpellFeatureSet::slot_cd(sp) + k));

            const bool oldReady = oldH > 0 && oldCd == 0;
            const bool newReady = newH > 0 && newCd == 0;
            if (oldReady != newReady)
                (newReady ? added : removed)
                  .push_back(SpellFeatureSet::make_global_index(perspective, c,
                                                                SpellFeatureSet::slot_ready(sp)));

            entry.hand[c][sp] = i8(newH);
            entry.cd[c][sp]   = i8(newCd);
        }

        const Bitboard newFrozen = pos.pieces(c) & pos.frozen_squares(c);
        Bitboard       rem       = entry.frozen[c] & ~newFrozen;
        while (rem)
            removed.push_back(SpellFeatureSet::make_frozen_index(perspective, c, pop_lsb(rem)));
        Bitboard add = newFrozen & ~entry.frozen[c];
        while (add)
            added.push_back(SpellFeatureSet::make_frozen_index(perspective, c, pop_lsb(add)));
        entry.frozen[c] = newFrozen;
    }
}

Bitboard get_changed_pieces_a(const std::array<Piece, SQUARE_NB>& oldPieces,
                              const std::array<Piece, SQUARE_NB>& newPieces) {
#if defined(USE_AVX2)
    static_assert(sizeof(Piece) == 1);
    Bitboard sameBB = 0;

    for (int i = 0; i < 64; i += 32)
    {
        const __m256i oldV = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&oldPieces[i]));
        const __m256i newV = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&newPieces[i]));
        const u32     equalMask = u32(_mm256_movemask_epi8(_mm256_cmpeq_epi8(oldV, newV)));
        sameBB |= Bitboard(equalMask) << i;
    }
    return ~sameBB;
#else
    Bitboard changed = 0;
    for (Square sq = SQ_A1; sq <= SQ_H8; ++sq)
        changed |= Bitboard(oldPieces[sq] != newPieces[sq]) << sq;
    return changed;
#endif
}

template<bool Forward>
void apply_combined_a(Color                             perspective,
                      const FeatureTransformerA&        featureTransformer,
                      const AccumulatorState&           from,
                      AccumulatorState&                 to,
                      const SpellFeatureSet::IndexList& added,
                      const SpellFeatureSet::IndexList& removed,
                      const ValueList<IndexType, 4>&    globalDeltas) {
    constexpr IndexType Dimensions = FeatureTransformerA::OutputDimensions;

    const auto& fromAcc = from.accumulation[perspective];
    auto&       toAcc   = to.accumulation[perspective];

    const auto& fromPsqtAcc = from.spellPsqtAccumulation[perspective];
    auto&       toPsqtAcc   = to.spellPsqtAccumulation[perspective];

#ifdef VECTOR
    using Tiling = SIMDTiling<Dimensions, Dimensions, SpellPSQTBuckets>;

    vec_t      acc[Tiling::NumRegs];
    psqt_vec_t psqt[Tiling::NumPsqtRegs];

    const auto* psqWeights = &featureTransformer.weights[0];

    for (IndexType j = 0; j < Dimensions / Tiling::TileHeight; ++j)
    {
        const usize tileOff  = j * Tiling::TileHeight;
        auto*       fromTile = reinterpret_cast<const vec_t*>(&fromAcc[tileOff]);
        auto*       toTile   = reinterpret_cast<vec_t*>(&toAcc[tileOff]);

        for (IndexType k = 0; k < Tiling::NumRegs; ++k)
            acc[k] = fromTile[k];

        for (int i = 0; i < removed.ssize(); ++i)
        {
            auto* row =
              reinterpret_cast<const vec_t*>(&psqWeights[removed[i] * Dimensions + tileOff]);
            for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                acc[k] = vec_sub_16(acc[k], row[k]);
        }

        for (int i = 0; i < added.ssize(); ++i)
        {
            auto* row =
              reinterpret_cast<const vec_t*>(&psqWeights[added[i] * Dimensions + tileOff]);
            for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                acc[k] = vec_add_16(acc[k], row[k]);
        }

        for (const IndexType index : globalDeltas)
        {
            const auto* row = reinterpret_cast<const vec_t*>(
              &featureTransformer.globalDeltas[index].weights[tileOff]);
            for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                if constexpr (Forward)
                    acc[k] = vec_add_16(acc[k], row[k]);
                else
                    acc[k] = vec_sub_16(acc[k], row[k]);
        }

        for (IndexType k = 0; k < Tiling::NumRegs; k++)
            vec_store(&toTile[k], acc[k]);
    }

    for (IndexType j = 0; j < SpellPSQTBuckets / Tiling::PsqtTileHeight; ++j)
    {
        const usize psqtTileOff  = j * Tiling::PsqtTileHeight;
        auto*       fromTilePsqt = reinterpret_cast<const psqt_vec_t*>(&fromPsqtAcc[psqtTileOff]);
        auto*       toTilePsqt   = reinterpret_cast<psqt_vec_t*>(&toPsqtAcc[psqtTileOff]);

        for (IndexType k = 0; k < Tiling::NumPsqtRegs; ++k)
            psqt[k] = fromTilePsqt[k];

        for (int i = 0; i < removed.ssize(); ++i)
        {
            auto* columnPsqt = reinterpret_cast<const psqt_vec_t*>(
              &featureTransformer.psqtWeights[removed[i] * SpellPSQTBuckets + psqtTileOff]);
            for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                psqt[k] = vec_sub_psqt_32(psqt[k], columnPsqt[k]);
        }

        for (int i = 0; i < added.ssize(); ++i)
        {
            auto* columnPsqt = reinterpret_cast<const psqt_vec_t*>(
              &featureTransformer.psqtWeights[added[i] * SpellPSQTBuckets + psqtTileOff]);
            for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                psqt[k] = vec_add_psqt_32(psqt[k], columnPsqt[k]);
        }

        for (const IndexType index : globalDeltas)
        {
            const auto* columnPsqt = reinterpret_cast<const psqt_vec_t*>(
              &featureTransformer.globalDeltas[index].psqtWeights[psqtTileOff]);
            for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                if constexpr (Forward)
                    psqt[k] = vec_add_psqt_32(psqt[k], columnPsqt[k]);
                else
                    psqt[k] = vec_sub_psqt_32(psqt[k], columnPsqt[k]);
        }

        for (IndexType k = 0; k < Tiling::NumPsqtRegs; ++k)
            vec_store_psqt(&toTilePsqt[k], psqt[k]);
    }

#else

    toAcc     = fromAcc;
    toPsqtAcc = fromPsqtAcc;

    for (const auto index : removed)
    {
        const IndexType offset = Dimensions * index;
        for (IndexType j = 0; j < Dimensions; ++j)
            toAcc[j] -= featureTransformer.weights[offset + j];
        for (usize k = 0; k < SpellPSQTBuckets; ++k)
            toPsqtAcc[k] -= featureTransformer.psqtWeights[index * SpellPSQTBuckets + k];
    }

    for (const auto index : added)
    {
        const IndexType offset = Dimensions * index;
        for (IndexType j = 0; j < Dimensions; ++j)
            toAcc[j] += featureTransformer.weights[offset + j];
        for (usize k = 0; k < SpellPSQTBuckets; ++k)
            toPsqtAcc[k] += featureTransformer.psqtWeights[index * SpellPSQTBuckets + k];
    }

    for (const auto index : globalDeltas)
    {
        for (IndexType j = 0; j < Dimensions; ++j)
            if constexpr (Forward)
                toAcc[j] += featureTransformer.globalDeltas[index].weights[j];
            else
                toAcc[j] -= featureTransformer.globalDeltas[index].weights[j];
        for (usize k = 0; k < SpellPSQTBuckets; ++k)
            if constexpr (Forward)
                toPsqtAcc[k] += featureTransformer.globalDeltas[index].psqtWeights[k];
            else
                toPsqtAcc[k] -= featureTransformer.globalDeltas[index].psqtWeights[k];
    }

#endif
}

template<bool Forward>
void update_accumulator_incremental_a(Color                      perspective,
                                      const FeatureTransformerA& featureTransformer,
                                      AccumulatorState&          target_state,
                                      const AccumulatorState&    computed) {

    assert(computed.computed[perspective]);
    assert(!target_state.computed[perspective]);

    SpellFeatureSet::IndexList removed, added;
    ValueList<IndexType, 4>    globalDeltas;

    const auto& dirtyPiece = Forward ? target_state.dirtyPiece : computed.dirtyPiece;
    const auto& dirtySpell = Forward ? target_state.dirtySpell : computed.dirtySpell;

    if constexpr (Forward)
        SpellFeatureSet::append_changed_indices(perspective, dirtyPiece, dirtySpell, removed, added);
    else
        SpellFeatureSet::append_changed_indices(perspective, dirtyPiece, dirtySpell, added, removed);

    for (Color owner : {WHITE, BLACK})
        for (SpellType spell : {SPELL_FREEZE, SPELL_JUMP})
        {
            const int oldHand = dirtySpell.oldHand[owner][spell];
            const int oldCd   = dirtySpell.oldCd[owner][spell];
            const int newHand = dirtySpell.newHand[owner][spell];
            const int newCd   = dirtySpell.newCd[owner][spell];
            if (oldHand != newHand || oldCd != newCd)
                globalDeltas.push_back(FeatureTransformerA::global_delta_index(
                  perspective, owner, spell, oldHand, oldCd, newHand, newCd));
        }

    apply_combined_a<Forward>(perspective, featureTransformer, computed, target_state, added,
                              removed, globalDeltas);

    target_state.computed[perspective] = true;
}

// Rebuild from the Finny entry by piece and spell diff. Unlike the v2 twin
// there is nothing left to add on top: the entry holds the complete
// accumulation once its diff is applied.
void update_accumulator_refresh_cache_a(Color                      perspective,
                                        const FeatureTransformerA& featureTransformer,
                                        const Position&            pos,
                                        AccumulatorState&          accumulator,
                                        Caches&                    cache) {
    constexpr auto Dimensions = FeatureTransformerA::OutputDimensions;

    using Tiling [[maybe_unused]] = SIMDTiling<Dimensions, Dimensions, SpellPSQTBuckets>;

    auto&                      entry = cache.entries[perspective];
    SpellFeatureSet::IndexList removed, added;

    const Bitboard changedBB = get_changed_pieces_a(entry.pieces, pos.piece_array());

    Bitboard removedBB = changedBB & entry.pieceBB;
    Bitboard addedBB   = changedBB & pos.pieces();

    while (removedBB)
    {
        Square sq = pop_lsb(removedBB);
        removed.push_back(SpellFeatureSet::make_index(perspective, sq, entry.pieces[sq]));
    }
    while (addedBB)
    {
        Square sq = pop_lsb(addedBB);
        added.push_back(SpellFeatureSet::make_index(perspective, sq, pos.piece_on(sq)));
    }

    entry.pieceBB = pos.pieces();
    entry.pieces  = pos.piece_array();

    // Spell-state diff vs the entry snapshot (also refreshes the snapshot)
    append_spell_diff(perspective, entry, pos, removed, added);

    accumulator.computed[perspective] = true;

#ifdef VECTOR
    vec_t      acc[Tiling::NumRegs];
    psqt_vec_t psqt[Tiling::NumPsqtRegs];

    const auto* weights = &featureTransformer.weights[0];

    for (IndexType j = 0; j < Dimensions / Tiling::TileHeight; ++j)
    {
        const usize tileOff = j * Tiling::TileHeight;
        auto* accTile   = reinterpret_cast<vec_t*>(&accumulator.accumulation[perspective][tileOff]);
        auto* entryTile = reinterpret_cast<vec_t*>(&entry.accumulation[tileOff]);

        for (IndexType k = 0; k < Tiling::NumRegs; ++k)
            acc[k] = entryTile[k];

        for (int i = 0; i < removed.ssize(); ++i)
        {
            auto* column =
              reinterpret_cast<const vec_t*>(&weights[removed[i] * Dimensions + tileOff]);
            for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                acc[k] = vec_sub_16(acc[k], column[k]);
        }
        for (int i = 0; i < added.ssize(); ++i)
        {
            auto* column =
              reinterpret_cast<const vec_t*>(&weights[added[i] * Dimensions + tileOff]);
            for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                acc[k] = vec_add_16(acc[k], column[k]);
        }

        for (IndexType k = 0; k < Tiling::NumRegs; k++)
        {
            vec_store(&entryTile[k], acc[k]);
            vec_store(&accTile[k], acc[k]);
        }
    }

    for (IndexType j = 0; j < SpellPSQTBuckets / Tiling::PsqtTileHeight; ++j)
    {
        const usize psqtTileOff = j * Tiling::PsqtTileHeight;
        auto*       accTilePsqt = reinterpret_cast<psqt_vec_t*>(
          &accumulator.spellPsqtAccumulation[perspective][psqtTileOff]);
        auto* entryTilePsqt = reinterpret_cast<psqt_vec_t*>(&entry.psqtAccumulation[psqtTileOff]);

        for (IndexType k = 0; k < Tiling::NumPsqtRegs; ++k)
            psqt[k] = entryTilePsqt[k];

        for (int i = 0; i < removed.ssize(); ++i)
        {
            auto* columnPsqt = reinterpret_cast<const psqt_vec_t*>(
              &featureTransformer.psqtWeights[removed[i] * SpellPSQTBuckets + psqtTileOff]);
            for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                psqt[k] = vec_sub_psqt_32(psqt[k], columnPsqt[k]);
        }
        for (int i = 0; i < added.ssize(); ++i)
        {
            auto* columnPsqt = reinterpret_cast<const psqt_vec_t*>(
              &featureTransformer.psqtWeights[added[i] * SpellPSQTBuckets + psqtTileOff]);
            for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                psqt[k] = vec_add_psqt_32(psqt[k], columnPsqt[k]);
        }

        for (IndexType k = 0; k < Tiling::NumPsqtRegs; ++k)
        {
            vec_store_psqt(&entryTilePsqt[k], psqt[k]);
            vec_store_psqt(&accTilePsqt[k], psqt[k]);
        }
    }

#else

    for (const auto index : removed)
    {
        const IndexType offset = Dimensions * index;
        for (IndexType j = 0; j < Dimensions; ++j)
            entry.accumulation[j] -= featureTransformer.weights[offset + j];

        for (usize k = 0; k < SpellPSQTBuckets; ++k)
            entry.psqtAccumulation[k] -=
              featureTransformer.psqtWeights[index * SpellPSQTBuckets + k];
    }
    for (const auto index : added)
    {
        const IndexType offset = Dimensions * index;
        for (IndexType j = 0; j < Dimensions; ++j)
            entry.accumulation[j] += featureTransformer.weights[offset + j];

        for (usize k = 0; k < SpellPSQTBuckets; ++k)
            entry.psqtAccumulation[k] +=
              featureTransformer.psqtWeights[index * SpellPSQTBuckets + k];
    }

    accumulator.accumulation[perspective]          = entry.accumulation;
    accumulator.spellPsqtAccumulation[perspective] = entry.psqtAccumulation;

#endif
}

}  // namespace

}  // namespace Stockfish::Eval::NNUE::SpellA

// ---------------------------------------------------------------------------
// AccumulatorStack members for the A path
// ---------------------------------------------------------------------------

namespace Stockfish::Eval::NNUE {

void AccumulatorStack::evaluate_a(const Position&                    pos,
                                  const SpellA::FeatureTransformerA& featureTransformer,
                                  SpellA::Caches&                    cache) noexcept {
    evaluate_a_side(WHITE, pos, featureTransformer, cache);
    evaluate_a_side(BLACK, pos, featureTransformer, cache);
}

void AccumulatorStack::evaluate_a_side(Color                              perspective,
                                       const Position&                    pos,
                                       const SpellA::FeatureTransformerA& featureTransformer,
                                       SpellA::Caches&                    cache) noexcept {

    const auto last_usable_accum = find_last_usable_accumulator_a(perspective);

    if (accumulators[last_usable_accum].computed[perspective])
    {
        for (usize next = last_usable_accum + 1; next < size; next++)
            SpellA::update_accumulator_incremental_a<true>(perspective, featureTransformer,
                                                           accumulators[next],
                                                           accumulators[next - 1]);
    }
    else
    {
        SpellA::update_accumulator_refresh_cache_a(perspective, featureTransformer, pos,
                                                   mut_latest(), cache);

        for (i64 next = i64(size) - 2; next >= i64(last_usable_accum); next--)
            SpellA::update_accumulator_incremental_a<false>(perspective, featureTransformer,
                                                            accumulators[next],
                                                            accumulators[next + 1]);
    }

    assert(latest().computed[perspective]);
}

// No SpellAv2 index depends on a king square and there is no threat block, so
// nothing on the path can invalidate an accumulated row: the earliest usable
// state is simply the most recent computed one.
usize AccumulatorStack::find_last_usable_accumulator_a(Color perspective) const noexcept {
    static_assert(!SpellA::SpellFeatureSet::RequiresRefresh);

    for (usize curr_idx = size - 1; curr_idx > 0; curr_idx--)
        if (accumulators[curr_idx].computed[perspective])
            return curr_idx;

    return 0;
}

}  // namespace Stockfish::Eval::NNUE

namespace Stockfish::Eval::NNUE::SpellA {

// ---------------------------------------------------------------------------
// Feature transformer output (A twin of FeatureTransformerV2::transform)
// ---------------------------------------------------------------------------

i32 FeatureTransformerA::transform(const Position&            pos,
                                   AccumulatorStack&          accumulatorStack,
                                   Caches&                    cache,
                                   TransformedFeatureType*    output,
                                   int                        bucket,
                                   NNZInfo<OutputDimensions>& nnzInfo) const {

    accumulatorStack.evaluate_a(pos, *this, cache);
    const auto& accumulatorState = accumulatorStack.latest();

    const Color perspectives[2]  = {pos.side_to_move(), ~pos.side_to_move()};
    const auto& psqtAccumulation = accumulatorState.spellPsqtAccumulation;
    const auto  psqt =
      (psqtAccumulation[perspectives[0]][bucket] - psqtAccumulation[perspectives[1]][bucket]) / 2;

    const auto& accumulation = accumulatorState.accumulation;

    for (IndexType p = 0; p < 2; ++p)
    {
        const IndexType offset = (HalfDimensions / 2) * p;

        [[maybe_unused]] auto cursor = nnzInfo.make_cursor(p);

#if defined(VECTOR)

        constexpr IndexType OutputChunkSize = MaxChunkSize;
        static_assert((HalfDimensions / 2) % OutputChunkSize == 0);
        constexpr IndexType NumOutputChunks = HalfDimensions / 2 / OutputChunkSize;

        [[maybe_unused]] const vec_t   Zero  = vec_zero();
        [[maybe_unused]] const vec_t   FtMax = vec_set_16(FtMaxVal);
        [[maybe_unused]] constexpr int shift = 7;

        const vec_t* in0 = reinterpret_cast<const vec_t*>(&(accumulation[perspectives[p]][0]));
        const vec_t* in1 =
          reinterpret_cast<const vec_t*>(&(accumulation[perspectives[p]][HalfDimensions / 2]));
        vec_t* out = reinterpret_cast<vec_t*>(output + offset);

        // See FeatureTransformer::transform for the commented derivation of
        // the packus/mulhi pairwise trick reproduced here verbatim.
        for (IndexType j = 0; j < NumOutputChunks; j += 2)
        {
            vec_t packed[2];
            for (IndexType k = 0; k < 2; ++k)
            {
                const IndexType i = (j + k) * 2;

                vec_t acc0a = in0[i + 0];
                vec_t acc0b = in0[i + 1];
                vec_t acc1a = in1[i + 0];
                vec_t acc1b = in1[i + 1];

                static_assert(FtMaxVal == 255);

    #if defined(USE_NEON)
                uint16x8_t mul0 = vmull_u8(vqmovun_s16(acc0a), vqmovun_s16(acc1a));
                uint16x8_t mul1 = vmull_u8(vqmovun_s16(acc0b), vqmovun_s16(acc1b));

                uint8x16x2_t uzp = vuzpq_u8(vreinterpretq_u8_u16(mul0), vreinterpretq_u8_u16(mul1));
                uint8x16_t   pab = vshrq_n_u8(uzp.val[1], 1);
                vec_t        result = reinterpret_cast<vec_t>(pab);
    #elif defined(USE_LSX) || defined(USE_LASX)
                vec_t pa = vec_packus_16(acc0a, acc0b);
                vec_t pb = vec_packus_16(acc1a, acc1b);

                vec_t hi     = vec_mulhi_8(pa, pb);
                vec_t result = vec_srli_8(hi, 1);
    #elif defined(__wasm__)
                vec_t mul0 = vec_packus_16(acc0a, acc0b);
                vec_t mul1 = vec_packus_16(acc1a, acc1b);

                vec_t low = wasm_u16x8_extmul_low_u8x16(mul0, mul1);
                vec_t hi  = wasm_u16x8_extmul_high_u8x16(mul0, mul1);

                vec_t merged = wasm_i8x16_shuffle(low, hi, 1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21,
                                                  23, 25, 27, 29, 31);
                vec_t result = wasm_u8x16_shr(merged, 1);
    #else
                vec_t sum0a = vec_slli_16(vec_max_16(vec_min_16(acc0a, FtMax), Zero), shift);
                vec_t sum0b = vec_slli_16(vec_max_16(vec_min_16(acc0b, FtMax), Zero), shift);
                vec_t sum1a = vec_min_16(acc1a, FtMax);
                vec_t sum1b = vec_min_16(acc1b, FtMax);

                vec_t pa = vec_mulhi_16(sum0a, sum1a);
                vec_t pb = vec_mulhi_16(sum0b, sum1b);

                vec_t result = vec_packus_16(pa, pb);
    #endif

                packed[k] = out[j + k] = result;
            }

            cursor.record2(packed[0], packed[1]);
        }

#else

        for (IndexType j = 0; j < HalfDimensions / 2; ++j)
        {
            BiasType sum0 = accumulation[static_cast<int>(perspectives[p])][j + 0];
            BiasType sum1 = accumulation[static_cast<int>(perspectives[p])][j + HalfDimensions / 2];

            sum0 = std::clamp<BiasType>(sum0, 0, FtMaxVal);
            sum1 = std::clamp<BiasType>(sum1, 0, FtMaxVal);

            output[offset + j] = static_cast<TransformedFeatureType>(unsigned(sum0 * sum1) / 512);
        }

#endif
    }

    return psqt;
}

// ---------------------------------------------------------------------------
// Evaluation entry points
// ---------------------------------------------------------------------------

int spell_bucket(const Position& pos) { return SpellV2::spell_bucket(pos); }

namespace {

void ensure_cache_generation(Caches& cache) {
    const u32 g = generation();
    if (cache.gen != g)
    {
        for (auto& entry : cache.entries)
            entry.clear(theNet->featureTransformer.biases);
        cache.gen = g;
    }
}

}  // namespace

std::pair<Value, Value> raw_evaluate(const Position& pos, AccumulatorStack& stack, Caches& cache) {
    assert(loaded());

    ensure_cache_generation(cache);

    constexpr u64                             alignment = CacheLineSize;
    alignas(alignment) TransformedFeatureType transformedFeatures[FeatureTransformerA::BufferSize];

    ASSERT_ALIGNED(transformedFeatures, alignment);

    NNZInfo<L1> nnzInfo;

    const int bucket = spell_bucket(pos);

    const i32 psqt =
      theNet->featureTransformer.transform(pos, stack, cache, transformedFeatures, bucket, nnzInfo);
    const i32 positional = theNet->stacks[bucket].propagate(transformedFeatures, nnzInfo);

    return {static_cast<Value>(psqt / OutputScale), static_cast<Value>(positional / OutputScale)};
}

Value evaluate(const Position& pos, AccumulatorStack& stack, Caches& cache, int optimism) {

    auto [psqt, positional] = raw_evaluate(pos, stack, cache);

    Value nnue = psqt + positional;

    // Blend optimism and eval with nnue complexity, exactly like
    // Eval::evaluate for the stock networks
    int nnueComplexity = std::abs(psqt - positional);
    optimism += optimism * i64(nnueComplexity) / 476;
    nnue -= nnue * i64(nnueComplexity) / 18236;

    int material = 534 * pos.count<PAWN>() + pos.non_pawn_material();
    int v        = (nnue * i64(77871 + material) + optimism * i64(7191 + material)) / 77871;

    v -= v * pos.rule50_count() / 199;

    return std::clamp(v, VALUE_TB_LOSS_IN_MAX_PLY + 1, VALUE_TB_WIN_IN_MAX_PLY - 1);
}

void dump_features(const Position& pos, std::ostream& os) {
    os << "bucket " << spell_bucket(pos) << "\n";

    for (Color perspective : {WHITE, BLACK})
    {
        SpellFeatureSet::IndexList psq;

        Bitboard bb = pos.pieces();
        while (bb)
        {
            const Square sq = pop_lsb(bb);
            psq.push_back(SpellFeatureSet::make_index(perspective, sq, pos.piece_on(sq)));
        }
        SpellFeatureSet::append_active_spell(perspective, pos, psq);

        std::vector<IndexType> sortedPsq(psq.begin(), psq.end());
        std::sort(sortedPsq.begin(), sortedPsq.end());

        os << "perspective " << (perspective == WHITE ? "w" : "b") << " psq";
        for (IndexType i : sortedPsq)
            os << ' ' << i;
        os << "\n";
    }
}

}  // namespace Stockfish::Eval::NNUE::SpellA
