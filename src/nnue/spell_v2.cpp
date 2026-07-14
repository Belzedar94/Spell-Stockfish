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

// Spell-NNUE v2 implementation: SPL2 loading, the SpellKAv2 feature
// transformer and the spell-aware incremental accumulator machinery.
// Structure deliberately mirrors nnue_accumulator.cpp / network.cpp /
// nnue_feature_transformer.h so the two paths stay reviewable side by side.

#include "spell_v2.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

#include "../bitboard.h"
#include "../evaluate.h"
#include "../misc.h"
#include "../position.h"
#include "../types.h"
#include "nnue_accumulator.h"
#include "nnue_common.h"
#include "nnue_feature_transformer.h"  // permute helpers and PackusEpi16Order
#include "simd.h"

namespace Stockfish::Eval::NNUE::SpellV2 {

using namespace SIMD;

namespace {

std::unique_ptr<NetworkV2> theNet;
std::atomic<u32>           netGeneration{0};
bool                       failedFlag = false;
std::string                failedPathStr;
std::string                fileNameStr;

}  // namespace

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

void FeatureTransformerV2::permute_weights() {
    permute<16>(biases, FeatureTransformer::PackusEpi16Order);
    permute<16>(weights, FeatureTransformer::PackusEpi16Order);
    permute<8>(threatWeights, FeatureTransformer::PackusEpi16Order);
}

bool FeatureTransformerV2::read_parameters(std::istream& stream) {
    read_leb_128(stream, biases);

    read_little_endian<ThreatWeightType>(stream, threatWeights.data(),
                                         ThreatInputDimensions * HalfDimensions);
    read_leb_128(stream, threatPsqtWeights);

    read_leb_128(stream, weights);
    read_leb_128(stream, psqtWeights);

    permute_weights();

    return !stream.fail();
}

bool NetworkV2::read_parameters(std::istream& stream, std::string& netDescription) {
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
    if (!stream || ftHeader != FeatureTransformerV2::get_hash_value())
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

bool looks_like_v2_net(const std::string& path) {
    std::ifstream stream(path_from_utf8(path), std::ios::binary);
    if (!stream)
        return false;
    return read_little_endian<u32>(stream) == Version && stream.good();
}

bool load(const std::string& path) {
    std::ifstream stream(path_from_utf8(path), std::ios::binary);
    if (!stream)
    {
        failedFlag    = true;
        failedPathStr = path;
        return false;
    }

    auto        candidate = std::make_unique<NetworkV2>();
    std::string description;

    if (!candidate->read_parameters(stream, description))
    {
        failedFlag    = true;
        failedPathStr = path;
        return false;
    }

    theNet      = std::move(candidate);
    failedFlag  = false;
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
    fileNameStr.clear();
}

bool loaded() { return theNet != nullptr; }

u32 generation() { return netGeneration.load(std::memory_order_relaxed); }

bool load_failed() { return failedFlag && !loaded(); }

const std::string& failed_path() { return failedPathStr; }

const std::string& file_name() { return fileNameStr; }

// ---------------------------------------------------------------------------
// Incremental accumulator machinery (v2 twin of nnue_accumulator.cpp)
// ---------------------------------------------------------------------------

namespace {

// Enumerate the spell-feature diff between a Finny entry's spell snapshot and
// the current position, then refresh the snapshot. Bound per direction:
// gates 4 + frozen 18 + globals 30 = 52 (on top of <= 32 piece diffs).
void append_spell_diff(Color           perspective,
                       Square          ksq,
                       Caches::Entry&  entry,
                       const Position& pos,
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
                        removed.push_back(
                          SpellFeatureSet::make_freeze_index(perspective, c, oldG, ksq));
                    if (newG != SQ_NONE)
                        added.push_back(
                          SpellFeatureSet::make_freeze_index(perspective, c, newG, ksq));
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
                  .push_back(SpellFeatureSet::make_global_index(
                    perspective, c, SpellFeatureSet::slot_ready(sp)));

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

void apply_combined_spell(Color                              perspective,
                          const FeatureTransformerV2&        featureTransformer,
                          const AccumulatorState&            from,
                          AccumulatorState&                  to,
                          const SpellFeatureSet::IndexList&  psqAdded,
                          const SpellFeatureSet::IndexList&  psqRemoved,
                          const ThreatFeatureSet::IndexList& thrAdded,
                          const ThreatFeatureSet::IndexList& thrRemoved) {
    constexpr IndexType Dimensions = FeatureTransformerV2::OutputDimensions;

    const auto& fromAcc = from.accumulation[perspective];
    auto&       toAcc   = to.accumulation[perspective];

    const auto& fromPsqtAcc = from.spellPsqtAccumulation[perspective];
    auto&       toPsqtAcc   = to.spellPsqtAccumulation[perspective];

#ifdef VECTOR
    using Tiling = SIMDTiling<Dimensions, Dimensions, SpellPSQTBuckets>;

    vec_t      acc[Tiling::NumRegs];
    psqt_vec_t psqt[Tiling::NumPsqtRegs];

    const auto* psqWeights    = &featureTransformer.weights[0];
    const auto* threatWeights = &featureTransformer.threatWeights[0];

    for (IndexType j = 0; j < Dimensions / Tiling::TileHeight; ++j)
    {
        const usize tileOff  = j * Tiling::TileHeight;
        auto*       fromTile = reinterpret_cast<const vec_t*>(&fromAcc[tileOff]);
        auto*       toTile   = reinterpret_cast<vec_t*>(&toAcc[tileOff]);

        for (IndexType k = 0; k < Tiling::NumRegs; ++k)
            acc[k] = fromTile[k];

        for (int i = 0; i < psqRemoved.ssize(); ++i)
        {
            auto* row =
              reinterpret_cast<const vec_t*>(&psqWeights[psqRemoved[i] * Dimensions + tileOff]);
            for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                acc[k] = vec_sub_16(acc[k], row[k]);
        }

        for (int i = 0; i < psqAdded.ssize(); ++i)
        {
            auto* row =
              reinterpret_cast<const vec_t*>(&psqWeights[psqAdded[i] * Dimensions + tileOff]);
            for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                acc[k] = vec_add_16(acc[k], row[k]);
        }

        for (int i = 0; i < thrRemoved.ssize(); ++i)
        {
            auto* column = reinterpret_cast<const vec_i8_t*>(
              &threatWeights[thrRemoved[i] * Dimensions + tileOff]);

    #ifdef USE_NEON
            for (IndexType k = 0; k < Tiling::NumRegs; k += 2)
            {
                acc[k]     = vsubw_s8(acc[k], vget_low_s8(column[k / 2]));
                acc[k + 1] = vsubw_high_s8(acc[k + 1], column[k / 2]);
            }
    #else
            for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                acc[k] = vec_sub_16(acc[k], vec_convert_8_16(column[k]));
    #endif
        }

        for (int i = 0; i < thrAdded.ssize(); ++i)
        {
            auto* column =
              reinterpret_cast<const vec_i8_t*>(&threatWeights[thrAdded[i] * Dimensions + tileOff]);

    #ifdef USE_NEON
            for (IndexType k = 0; k < Tiling::NumRegs; k += 2)
            {
                acc[k]     = vaddw_s8(acc[k], vget_low_s8(column[k / 2]));
                acc[k + 1] = vaddw_high_s8(acc[k + 1], column[k / 2]);
            }
    #else
            for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                acc[k] = vec_add_16(acc[k], vec_convert_8_16(column[k]));
    #endif
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

        for (int i = 0; i < psqRemoved.ssize(); ++i)
        {
            auto* columnPsqt = reinterpret_cast<const psqt_vec_t*>(
              &featureTransformer.psqtWeights[psqRemoved[i] * SpellPSQTBuckets + psqtTileOff]);
            for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                psqt[k] = vec_sub_psqt_32(psqt[k], columnPsqt[k]);
        }

        for (int i = 0; i < psqAdded.ssize(); ++i)
        {
            auto* columnPsqt = reinterpret_cast<const psqt_vec_t*>(
              &featureTransformer.psqtWeights[psqAdded[i] * SpellPSQTBuckets + psqtTileOff]);
            for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                psqt[k] = vec_add_psqt_32(psqt[k], columnPsqt[k]);
        }

        for (int i = 0; i < thrRemoved.ssize(); ++i)
        {
            auto* columnPsqt = reinterpret_cast<const psqt_vec_t*>(
              &featureTransformer
                 .threatPsqtWeights[thrRemoved[i] * SpellPSQTBuckets + psqtTileOff]);
            for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                psqt[k] = vec_sub_psqt_32(psqt[k], columnPsqt[k]);
        }

        for (int i = 0; i < thrAdded.ssize(); ++i)
        {
            auto* columnPsqt = reinterpret_cast<const psqt_vec_t*>(
              &featureTransformer.threatPsqtWeights[thrAdded[i] * SpellPSQTBuckets + psqtTileOff]);
            for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                psqt[k] = vec_add_psqt_32(psqt[k], columnPsqt[k]);
        }

        for (IndexType k = 0; k < Tiling::NumPsqtRegs; ++k)
            vec_store_psqt(&toTilePsqt[k], psqt[k]);
    }

#else

    toAcc     = fromAcc;
    toPsqtAcc = fromPsqtAcc;

    for (const auto index : psqRemoved)
    {
        const IndexType offset = Dimensions * index;
        for (IndexType j = 0; j < Dimensions; ++j)
            toAcc[j] -= featureTransformer.weights[offset + j];
        for (usize k = 0; k < SpellPSQTBuckets; ++k)
            toPsqtAcc[k] -= featureTransformer.psqtWeights[index * SpellPSQTBuckets + k];
    }

    for (const auto index : psqAdded)
    {
        const IndexType offset = Dimensions * index;
        for (IndexType j = 0; j < Dimensions; ++j)
            toAcc[j] += featureTransformer.weights[offset + j];
        for (usize k = 0; k < SpellPSQTBuckets; ++k)
            toPsqtAcc[k] += featureTransformer.psqtWeights[index * SpellPSQTBuckets + k];
    }

    for (const auto index : thrRemoved)
    {
        const IndexType offset = Dimensions * index;
        for (IndexType j = 0; j < Dimensions; ++j)
            toAcc[j] -= featureTransformer.threatWeights[offset + j];
        for (usize k = 0; k < SpellPSQTBuckets; ++k)
            toPsqtAcc[k] -= featureTransformer.threatPsqtWeights[index * SpellPSQTBuckets + k];
    }

    for (const auto index : thrAdded)
    {
        const IndexType offset = Dimensions * index;
        for (IndexType j = 0; j < Dimensions; ++j)
            toAcc[j] += featureTransformer.threatWeights[offset + j];
        for (usize k = 0; k < SpellPSQTBuckets; ++k)
            toPsqtAcc[k] += featureTransformer.threatPsqtWeights[index * SpellPSQTBuckets + k];
    }

#endif
}

template<bool Forward>
void update_accumulator_incremental_spell(Color                       perspective,
                                          const FeatureTransformerV2& featureTransformer,
                                          const Square                ksq,
                                          AccumulatorState&           target_state,
                                          const AccumulatorState&     computed) {

    assert(computed.computed[perspective]);
    assert(!target_state.computed[perspective]);

    SpellFeatureSet::IndexList  psqRemoved, psqAdded;
    ThreatFeatureSet::IndexList thrRemoved, thrAdded;

    const auto& dirtyPiece   = Forward ? target_state.dirtyPiece : computed.dirtyPiece;
    const auto& dirtyThreats = Forward ? target_state.dirtyThreats : computed.dirtyThreats;
    const auto& dirtySpell   = Forward ? target_state.dirtySpell : computed.dirtySpell;

    const auto* pfBase   = &featureTransformer.threatWeights[0];
    IndexType   pfStride = FeatureTransformerV2::OutputDimensions;

    if constexpr (Forward)
    {
        ThreatFeatureSet::append_changed_indices(perspective, ksq, dirtyThreats, thrRemoved,
                                                 thrAdded, pfBase, pfStride);
        SpellFeatureSet::append_changed_indices(perspective, ksq, dirtyPiece, dirtySpell,
                                                psqRemoved, psqAdded);
    }
    else
    {
        ThreatFeatureSet::append_changed_indices(perspective, ksq, dirtyThreats, thrAdded,
                                                 thrRemoved, pfBase, pfStride);
        SpellFeatureSet::append_changed_indices(perspective, ksq, dirtyPiece, dirtySpell,
                                                psqAdded, psqRemoved);
    }

    apply_combined_spell(perspective, featureTransformer, computed, target_state, psqAdded,
                         psqRemoved, thrAdded, thrRemoved);

    target_state.computed[perspective] = true;
}

// HalfKA-extended data comes from the Finny table entry (piece AND spell
// diffs), while the threats are rebuilt from the active threat features.
void update_accumulator_refresh_cache_spell(Color                       perspective,
                                            const FeatureTransformerV2& featureTransformer,
                                            const Position&             pos,
                                            AccumulatorState&           accumulator,
                                            Caches&                     cache) {
    constexpr auto Dimensions = FeatureTransformerV2::OutputDimensions;

    using Tiling [[maybe_unused]] = SIMDTiling<Dimensions, Dimensions, SpellPSQTBuckets>;

    const Square                ksq   = pos.square<KING>(perspective);
    auto&                       entry = cache[ksq][perspective];
    SpellFeatureSet::IndexList  removed, added;

    Bitboard changedBB = 0;
    for (Square sq = SQ_A1; sq <= SQ_H8; ++sq)
        changedBB |= Bitboard(entry.pieces[sq] != pos.piece_on(sq)) << sq;

    Bitboard removedBB = changedBB & entry.pieceBB;
    Bitboard addedBB   = changedBB & pos.pieces();

    while (removedBB)
    {
        Square sq = pop_lsb(removedBB);
        removed.push_back(SpellFeatureSet::make_index(perspective, sq, entry.pieces[sq], ksq));
    }
    while (addedBB)
    {
        Square sq = pop_lsb(addedBB);
        added.push_back(SpellFeatureSet::make_index(perspective, sq, pos.piece_on(sq), ksq));
    }

    entry.pieceBB = pos.pieces();
    entry.pieces  = pos.piece_array();

    // Spell-state diff vs the entry snapshot (also refreshes the snapshot)
    append_spell_diff(perspective, ksq, entry, pos, removed, added);

    ThreatFeatureSet::IndexList active;
    ThreatFeatureSet::append_active_indices(perspective, pos, active);

    accumulator.computed[perspective] = true;

#ifdef VECTOR
    vec_t      acc[Tiling::NumRegs];
    psqt_vec_t psqt[Tiling::NumPsqtRegs];

    const auto* weights       = &featureTransformer.weights[0];
    const auto* threatWeights = &featureTransformer.threatWeights[0];

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
            auto* column = reinterpret_cast<const vec_t*>(&weights[added[i] * Dimensions + tileOff]);
            for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                acc[k] = vec_add_16(acc[k], column[k]);
        }

        for (IndexType k = 0; k < Tiling::NumRegs; k++)
            vec_store(&entryTile[k], acc[k]);

        for (int i = 0; i < active.ssize(); ++i)
        {
            auto* column =
              reinterpret_cast<const vec_i8_t*>(&threatWeights[active[i] * Dimensions + tileOff]);

    #ifdef USE_NEON
            for (IndexType k = 0; k < Tiling::NumRegs; k += 2)
            {
                acc[k]     = vaddw_s8(acc[k], vget_low_s8(column[k / 2]));
                acc[k + 1] = vaddw_high_s8(acc[k + 1], column[k / 2]);
            }
    #else
            for (IndexType k = 0; k < Tiling::NumRegs; ++k)
                acc[k] = vec_add_16(acc[k], vec_convert_8_16(column[k]));
    #endif
        }

        for (IndexType k = 0; k < Tiling::NumRegs; k++)
            vec_store(&accTile[k], acc[k]);
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
            vec_store_psqt(&entryTilePsqt[k], psqt[k]);

        for (int i = 0; i < active.ssize(); ++i)
        {
            auto* columnPsqt = reinterpret_cast<const psqt_vec_t*>(
              &featureTransformer.threatPsqtWeights[active[i] * SpellPSQTBuckets + psqtTileOff]);
            for (usize k = 0; k < Tiling::NumPsqtRegs; ++k)
                psqt[k] = vec_add_psqt_32(psqt[k], columnPsqt[k]);
        }

        for (IndexType k = 0; k < Tiling::NumPsqtRegs; ++k)
            vec_store_psqt(&accTilePsqt[k], psqt[k]);
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

    // The refresh entry is up to date; now copy it to the accumulator we
    // were refreshing and add the (non-cached) threat features on top.
    accumulator.accumulation[perspective]          = entry.accumulation;
    accumulator.spellPsqtAccumulation[perspective] = entry.psqtAccumulation;

    for (const auto index : active)
    {
        const IndexType offset = Dimensions * index;

        for (IndexType j = 0; j < Dimensions; ++j)
            accumulator.accumulation[perspective][j] +=
              featureTransformer.threatWeights[offset + j];

        for (usize k = 0; k < SpellPSQTBuckets; ++k)
            accumulator.spellPsqtAccumulation[perspective][k] +=
              featureTransformer.threatPsqtWeights[index * SpellPSQTBuckets + k];
    }

#endif
}

}  // namespace

}  // namespace Stockfish::Eval::NNUE::SpellV2

// ---------------------------------------------------------------------------
// AccumulatorStack members for the v2 path (twin of the stock evaluate walk)
// ---------------------------------------------------------------------------

namespace Stockfish::Eval::NNUE {

void AccumulatorStack::evaluate_spell(const Position&                      pos,
                                      const SpellV2::FeatureTransformerV2& featureTransformer,
                                      SpellV2::Caches&                     cache) noexcept {
    evaluate_spell_side(WHITE, pos, featureTransformer, cache);
    evaluate_spell_side(BLACK, pos, featureTransformer, cache);
}

void AccumulatorStack::evaluate_spell_side(Color                                perspective,
                                           const Position&                      pos,
                                           const SpellV2::FeatureTransformerV2& featureTransformer,
                                           SpellV2::Caches& cache) noexcept {

    const auto last_usable_accum = find_last_usable_accumulator_spell(perspective);

    if (accumulators[last_usable_accum].computed[perspective])
    {
        const Square ksq = pos.square<KING>(perspective);
        for (usize next = last_usable_accum + 1; next < size; next++)
            SpellV2::update_accumulator_incremental_spell<true>(
              perspective, featureTransformer, ksq, accumulators[next], accumulators[next - 1]);
    }
    else
    {
        SpellV2::update_accumulator_refresh_cache_spell(perspective, featureTransformer, pos,
                                                        mut_latest(), cache);

        const Square ksq = pos.square<KING>(perspective);
        for (i64 next = i64(size) - 2; next >= i64(last_usable_accum); next--)
            SpellV2::update_accumulator_incremental_spell<false>(
              perspective, featureTransformer, ksq, accumulators[next], accumulators[next + 1]);
    }

    assert(latest().computed[perspective]);
}

// Find the earliest usable accumulator: either an already computed one or the
// state just before a change that requires a full refresh (own king move;
// null-move states never require one).
usize AccumulatorStack::find_last_usable_accumulator_spell(Color perspective) const noexcept {

    for (usize curr_idx = size - 1; curr_idx > 0; curr_idx--)
    {
        if (accumulators[curr_idx].computed[perspective])
            return curr_idx;

        if (SpellV2::SpellFeatureSet::requires_refresh(accumulators[curr_idx].dirtyPiece,
                                                       accumulators[curr_idx].dirtySpell,
                                                       perspective))
            return curr_idx;
    }

    return 0;
}

}  // namespace Stockfish::Eval::NNUE

namespace Stockfish::Eval::NNUE::SpellV2 {

// ---------------------------------------------------------------------------
// Feature transformer output (v2 twin of FeatureTransformer::transform)
// ---------------------------------------------------------------------------

i32 FeatureTransformerV2::transform(const Position&            pos,
                                    AccumulatorStack&          accumulatorStack,
                                    Caches&                    cache,
                                    TransformedFeatureType*    output,
                                    int                        bucket,
                                    NNZInfo<OutputDimensions>& nnzInfo) const {

    accumulatorStack.evaluate_spell(pos, *this, cache);
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

                uint8x16x2_t uzp =
                  vuzpq_u8(vreinterpretq_u8_u16(mul0), vreinterpretq_u8_u16(mul1));
                uint8x16_t pab    = vshrq_n_u8(uzp.val[1], 1);
                vec_t      result = reinterpret_cast<vec_t>(pab);
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
            BiasType sum1 =
              accumulation[static_cast<int>(perspectives[p])][j + HalfDimensions / 2];

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

int spell_bucket(const Position& pos) {
    const int mat = std::min(3, (pos.count<ALL_PIECES>() - 1) / 8);

    int potions = 0;
    for (Color c : {WHITE, BLACK})
        for (int sp = 0; sp < SPELL_NB; ++sp)
            potions += pos.spells_in_hand(c, SpellType(sp));

    return mat * 4 + std::min(3, potions / 4);
}

namespace {

void ensure_cache_generation(Caches& cache) {
    const u32 g = generation();
    if (cache.gen != g)
    {
        for (auto& entries1D : cache.entries)
            for (auto& entry : entries1D)
                entry.clear(theNet->featureTransformer.biases);
        cache.gen = g;
    }
}

}  // namespace

std::pair<Value, Value> raw_evaluate(const Position& pos, AccumulatorStack& stack, Caches& cache) {
    assert(loaded());

    ensure_cache_generation(cache);

    constexpr u64 alignment = CacheLineSize;
    alignas(alignment) TransformedFeatureType transformedFeatures[FeatureTransformerV2::BufferSize];

    ASSERT_ALIGNED(transformedFeatures, alignment);

    NNZInfo<L1> nnzInfo;

    const int  bucket = spell_bucket(pos);
    const auto psqt   = theNet->featureTransformer.transform(pos, stack, cache,
                                                             transformedFeatures, bucket, nnzInfo);
    const auto positional = theNet->stacks[bucket].propagate(transformedFeatures, nnzInfo);

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
        const Square ksq = pos.square<KING>(perspective);

        SpellFeatureSet::IndexList psq;

        Bitboard bb = pos.pieces();
        while (bb)
        {
            const Square sq = pop_lsb(bb);
            psq.push_back(SpellFeatureSet::make_index(perspective, sq, pos.piece_on(sq), ksq));
        }
        SpellFeatureSet::append_active_spell(perspective, ksq, pos, psq);

        ThreatFeatureSet::IndexList thr;
        ThreatFeatureSet::append_active_indices(perspective, pos, thr);

        std::vector<IndexType> sortedPsq(psq.begin(), psq.end());
        std::sort(sortedPsq.begin(), sortedPsq.end());
        std::vector<IndexType> sortedThr(thr.begin(), thr.end());
        std::sort(sortedThr.begin(), sortedThr.end());

        os << "perspective " << (perspective == WHITE ? "w" : "b") << " psq";
        for (IndexType i : sortedPsq)
            os << ' ' << i;
        os << "\n";
        os << "perspective " << (perspective == WHITE ? "w" : "b") << " threats";
        for (IndexType i : sortedThr)
            os << ' ' << i;
        os << "\n";
    }
}

}  // namespace Stockfish::Eval::NNUE::SpellV2
