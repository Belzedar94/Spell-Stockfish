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

#include "datagen.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <ios>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "engine.h"
#include "misc.h"
#include "movegen.h"
#include "position.h"
#include "score.h"
#include "uci.h"
#include "ucioption.h"

namespace Stockfish::Datagen {

namespace {

constexpr usize Run7HeaderSize    = 32;
constexpr usize Run7RecordSize    = 44;
constexpr int   MateTarget        = 32000;
constexpr u64   ResumeMetaVersion = 1;

using Run7Record = std::array<u8, Run7RecordSize>;

struct Params {
    std::filesystem::path book;
    std::filesystem::path out;
    u64                   nodes             = 40000;
    u64                   count             = 10000;
    int                   randomMultiPv     = 4;
    int                   randomMultiPvDiff = 100;
    int                   randomMoveCount   = 8;
    int                   randomMoveMinPly  = 1;
    int                   randomMoveMaxPly  = 20;
    int                   writeMinPly       = 5;
    int                   evalLimit         = 10000;
    int                   evalDiffLimit     = 32000;
    bool                  filterCaptures    = true;
    bool                  filterChecks      = true;
    bool                  filterPromotions  = false;
    usize                 threads           = 1;
    u64                   seed              = 1;
    usize                 debugSample       = 0;
    bool                  resume            = false;
    u64                   resumeNumber      = 0;
};

struct ResumeMetadata {
    u64         metaVersion = ResumeMetaVersion;
    std::string command;
    std::string lastCommand;
    std::string format        = "run7";
    u64         formatVersion = 1;
    u64         recordSize    = Run7RecordSize;
    std::string bookPath;
    u64         bookSize = 0;
    std::string bookHash;
    u64         count             = 0;
    u64         nodes             = 0;
    int         randomMultiPv     = 0;
    int         randomMultiPvDiff = 0;
    int         randomMoveCount   = 0;
    int         randomMoveMinPly  = 0;
    int         randomMoveMaxPly  = 0;
    int         writeMinPly       = 0;
    int         evalLimit         = 0;
    int         evalDiffLimit     = 0;
    bool        filterCaptures    = false;
    bool        filterChecks      = false;
    bool        filterPromotions  = false;
    usize       initialThreads    = 0;
    u64         seed              = 0;
    usize       debugSample       = 0;
    u64         resumeCount       = 0;
};

struct BufferedRecord {
    Run7Record  record{};
    std::string fen;
    int         score = 0;
};

struct Candidate {
    Move move  = Move::none();
    int  score = 0;
    bool valid = false;
};

struct SearchCapture {
    explicit SearchCapture(usize multiPv) :
        lines(multiPv) {}

    void reset(const Position& p) {
        pos = &p;
        std::fill(lines.begin(), lines.end(), Candidate{});
    }

    void update(const Engine::InfoFull& info) {
        if (!pos || info.multiPV == 0 || info.multiPV > lines.size())
            return;

        std::istringstream pv(std::string(info.pv));
        std::string        moveText;
        pv >> moveText;
        if (moveText.empty())
            return;

        Candidate& candidate = lines[info.multiPV - 1];
        candidate.move       = UCIEngine::to_move(*pos, moveText);
        candidate.score      = score_to_cp(info.score);
        candidate.valid      = bool(candidate.move);
    }

    static int score_to_cp(const Score& score) {
        if (score.is<Score::InternalUnits>())
            return std::clamp(score.get<Score::InternalUnits>().value, -MateTarget, MateTarget);
        if (score.is<Score::Mate>())
            return score.get<Score::Mate>().plies >= 0 ? MateTarget : -MateTarget;

        const auto tb = score.get<Score::Tablebase>();
        return tb.win ? 20000 - std::abs(tb.plies) : -20000 + std::abs(tb.plies);
    }

    const Position*        pos = nullptr;
    std::vector<Candidate> lines;
};

struct WorkerStats {
    usize              shardId         = 0;
    u64                target          = 0;
    u64                records         = 0;
    u64                sourcePositions = 0;
    u64                games           = 0;
    u64                whiteWins       = 0;
    u64                blackWins       = 0;
    u64                draws           = 0;
    u64                seed            = 0;
    usize              debugTarget     = 0;
    double             seconds         = 0.0;
    std::map<u64, u64> recordsPerGame;
    std::string        error;
};

struct ShardInfo {
    usize                 id = 0;
    std::filesystem::path path;
    u64                   records         = 0;
    u64                   sourcePositions = 0;
};

struct BitWriter {
    explicit BitWriter(u8* bytes) :
        data(bytes) {}

    void put(u64 value, int bits) {
        for (int i = 0; i < bits; ++i, ++cursor)
            if (value & (u64(1) << i))
                data[cursor >> 3] |= u8(1u << (cursor & 7));
    }

    void put_signed(int value, int bits) { put(u64(value) & ((u64(1) << bits) - 1), bits); }

    u8* data;
    int cursor = 0;
};

void put_le(u8* destination, u64 value, usize bytes) {
    for (usize i = 0; i < bytes; ++i)
        destination[i] = u8(value >> (8 * i));
}

u64 get_le(const u8* source, usize bytes) {
    u64 value = 0;
    for (usize i = 0; i < bytes; ++i)
        value |= u64(source[i]) << (8 * i);
    return value;
}

void write_header(std::ostream& file, u64 count, u64 sourceCount, u64 flags = 0) {
    std::array<u8, Run7HeaderSize> header{};
    header[0] = 'R';
    header[1] = 'U';
    header[2] = 'N';
    header[3] = '7';
    put_le(header.data() + 4, 1, 2);
    put_le(header.data() + 6, Run7RecordSize, 2);
    put_le(header.data() + 8, count, 8);
    put_le(header.data() + 16, sourceCount, 8);
    put_le(header.data() + 24, flags, 8);
    file.write(reinterpret_cast<const char*>(header.data()), std::streamsize(header.size()));
}

bool read_header(std::istream& file, u64& count, u64& sourceCount, u64& flags, std::string& error) {
    std::array<u8, Run7HeaderSize> header{};
    file.read(reinterpret_cast<char*>(header.data()), std::streamsize(header.size()));
    if (file.gcount() != std::streamsize(header.size()))
    {
        error = "truncated run7 header";
        return false;
    }
    if (header[0] != 'R' || header[1] != 'U' || header[2] != 'N' || header[3] != '7'
        || get_le(header.data() + 4, 2) != 1 || get_le(header.data() + 6, 2) != Run7RecordSize)
    {
        error = "unsupported run7 header";
        return false;
    }
    count       = get_le(header.data() + 8, 8);
    sourceCount = get_le(header.data() + 16, 8);
    flags       = get_le(header.data() + 24, 8);
    return true;
}

void overwrite_bits(u8* data, int cursor, u64 value, int bits) {
    for (int i = 0; i < bits; ++i, ++cursor)
    {
        const u8 mask = u8(1u << (cursor & 7));
        data[cursor >> 3] &= u8(~mask);
        if (value & (u64(1) << i))
            data[cursor >> 3] |= mask;
    }
}

void set_result(Run7Record& record, int whiteResult) {
    u8*        metadata  = record.data() + 24;
    const bool stmBlack  = metadata[0] & 1;
    const int  stmResult = whiteResult == 0 ? 0 : (whiteResult > 0) != stmBlack ? 1 : -1;
    overwrite_bits(metadata, 145, u64(stmResult + 1), 2);
}

Run7Record pack_record(const Position& pos, int score, Move move) {
    Run7Record record{};

    u64 occupancy = 0;
    int occupied  = 0;
    for (Square square = SQ_A1; square <= SQ_H8; ++square)
    {
        const Piece piece = pos.piece_on(square);
        if (piece == NO_PIECE)
            continue;

        occupancy |= u64(1) << int(square);
        const int nibble = color_of(piece) == WHITE ? int(piece) : int(piece) - 2;
        record[8 + (occupied >> 1)] |= u8(nibble << (4 * (occupied & 1)));
        ++occupied;
    }
    put_le(record.data(), occupancy, 8);

    BitWriter bits(record.data() + 24);
    bits.put(pos.side_to_move() == BLACK, 1);

    int castling = 0;
    castling |= pos.can_castle(WHITE_OO) ? 1 : 0;
    castling |= pos.can_castle(WHITE_OOO) ? 2 : 0;
    castling |= pos.can_castle(BLACK_OO) ? 4 : 0;
    castling |= pos.can_castle(BLACK_OOO) ? 8 : 0;
    bits.put(castling, 4);

    bits.put(pos.ep_square() == SQ_NONE ? 0 : int(pos.ep_square()) + 1, 7);
    bits.put(pos.rule50_count(), 7);
    const int fullmove = 1 + (pos.game_ply() - (pos.side_to_move() == BLACK)) / 2;
    bits.put(fullmove, 16);

    bits.put(pos.spells_in_hand(WHITE, SPELL_FREEZE), 3);
    bits.put(pos.spells_in_hand(WHITE, SPELL_JUMP), 2);
    bits.put(pos.spells_in_hand(BLACK, SPELL_FREEZE), 3);
    bits.put(pos.spells_in_hand(BLACK, SPELL_JUMP), 2);

    for (Color color : {WHITE, BLACK})
        for (SpellType spell : {SPELL_FREEZE, SPELL_JUMP})
            bits.put(pos.spell_cooldown(color, spell), 2);

    for (Color color : {WHITE, BLACK})
        for (SpellType spell : {SPELL_FREEZE, SPELL_JUMP})
        {
            const Square gate = pos.spell_gate(color, spell);
            bits.put(gate == SQ_NONE ? 0 : int(gate) + 1, 7);
        }

    bits.put_signed(std::clamp(score, -MateTarget, MateTarget), 16);
    bits.put(move.raw(), 32);
    bits.put(pos.game_ply(), 16);
    bits.put(1, 2);  // Draw placeholder, patched after the real game ends.

    assert(bits.cursor == 147);
    assert(occupied <= 32);
    return record;
}

u64 bounded_rand(PRNG& rng, u64 bound) {
    assert(bound > 0);
    const u64 threshold = u64(-bound) % bound;
    u64       value;
    do
        value = rng.rand<u64>();
    while (value < threshold);
    return value % bound;
}

u64 splitmix_seed(u64 seed, u64 resumeNumber, usize threadId) {
    assert(resumeNumber <= std::numeric_limits<std::uint32_t>::max());
    assert(u64(threadId) <= std::numeric_limits<std::uint32_t>::max());
    const u64 stream = (resumeNumber << 32) | u64(threadId);
    u64       z      = seed + 0x9E3779B97F4A7C15ULL * (stream + 1);
    z                = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z                = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z ^= z >> 31;
    return z ? z : 1;
}

std::vector<u8> random_move_flags(const Params& params, PRNG& rng) {
    std::vector<int> candidates;
    candidates.reserve(usize(params.randomMoveMaxPly - params.randomMoveMinPly + 1));
    for (int ply = std::max(0, params.randomMoveMinPly - 1); ply < params.randomMoveMaxPly; ++ply)
        candidates.push_back(ply);

    const int selected = std::min(params.randomMoveCount, int(candidates.size()));
    for (int i = 0; i < selected; ++i)
    {
        const int j = i + int(bounded_rand(rng, u64(candidates.size() - usize(i))));
        std::swap(candidates[i], candidates[j]);
    }

    std::vector<u8> flags(usize(params.randomMoveMaxPly), 0);
    for (int i = 0; i < selected; ++i)
        flags[usize(candidates[i])] = 1;
    return flags;
}

bool captures_king(const Position& pos, Move move) {
    return move && pos.piece_on(move.to_sq()) == make_piece(~pos.side_to_move(), KING);
}

bool capture_filter_matches(const Position& pos, Move move) {
    return pos.capture(move) || (move.is_spell() && pos.capture(move.base_move()));
}

std::filesystem::path with_suffix(std::filesystem::path path, const std::string& suffix) {
    path += suffix;
    return path;
}

std::string trim(std::string value) {
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

std::filesystem::path normalized_path(const std::filesystem::path& path) {
    std::error_code ec;
    auto            normalized = std::filesystem::weakly_canonical(path, ec);
    if (!ec)
        return normalized;
    normalized = std::filesystem::absolute(path, ec);
    return ec ? path.lexically_normal() : normalized.lexically_normal();
}

std::string portable_path(const std::filesystem::path& path) { return path.generic_u8string(); }

bool fast_file_hash(const std::filesystem::path& path,
                    u64&                         size,
                    std::string&                 hash,
                    std::string&                 error) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        error = "cannot hash book " + path.string();
        return false;
    }

    constexpr u64                 FnvOffset = 14695981039346656037ULL;
    constexpr u64                 FnvPrime  = 1099511628211ULL;
    std::array<char, 1024 * 1024> buffer{};
    u64                           value = FnvOffset;
    size                                = 0;
    while (file)
    {
        file.read(buffer.data(), std::streamsize(buffer.size()));
        const auto bytes = file.gcount();
        for (std::streamsize i = 0; i < bytes; ++i)
        {
            value ^= static_cast<unsigned char>(buffer[usize(i)]);
            value *= FnvPrime;
        }
        size += u64(bytes);
    }
    if (!file.eof())
    {
        error = "failed while hashing book " + path.string();
        return false;
    }

    std::ostringstream text;
    text << "fnv1a64:" << std::hex << std::uppercase << std::setw(16) << std::setfill('0') << value;
    hash = text.str();
    return true;
}

std::string normalized_command(const Params&                params,
                               const std::filesystem::path& book,
                               const std::filesystem::path& out) {
    std::ostringstream command;
    command << "datagen book " << std::quoted(portable_path(book)) << " nodes " << params.nodes
            << " count " << params.count << " random_multi_pv " << params.randomMultiPv
            << " random_multi_pv_diff " << params.randomMultiPvDiff << " random_move_count "
            << params.randomMoveCount << " random_move_min_ply " << params.randomMoveMinPly
            << " random_move_max_ply " << params.randomMoveMaxPly << " write_min_ply "
            << params.writeMinPly << " eval_limit " << params.evalLimit << " eval_diff_limit "
            << params.evalDiffLimit << " filter_captures " << int(params.filterCaptures)
            << " filter_checks " << int(params.filterChecks) << " filter_promotions "
            << int(params.filterPromotions) << " threads " << params.threads << " seed "
            << params.seed << " out " << std::quoted(portable_path(out)) << " --debug-sample "
            << params.debugSample;
    if (params.resume)
        command << " --resume";
    return command.str();
}

ResumeMetadata make_resume_metadata(const Params&                params,
                                    const std::filesystem::path& book,
                                    const std::filesystem::path& out,
                                    u64                          bookSize,
                                    const std::string&           bookHash) {
    ResumeMetadata metadata;
    metadata.command           = normalized_command(params, book, out);
    metadata.lastCommand       = metadata.command;
    metadata.bookPath          = portable_path(book);
    metadata.bookSize          = bookSize;
    metadata.bookHash          = bookHash;
    metadata.count             = params.count;
    metadata.nodes             = params.nodes;
    metadata.randomMultiPv     = params.randomMultiPv;
    metadata.randomMultiPvDiff = params.randomMultiPvDiff;
    metadata.randomMoveCount   = params.randomMoveCount;
    metadata.randomMoveMinPly  = params.randomMoveMinPly;
    metadata.randomMoveMaxPly  = params.randomMoveMaxPly;
    metadata.writeMinPly       = params.writeMinPly;
    metadata.evalLimit         = params.evalLimit;
    metadata.evalDiffLimit     = params.evalDiffLimit;
    metadata.filterCaptures    = params.filterCaptures;
    metadata.filterChecks      = params.filterChecks;
    metadata.filterPromotions  = params.filterPromotions;
    metadata.initialThreads    = params.threads;
    metadata.seed              = params.seed;
    metadata.debugSample       = params.debugSample;
    return metadata;
}

bool write_resume_metadata(const std::filesystem::path& out,
                           const ResumeMetadata&        metadata,
                           std::string&                 error) {
    const auto path      = with_suffix(out, ".meta");
    const auto temporary = with_suffix(out, ".meta.tmp");
    const auto previous  = with_suffix(out, ".meta.prev");

    std::ofstream file(temporary, std::ios::trunc);
    if (!file)
    {
        error = "cannot write resume metadata " + temporary.string();
        return false;
    }
    file << "schema " << std::quoted("spell-datagen-resume") << '\n'
         << "meta_version " << metadata.metaVersion << '\n'
         << "command " << std::quoted(metadata.command) << '\n'
         << "last_command " << std::quoted(metadata.lastCommand) << '\n'
         << "format " << std::quoted(metadata.format) << '\n'
         << "format_version " << metadata.formatVersion << '\n'
         << "record_size " << metadata.recordSize << '\n'
         << "book_path " << std::quoted(metadata.bookPath) << '\n'
         << "book_size " << metadata.bookSize << '\n'
         << "book_hash " << std::quoted(metadata.bookHash) << '\n'
         << "count " << metadata.count << '\n'
         << "nodes " << metadata.nodes << '\n'
         << "random_multi_pv " << metadata.randomMultiPv << '\n'
         << "random_multi_pv_diff " << metadata.randomMultiPvDiff << '\n'
         << "random_move_count " << metadata.randomMoveCount << '\n'
         << "random_move_min_ply " << metadata.randomMoveMinPly << '\n'
         << "random_move_max_ply " << metadata.randomMoveMaxPly << '\n'
         << "write_min_ply " << metadata.writeMinPly << '\n'
         << "eval_limit " << metadata.evalLimit << '\n'
         << "eval_diff_limit " << metadata.evalDiffLimit << '\n'
         << "filter_captures " << int(metadata.filterCaptures) << '\n'
         << "filter_checks " << int(metadata.filterChecks) << '\n'
         << "filter_promotions " << int(metadata.filterPromotions) << '\n'
         << "initial_threads " << metadata.initialThreads << '\n'
         << "seed " << metadata.seed << '\n'
         << "debug_sample " << metadata.debugSample << '\n'
         << "resume_count " << metadata.resumeCount << '\n';
    file.close();
    if (!file)
    {
        error = "failed while writing resume metadata " + temporary.string();
        return false;
    }

    std::error_code ec;
    std::filesystem::remove(previous, ec);
    ec.clear();
    const bool hadCurrent = std::filesystem::exists(path, ec);
    if (ec)
    {
        error = "cannot inspect resume metadata: " + ec.message();
        return false;
    }
    if (hadCurrent)
    {
        std::filesystem::rename(path, previous, ec);
        if (ec)
        {
            error = "cannot rotate resume metadata: " + ec.message();
            return false;
        }
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec)
    {
        if (hadCurrent)
        {
            std::error_code restoreError;
            std::filesystem::rename(previous, path, restoreError);
        }
        error = "cannot publish resume metadata: " + ec.message();
        return false;
    }
    std::filesystem::remove(previous, ec);
    return true;
}

bool parse_resume_metadata_file(const std::filesystem::path& path,
                                ResumeMetadata&              metadata,
                                std::string&                 error) {
    std::ifstream file(path);
    if (!file)
    {
        error = "cannot open " + path.string();
        return false;
    }

    std::set<std::string> seen;
    std::string           line;
    usize                 lineNumber = 0;
    while (std::getline(file, line))
    {
        ++lineNumber;
        std::istringstream input(line);
        std::string        key;
        if (!(input >> key))
            continue;
        if (!seen.insert(key).second)
        {
            error = "duplicate resume metadata field '" + key + "'";
            return false;
        }

        bool ok = true;
        if (key == "schema")
        {
            std::string schema;
            ok = bool(input >> std::quoted(schema)) && schema == "spell-datagen-resume";
        }
        else if (key == "meta_version")
            ok = bool(input >> metadata.metaVersion);
        else if (key == "command")
            ok = bool(input >> std::quoted(metadata.command));
        else if (key == "last_command")
            ok = bool(input >> std::quoted(metadata.lastCommand));
        else if (key == "format")
            ok = bool(input >> std::quoted(metadata.format));
        else if (key == "format_version")
            ok = bool(input >> metadata.formatVersion);
        else if (key == "record_size")
            ok = bool(input >> metadata.recordSize);
        else if (key == "book_path")
            ok = bool(input >> std::quoted(metadata.bookPath));
        else if (key == "book_size")
            ok = bool(input >> metadata.bookSize);
        else if (key == "book_hash")
            ok = bool(input >> std::quoted(metadata.bookHash));
        else if (key == "count")
            ok = bool(input >> metadata.count);
        else if (key == "nodes")
            ok = bool(input >> metadata.nodes);
        else if (key == "random_multi_pv")
            ok = bool(input >> metadata.randomMultiPv);
        else if (key == "random_multi_pv_diff")
            ok = bool(input >> metadata.randomMultiPvDiff);
        else if (key == "random_move_count")
            ok = bool(input >> metadata.randomMoveCount);
        else if (key == "random_move_min_ply")
            ok = bool(input >> metadata.randomMoveMinPly);
        else if (key == "random_move_max_ply")
            ok = bool(input >> metadata.randomMoveMaxPly);
        else if (key == "write_min_ply")
            ok = bool(input >> metadata.writeMinPly);
        else if (key == "eval_limit")
            ok = bool(input >> metadata.evalLimit);
        else if (key == "eval_diff_limit")
            ok = bool(input >> metadata.evalDiffLimit);
        else if (key == "filter_captures" || key == "filter_checks" || key == "filter_promotions")
        {
            int value = -1;
            ok        = bool(input >> value) && (value == 0 || value == 1);
            if (ok && key == "filter_captures")
                metadata.filterCaptures = value;
            else if (ok && key == "filter_checks")
                metadata.filterChecks = value;
            else if (ok)
                metadata.filterPromotions = value;
        }
        else if (key == "initial_threads")
            ok = bool(input >> metadata.initialThreads);
        else if (key == "seed")
            ok = bool(input >> metadata.seed);
        else if (key == "debug_sample")
            ok = bool(input >> metadata.debugSample);
        else if (key == "resume_count")
            ok = bool(input >> metadata.resumeCount);
        else
        {
            error = "unknown resume metadata field '" + key + "'";
            return false;
        }

        input >> std::ws;
        if (!ok || !input.eof())
        {
            error =
              "invalid resume metadata field '" + key + "' on line " + std::to_string(lineNumber);
            return false;
        }
    }
    if (!file.eof())
    {
        error = "failed while reading " + path.string();
        return false;
    }

    static const std::array<const char*, 27> Required = {"schema",
                                                         "meta_version",
                                                         "command",
                                                         "last_command",
                                                         "format",
                                                         "format_version",
                                                         "record_size",
                                                         "book_path",
                                                         "book_size",
                                                         "book_hash",
                                                         "count",
                                                         "nodes",
                                                         "random_multi_pv",
                                                         "random_multi_pv_diff",
                                                         "random_move_count",
                                                         "random_move_min_ply",
                                                         "random_move_max_ply",
                                                         "write_min_ply",
                                                         "eval_limit",
                                                         "eval_diff_limit",
                                                         "filter_captures",
                                                         "filter_checks",
                                                         "filter_promotions",
                                                         "initial_threads",
                                                         "seed",
                                                         "debug_sample",
                                                         "resume_count"};
    for (const char* key : Required)
        if (!seen.count(key))
        {
            error = "resume metadata is missing field '" + std::string(key) + "'";
            return false;
        }
    return true;
}

bool load_resume_metadata(const std::filesystem::path& out,
                          ResumeMetadata&              metadata,
                          std::string&                 error) {
    const auto       path       = with_suffix(out, ".meta");
    const auto       temporary  = with_suffix(out, ".meta.tmp");
    const auto       previous   = with_suffix(out, ".meta.prev");
    const std::array candidates = {path, temporary, previous};
    std::string      lastError;
    for (const auto& candidate : candidates)
    {
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec) || ec)
            continue;
        ResumeMetadata parsed;
        if (!parse_resume_metadata_file(candidate, parsed, lastError))
            continue;

        if (candidate != path)
        {
            std::filesystem::remove(path, ec);
            ec.clear();
            std::filesystem::rename(candidate, path, ec);
            if (ec)
            {
                error = "cannot recover resume metadata: " + ec.message();
                return false;
            }
        }
        metadata = std::move(parsed);
        std::filesystem::remove(temporary, ec);
        std::filesystem::remove(previous, ec);
        return true;
    }
    error = "cannot read valid resume metadata " + path.string();
    if (!lastError.empty())
        error += ": " + lastError;
    return false;
}

template<typename T>
bool resume_value_matches(const char*  field,
                          const T&     requested,
                          const T&     stored,
                          std::string& error) {
    if (requested == stored)
        return true;
    std::ostringstream message;
    message << "resume metadata mismatch for " << field << ": requested " << requested
            << ", stored " << stored;
    error = message.str();
    return false;
}

bool validate_resume_metadata(const Params&                params,
                              const ResumeMetadata&        metadata,
                              const std::filesystem::path& book,
                              u64                          bookSize,
                              const std::string&           bookHash,
                              std::string&                 error) {
    if (metadata.metaVersion != ResumeMetaVersion || metadata.format != "run7"
        || metadata.formatVersion != 1 || metadata.recordSize != Run7RecordSize)
    {
        error = "resume metadata uses an unsupported format/version";
        return false;
    }
    if (!resume_value_matches("count", params.count, metadata.count, error)
        || !resume_value_matches("seed", params.seed, metadata.seed, error)
        || !resume_value_matches("nodes", params.nodes, metadata.nodes, error)
        || !resume_value_matches("random_multi_pv", params.randomMultiPv, metadata.randomMultiPv,
                                 error)
        || !resume_value_matches("random_multi_pv_diff", params.randomMultiPvDiff,
                                 metadata.randomMultiPvDiff, error)
        || !resume_value_matches("random_move_count", params.randomMoveCount,
                                 metadata.randomMoveCount, error)
        || !resume_value_matches("random_move_min_ply", params.randomMoveMinPly,
                                 metadata.randomMoveMinPly, error)
        || !resume_value_matches("random_move_max_ply", params.randomMoveMaxPly,
                                 metadata.randomMoveMaxPly, error)
        || !resume_value_matches("write_min_ply", params.writeMinPly, metadata.writeMinPly, error)
        || !resume_value_matches("eval_limit", params.evalLimit, metadata.evalLimit, error)
        || !resume_value_matches("eval_diff_limit", params.evalDiffLimit, metadata.evalDiffLimit,
                                 error)
        || !resume_value_matches("filter_captures", params.filterCaptures, metadata.filterCaptures,
                                 error)
        || !resume_value_matches("filter_checks", params.filterChecks, metadata.filterChecks, error)
        || !resume_value_matches("filter_promotions", params.filterPromotions,
                                 metadata.filterPromotions, error)
        || !resume_value_matches("debug_sample", params.debugSample, metadata.debugSample, error))
        return false;

    std::error_code ec;
    const bool sameBook = std::filesystem::equivalent(book, path_from_utf8(metadata.bookPath), ec);
    if (ec || !sameBook)
    {
        error = "resume metadata mismatch for book path: requested " + portable_path(book)
              + ", stored " + metadata.bookPath;
        return false;
    }
    if (bookSize != metadata.bookSize || bookHash != metadata.bookHash)
    {
        error = "resume metadata mismatch for book hash: requested " + bookHash + " ("
              + std::to_string(bookSize) + " bytes), stored " + metadata.bookHash + " ("
              + std::to_string(metadata.bookSize) + " bytes)";
        return false;
    }
    if (metadata.resumeCount >= std::numeric_limits<std::uint32_t>::max())
    {
        error = "resume counter exhausted";
        return false;
    }
    return true;
}

bool read_path(std::istream& args, std::filesystem::path& value) {
    std::string text;
    if (!(args >> std::quoted(text)))
        return false;
    value = path_from_utf8(text);
    return true;
}

template<typename T>
bool read_value(std::istream& args, T& value) {
    return bool(args >> value);
}

bool parse_params(std::istream& args, Params& params, std::string& error) {
    params.seed = u64(now());
    if (!params.seed)
        params.seed = 1;

    std::string token;
    while (args >> token)
    {
        bool ok = true;
        if (token == "book")
            ok = read_path(args, params.book);
        else if (token == "nodes")
            ok = read_value(args, params.nodes);
        else if (token == "count")
            ok = read_value(args, params.count);
        else if (token == "random_multi_pv")
            ok = read_value(args, params.randomMultiPv);
        else if (token == "random_multi_pv_diff")
            ok = read_value(args, params.randomMultiPvDiff);
        else if (token == "random_move_count")
            ok = read_value(args, params.randomMoveCount);
        else if (token == "random_move_min_ply")
            ok = read_value(args, params.randomMoveMinPly);
        else if (token == "random_move_max_ply")
            ok = read_value(args, params.randomMoveMaxPly);
        else if (token == "write_min_ply")
            ok = read_value(args, params.writeMinPly);
        else if (token == "eval_limit")
            ok = read_value(args, params.evalLimit);
        else if (token == "eval_diff_limit")
            ok = read_value(args, params.evalDiffLimit);
        else if (token == "filter_captures" || token == "filter_checks"
                 || token == "filter_promotions")
        {
            int flag = -1;
            ok       = read_value(args, flag) && (flag == 0 || flag == 1);
            if (ok && token == "filter_captures")
                params.filterCaptures = flag;
            else if (ok && token == "filter_checks")
                params.filterChecks = flag;
            else if (ok)
                params.filterPromotions = flag;
        }
        else if (token == "threads")
            ok = read_value(args, params.threads);
        else if (token == "seed")
            ok = read_value(args, params.seed);
        else if (token == "out")
            ok = read_path(args, params.out);
        else if (token == "--debug-sample")
            ok = read_value(args, params.debugSample);
        else if (token == "--resume")
            params.resume = true;
        else
        {
            error = "unknown option '" + token + "'";
            return false;
        }

        if (!ok)
        {
            error = "invalid or missing value for '" + token + "'";
            return false;
        }
    }

    if (params.book.empty() || params.out.empty())
        error = "book and out are required";
    else if (!params.nodes || !params.count)
        error = "nodes and count must be greater than zero";
    else if (params.count > (std::numeric_limits<u64>::max() - Run7HeaderSize) / Run7RecordSize)
        error = "count is too large for the run7 file size";
    else if (!params.threads)
        error = "threads must be greater than zero";
    else if (u64(params.threads) > std::numeric_limits<std::uint32_t>::max())
        error = "threads exceed the resume stream-id limit";
    else if (params.randomMultiPv < 1 || params.randomMultiPv > MAX_MOVES)
        error = "random_multi_pv must be in [1, MAX_MOVES]";
    else if (params.randomMultiPvDiff < 0)
        error = "random_multi_pv_diff must be non-negative";
    else if (params.randomMoveCount < 0 || params.randomMoveMinPly < 1
             || params.randomMoveMaxPly < params.randomMoveMinPly)
        error = "invalid random move count/range";
    else if (params.writeMinPly < 0 || params.evalLimit < 0 || params.evalLimit > MateTarget
             || params.evalDiffLimit < 0 || params.evalDiffLimit > MateTarget)
        error = "invalid write/eval limit";
    else if (params.debugSample > params.count)
        error = "--debug-sample cannot exceed count";
    else if (!params.seed)
        error = "seed must be non-zero";

    return error.empty();
}

bool load_book(const std::filesystem::path& path,
               std::vector<std::string>&    positions,
               std::string&                 error) {
    std::ifstream file(path);
    if (!file)
    {
        error = "cannot open book " + path.string();
        return false;
    }

    std::string line;
    while (std::getline(file, line))
    {
        if (positions.empty() && line.size() >= 3 && u8(line[0]) == 0xEF && u8(line[1]) == 0xBB
            && u8(line[2]) == 0xBF)
            line.erase(0, 3);
        line = trim(std::move(line));
        if (!line.empty() && line[0] != '#')
            positions.push_back(std::move(line));
    }

    if (positions.empty())
    {
        error = "book contains no positions";
        return false;
    }
    return true;
}

bool parse_shard_id(const std::string& filename, const std::string& prefix, usize& id) {
    if (filename.size() <= prefix.size() || filename.compare(0, prefix.size(), prefix) != 0)
        return false;
    const std::string_view suffix(filename.data() + prefix.size(), filename.size() - prefix.size());
    u64                    value = 0;
    for (char c : suffix)
    {
        if (c < '0' || c > '9')
            return false;
        const u64 digit = u64(c - '0');
        if (value > (std::numeric_limits<u64>::max() - digit) / 10)
            return false;
        value = value * 10 + digit;
    }
    if (value > std::numeric_limits<usize>::max())
        return false;
    id = usize(value);
    return true;
}

bool inspect_shard(ShardInfo& shard, bool sanitize, std::string& error) {
    std::error_code ec;
    u64             size = std::filesystem::file_size(shard.path, ec);
    if (ec)
    {
        error = "cannot size shard " + shard.path.string();
        return false;
    }
    if (size < Run7HeaderSize)
    {
        if (!sanitize)
        {
            error = "truncated shard header in " + shard.path.string();
            return false;
        }
        std::ofstream reset(shard.path, std::ios::binary | std::ios::trunc);
        write_header(reset, 0, 0);
        reset.close();
        if (!reset)
        {
            error = "cannot reset header-truncated shard " + shard.path.string();
            return false;
        }
        shard.records         = 0;
        shard.sourcePositions = 0;
        sync_cout << "info string datagen resume: reset header-truncated shard "
                  << shard.path.string() << " (" << size << " byte(s)) as empty" << sync_endl;
        return true;
    }

    std::ifstream input(shard.path, std::ios::binary);
    if (!input)
    {
        error = "cannot read shard " + shard.path.string();
        return false;
    }
    u64         declaredCount = 0;
    u64         flags         = 0;
    std::string headerError;
    if (!read_header(input, declaredCount, shard.sourcePositions, flags, headerError))
    {
        error = "invalid shard " + shard.path.string() + ": " + headerError;
        return false;
    }
    if (flags)
    {
        error = "invalid shard " + shard.path.string() + ": unsupported non-zero flags";
        return false;
    }
    input.close();

    u64       payload = size - Run7HeaderSize;
    const u64 tail    = payload % Run7RecordSize;
    if (tail)
    {
        if (!sanitize)
        {
            error = "truncated shard " + shard.path.string() + " has " + std::to_string(tail)
                  + " trailing byte(s)";
            return false;
        }
        const u64 cleanSize = size - tail;
        std::filesystem::resize_file(shard.path, cleanSize, ec);
        if (ec)
        {
            error = "cannot truncate shard " + shard.path.string() + ": " + ec.message();
            return false;
        }
        payload -= tail;
        sync_cout << "info string datagen resume: truncated shard " << shard.path.string() << " by "
                  << tail << " byte(s) to the last complete 44-byte record" << sync_endl;
    }
    shard.records = payload / Run7RecordSize;

    const bool countMismatch = declaredCount != shard.records;
    const bool sourceMissing = shard.sourcePositions < shard.records;
    if (countMismatch || sourceMissing)
    {
        if (!sanitize)
        {
            error = "shard header count/source mismatch in " + shard.path.string();
            return false;
        }
        if (sourceMissing)
        {
            shard.sourcePositions = shard.records;
            sync_cout << "info string datagen resume: recovered a conservative source-position "
                         "lower bound of "
                      << shard.sourcePositions << " for " << shard.path.string() << sync_endl;
        }
        std::fstream update(shard.path, std::ios::binary | std::ios::in | std::ios::out);
        if (!update)
        {
            error = "cannot repair shard header " + shard.path.string();
            return false;
        }
        write_header(update, shard.records, shard.sourcePositions);
        update.close();
        if (!update)
        {
            error = "failed while repairing shard header " + shard.path.string();
            return false;
        }
        if (countMismatch)
            sync_cout << "info string datagen resume: repaired shard " << shard.path.string()
                      << " header count from " << declaredCount << " to " << shard.records
                      << sync_endl;
    }
    return true;
}

bool discover_shards(const std::filesystem::path& out,
                     bool                         sanitize,
                     std::vector<ShardInfo>&      shards,
                     std::string&                 error) {
    shards.clear();
    auto parent = out.parent_path();
    if (parent.empty())
        parent = ".";
    const std::string prefix = out.filename().string() + ".";

    std::error_code ec;
    if (!std::filesystem::exists(parent, ec))
        return !ec;
    std::filesystem::directory_iterator iterator(parent, ec);
    if (ec)
    {
        error = "cannot enumerate output shards: " + ec.message();
        return false;
    }
    for (const auto& entry : iterator)
    {
        if (!entry.is_regular_file(ec))
        {
            if (ec)
            {
                error = "cannot inspect output shard directory: " + ec.message();
                return false;
            }
            continue;
        }
        usize id = 0;
        if (parse_shard_id(entry.path().filename().string(), prefix, id))
            shards.push_back({id, entry.path(), 0, 0});
    }
    std::sort(shards.begin(), shards.end(),
              [](const auto& left, const auto& right) { return left.id < right.id; });
    for (usize i = 1; i < shards.size(); ++i)
        if (shards[i - 1].id == shards[i].id)
        {
            error = "duplicate numeric shard id " + std::to_string(shards[i].id);
            return false;
        }
    for (auto& shard : shards)
        if (!inspect_shard(shard, sanitize, error))
            return false;
    return true;
}

bool valid_debug_record_line(const std::string& line) {
    const auto first = line.find(" | ");
    if (first == std::string::npos || first == 0)
        return false;
    const auto second = line.find(" | ", first + 3);
    if (second == std::string::npos)
        return false;

    int                score  = 0;
    int                result = 0;
    std::string        extra;
    std::istringstream scoreText(line.substr(first + 3, second - first - 3));
    std::istringstream resultText(line.substr(second + 3));
    return bool(scoreText >> score) && !(scoreText >> extra) && bool(resultText >> result)
        && !(resultText >> extra) && result >= -1 && result <= 1;
}

usize count_debug_records(const std::vector<ShardInfo>& shards) {
    usize total = 0;
    for (const auto& shard : shards)
    {
        std::ifstream debug(with_suffix(shard.path, ".debug"));
        std::string   line;
        u64           inShard = 0;
        while (inShard < shard.records && std::getline(debug, line))
            if (!line.empty() && line[0] != '#' && valid_debug_record_line(line))
            {
                ++inShard;
                ++total;
            }
    }
    return total;
}

bool verify_merged_output(const std::filesystem::path& path,
                          u64                          count,
                          u64                          sourcePositions,
                          std::string&                 error) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        error = "cannot verify merged output " + path.string();
        return false;
    }
    u64 headerCount  = 0;
    u64 headerSource = 0;
    u64 flags        = 0;
    if (!read_header(input, headerCount, headerSource, flags, error))
    {
        error = "cannot verify merged output: " + error;
        return false;
    }
    std::error_code ec;
    const u64       size     = std::filesystem::file_size(path, ec);
    const u64       expected = Run7HeaderSize + count * Run7RecordSize;
    if (ec || headerCount != count || headerSource != sourcePositions || flags || size != expected)
    {
        error = "merged output verification failed (header/size mismatch)";
        return false;
    }
    return true;
}

bool write_metadata(const Params&                   params,
                    const std::vector<WorkerStats>& stats,
                    u64                             sourcePositions,
                    u64                             games,
                    u64                             survivorRecords,
                    double                          seconds,
                    std::string&                    error) {
    const auto    path = with_suffix(params.out, ".meta.json");
    std::ofstream file(path, std::ios::trunc);
    if (!file)
    {
        error = "cannot write metadata " + path.string();
        return false;
    }

    u64 sessionRecords = 0;
    for (const auto& worker : stats)
        sessionRecords += worker.records;
    const bool   exactGameStats     = survivorRecords == 0;
    const double positionsPerSecond = double(sessionRecords) / std::max(seconds, 1e-9);
    const double perThread          = positionsPerSecond / double(params.threads);
    const double projectionHours    = perThread > 0 ? 50000000.0 / (perThread * 24.0) / 3600.0 : 0;

    std::map<u64, u64> recordsPerGame;
    for (const auto& worker : stats)
        for (const auto& [records, gameCount] : worker.recordsPerGame)
            recordsPerGame[records] += gameCount;

    const u64 zeroRecordGames = recordsPerGame.count(0) ? recordsPerGame.at(0) : 0;
    const u64 nonemptyGames   = games - zeroRecordGames;
    u64       whiteWins       = 0;
    u64       blackWins       = 0;
    u64       draws           = 0;
    for (const auto& worker : stats)
    {
        whiteWins += worker.whiteWins;
        blackWins += worker.blackWins;
        draws += worker.draws;
    }

    file << std::fixed << std::setprecision(6) << "{\n"
         << "  \"format\": \"run7\",\n"
         << "  \"version\": 1,\n"
         << "  \"records\": " << params.count << ",\n"
         << "  \"source_positions\": " << sourcePositions << ",\n"
         << "  \"resume_count\": " << params.resumeNumber << ",\n"
         << "  \"survivor_records\": " << survivorRecords << ",\n"
         << "  \"session_records\": " << sessionRecords << ",\n";
    if (exactGameStats)
        file << "  \"games\": " << games << ",\n"
             << "  \"game_results\": {\"white_win\": " << whiteWins
             << ", \"black_win\": " << blackWins << ", \"draw\": " << draws << "},\n"
             << "  \"games_with_records\": " << nonemptyGames << ",\n"
             << "  \"zero_record_games\": " << zeroRecordGames << ",\n"
             << "  \"records_per_game_mean\": "
             << (games ? double(params.count) / double(games) : 0.0) << ",\n"
             << "  \"records_per_nonempty_game_mean\": "
             << (nonemptyGames ? double(params.count) / double(nonemptyGames) : 0.0) << ",\n";
    file
      << "  \"seconds\": " << seconds << ",\n"
      << "  \"threads\": " << params.threads << ",\n"
      << "  \"nodes\": " << params.nodes << ",\n"
      << "  \"seed\": " << params.seed << ",\n"
      << "  \"positions_per_second\": " << positionsPerSecond << ",\n"
      << "  \"positions_per_second_per_thread\": " << perThread << ",\n"
      << "  \"projected_hours_50m_24_threads\": " << projectionHours << ",\n"
      << "  \"debug_sample\": " << params.debugSample << ",\n"
      << "  \"shard_policy\": \"temporary numbered run7 shards; append sessions use new ids; remove after verified merge\",\n";
    if (exactGameStats)
    {
        file << "  \"records_per_game_histogram\": {";
        usize histogramIndex = 0;
        for (const auto& [records, gameCount] : recordsPerGame)
            file << (histogramIndex++ ? ", " : "") << '\"' << records << "\": " << gameCount;
        file << "},\n";
    }
    file << "  \"workers\": [\n";
    for (usize i = 0; i < stats.size(); ++i)
    {
        const auto& worker = stats[i];
        file << "    {\"id\": " << worker.shardId << ", \"seed\": " << worker.seed
             << ", \"records\": " << worker.records
             << ", \"source_positions\": " << worker.sourcePositions
             << ", \"games\": " << worker.games << ", \"seconds\": " << worker.seconds << "}"
             << (i + 1 == stats.size() ? "\n" : ",\n");
    }
    file << "  ]\n}\n";
    if (!file)
    {
        error = "failed while writing metadata " + path.string();
        return false;
    }
    return true;
}

bool merge_shards(const Params&                 params,
                  const std::vector<ShardInfo>& shards,
                  u64                           sourcePositions,
                  std::string&                  error) {
    u64 records = 0;
    for (const auto& shard : shards)
        records += shard.records;
    if (records != params.count)
    {
        error = "cannot merge " + std::to_string(records) + " records; target is "
              + std::to_string(params.count);
        return false;
    }

    const auto    temporary = with_suffix(params.out, ".tmp");
    std::ofstream merged(temporary, std::ios::binary | std::ios::trunc);
    if (!merged)
    {
        error = "cannot create merged output " + temporary.string();
        return false;
    }
    write_header(merged, params.count, sourcePositions);

    std::array<char, 1024 * 1024> buffer{};
    for (const auto& info : shards)
    {
        std::ifstream shard(info.path, std::ios::binary);
        if (!shard)
        {
            error = "cannot read shard " + info.path.string();
            return false;
        }
        shard.seekg(std::streamoff(Run7HeaderSize));
        u64 remaining = info.records * Run7RecordSize;
        while (remaining)
        {
            const usize chunk = usize(std::min<u64>(remaining, buffer.size()));
            shard.read(buffer.data(), std::streamsize(chunk));
            if (shard.gcount() != std::streamsize(chunk))
            {
                error = "truncated shard " + info.path.string();
                return false;
            }
            merged.write(buffer.data(), std::streamsize(chunk));
            remaining -= chunk;
        }
    }
    merged.close();
    if (!merged)
    {
        error = "failed while writing merged output";
        return false;
    }
    if (!verify_merged_output(temporary, params.count, sourcePositions, error))
        return false;

    std::optional<std::filesystem::path> debugTemporary;
    std::optional<std::filesystem::path> debugFinal;
    if (params.debugSample)
    {
        debugTemporary = with_suffix(params.out, ".debug.txt.tmp");
        debugFinal     = with_suffix(params.out, ".debug.txt");
        std::ofstream debug(*debugTemporary, std::ios::trunc);
        if (!debug)
        {
            error = "cannot create debug sidecar";
            return false;
        }

        usize             debugRecords = 0;
        std::string       lastMarker;
        const std::string currentMarker =
          "# datagen resume session " + std::to_string(params.resumeNumber);
        bool sawCurrentMarker = false;
        for (const auto& info : shards)
        {
            const auto    debugShard = with_suffix(info.path, ".debug");
            std::ifstream input(debugShard);
            std::string   line;
            u64           shardRecords = 0;
            while (std::getline(input, line))
            {
                if (!line.empty() && line[0] == '#')
                {
                    if (line == currentMarker)
                        sawCurrentMarker = true;
                    if (line != lastMarker)
                    {
                        debug << line << '\n';
                        lastMarker = line;
                    }
                }
                else if (shardRecords < info.records && valid_debug_record_line(line))
                {
                    ++shardRecords;
                    if (debugRecords < params.debugSample)
                    {
                        debug << line << '\n';
                        ++debugRecords;
                    }
                }
            }
        }
        if (params.resume && !sawCurrentMarker)
            debug << currentMarker << '\n';
        debug.close();
        if (debugRecords != params.debugSample || !debug)
        {
            error = "debug shards did not contain the requested sample";
            return false;
        }
    }

    std::error_code ec;
    std::filesystem::rename(temporary, params.out, ec);
    if (ec)
    {
        error = "cannot publish merged output: " + ec.message();
        return false;
    }
    if (debugTemporary)
    {
        std::filesystem::rename(*debugTemporary, *debugFinal, ec);
        if (ec)
        {
            std::error_code rollbackError;
            std::filesystem::remove(params.out, rollbackError);
            error = "cannot publish debug sidecar: " + ec.message();
            return false;
        }
    }
    if (!verify_merged_output(params.out, params.count, sourcePositions, error))
        return false;

    // Shards are retained on every failure. Only a verified, fully published
    // output and requested debug sidecar make it safe to reclaim duplicates.
    for (const auto& info : shards)
    {
        std::filesystem::remove(info.path, ec);
        if (params.debugSample)
            std::filesystem::remove(with_suffix(info.path, ".debug"), ec);
    }
    return true;
}

void configure_engine(Engine& engine, int multiPv) {
    engine.set_on_iter([](const Engine::InfoIter&) {});
    engine.set_on_update_no_moves([](const Engine::InfoShort&) {});
    engine.set_on_bestmove([](std::string_view, std::string_view) {});
    engine.set_on_verify_network([](std::string_view) {});
    std::istringstream option("name MultiPV value " + std::to_string(multiPv));
    engine.get_options().setoption(option);
}

void generate_worker(const Params&                   params,
                     const std::vector<std::string>& book,
                     Engine&                         engine,
                     WorkerStats&                    stats,
                     std::atomic_bool&               abort,
                     std::atomic<u64>&               globalRecords,
                     std::atomic<TimePoint>&         lastReport,
                     TimePoint                       globalStart,
                     std::mutex&                     reportMutex) {
    const auto    shardPath = with_suffix(params.out, "." + std::to_string(stats.shardId));
    std::ofstream shard(shardPath, std::ios::binary | std::ios::trunc);
    if (!shard)
    {
        stats.error = "cannot create shard " + shardPath.string();
        abort       = true;
        return;
    }
    write_header(shard, stats.target, 0);
    shard.flush();
    if (!shard)
    {
        stats.error = "cannot initialize shard " + shardPath.string();
        abort       = true;
        return;
    }

    std::ofstream debug;
    if (params.debugSample)
    {
        debug.open(with_suffix(shardPath, ".debug"), std::ios::trunc);
        if (!debug)
        {
            stats.error = "cannot create debug shard";
            abort       = true;
            return;
        }
        if (params.resume)
        {
            debug << "# datagen resume session " << params.resumeNumber << '\n';
            debug.flush();
        }
    }

    PRNG          rng(stats.seed);
    SearchCapture capture(usize(params.randomMultiPv));
    engine.set_on_update_full([&capture](const Engine::InfoFull& info) { capture.update(info); });

    const TimePoint workerStart = now();
    usize           debugLines  = 0;

    while (stats.records < stats.target && !abort.load(std::memory_order_relaxed))
    {
        Position     pos;
        StateListPtr states(new std::deque<StateInfo>(1));
        const auto&  opening = book[usize(bounded_rand(rng, book.size()))];
        if (auto setError = pos.set(opening, false, &states->back()))
        {
            stats.error = "invalid book position: " + std::string(setError->what());
            abort       = true;
            break;
        }

        const auto                  randomFlags = random_move_flags(params, rng);
        std::vector<BufferedRecord> game;
        int                         whiteResult = 0;

        for (int ply = 0; !abort.load(std::memory_order_relaxed); ++ply)
        {
            if (!pos.count<KING>(WHITE) || !pos.count<KING>(BLACK))
            {
                whiteResult = pos.count<KING>(WHITE) ? 1 : pos.count<KING>(BLACK) ? -1 : 0;
                break;
            }

            MoveList<LEGAL> legal(pos);
            if (!legal.size())
            {
                whiteResult = pos.checkers() ? (pos.side_to_move() == WHITE ? -1 : 1) : 0;
                break;
            }
            if (pos.is_draw(ply))
            {
                whiteResult = 0;
                break;
            }

            ++stats.sourcePositions;
            const bool uniformRandom = usize(ply) < randomFlags.size() && randomFlags[usize(ply)];
            capture.reset(pos);
            if (auto setError = engine.set_position(pos.fen(), {}))
            {
                stats.error = "search position rejected: " + std::string(setError->what());
                abort       = true;
                break;
            }

            Search::LimitsType limits;
            limits.nodes = params.nodes;
            engine.go(limits);
            engine.wait_for_search_finished();

            if (!capture.lines[0].valid)
            {
                stats.error = "fixed-node search returned no scored PV in a legal position";
                abort       = true;
                break;
            }

            const Move best      = capture.lines[0].move;
            const int  bestScore = capture.lines[0].score;
            Move       played    = Move::none();
            if (uniformRandom)
                played = legal.begin()[bounded_rand(rng, legal.size())];
            else
            {
                usize eligible = 1;
                while (eligible < capture.lines.size() && capture.lines[eligible].valid
                       && capture.lines[0].score - capture.lines[eligible].score
                            <= params.randomMultiPvDiff)
                    ++eligible;
                played = capture.lines[usize(bounded_rand(rng, eligible))].move;
            }

            const bool terminal = captures_king(pos, played);
            bool       write    = terminal;

            if (!terminal)
            {
                write = ply >= params.writeMinPly && std::abs(bestScore) <= params.evalLimit
                     && !(params.filterChecks && pos.checkers())
                     && !(params.filterCaptures && capture_filter_matches(pos, best))
                     && !(params.filterPromotions && best.type_of() == PROMOTION);

                if (write && params.evalDiffLimit < MateTarget)
                {
                    const int qsearchScore = SearchCapture::score_to_cp(engine.qsearch());
                    write = std::abs(bestScore - qsearchScore) <= params.evalDiffLimit;
                }
            }

            if (write)
            {
                const int  score      = terminal ? MateTarget : bestScore;
                const Move recordMove = terminal ? played : best;
                game.push_back({pack_record(pos, score, recordMove),
                                params.debugSample ? pos.fen() : "", score});
            }

            if (!played || !pos.legal(played))
            {
                stats.error = "selected move is not legal";
                abort       = true;
                break;
            }

            states->emplace_back();
            pos.do_move(played, states->back());
        }

        if (abort.load(std::memory_order_relaxed))
            break;

        u64 writtenThisGame = 0;
        for (auto& item : game)
        {
            if (stats.records >= stats.target)
                break;
            set_result(item.record, whiteResult);
            shard.write(reinterpret_cast<const char*>(item.record.data()),
                        std::streamsize(item.record.size()));
            if (params.debugSample && debugLines < stats.debugTarget)
            {
                const bool stmBlack = item.record[24] & 1;
                const int stmResult = whiteResult == 0 ? 0 : (whiteResult > 0) != stmBlack ? 1 : -1;
                debug << item.fen << " | " << item.score << " | " << stmResult << '\n';
                debug.flush();
                ++debugLines;
            }
            ++stats.records;
            ++writtenThisGame;
        }
        shard.flush();
        if (!shard)
        {
            stats.error = "failed while flushing shard " + shardPath.string();
            abort       = true;
            break;
        }
        ++stats.games;
        if (whiteResult > 0)
            ++stats.whiteWins;
        else if (whiteResult < 0)
            ++stats.blackWins;
        else
            ++stats.draws;
        ++stats.recordsPerGame[writtenThisGame];

        const u64 done =
          globalRecords.fetch_add(writtenThisGame, std::memory_order_relaxed) + writtenThisGame;
        const auto current  = now();
        auto       previous = lastReport.load(std::memory_order_relaxed);
        if (current - previous >= 5000
            && lastReport.compare_exchange_strong(previous, current, std::memory_order_relaxed))
        {
            std::lock_guard<std::mutex> lock(reportMutex);
            const double                elapsed = double(current - globalStart) / 1000.0;
            sync_cout << "info string datagen " << std::min(done, params.count) << '/'
                      << params.count << " positions, " << u64(done / std::max(elapsed, 1e-9))
                      << " pos/s" << sync_endl;
        }
    }

    stats.seconds = double(now() - workerStart) / 1000.0;
    shard.seekp(0);
    write_header(shard, stats.records, stats.sourcePositions);
    shard.close();
    debug.close();
    if (!stats.error.empty())
        return;
    if (!shard || (params.debugSample && !debug))
    {
        stats.error = "failed while writing shard " + shardPath.string();
        abort       = true;
    }
}

}  // namespace

bool run(std::istream&                               args,
         const std::optional<std::filesystem::path>& binaryPath,
         std::string&                                error) {
    Params params;
    if (!parse_params(args, params, error))
        return false;

    std::error_code ec;
    if (!std::filesystem::is_regular_file(params.book, ec))
    {
        error = "book does not exist: " + params.book.string();
        return false;
    }
    if (std::filesystem::exists(params.out, ec)
        || std::filesystem::exists(with_suffix(params.out, ".meta.json"), ec)
        || std::filesystem::exists(with_suffix(params.out, ".debug.txt"), ec))
    {
        error =
          "output or one of its final sidecars already exists; generation is already complete";
        return false;
    }

    const auto parent = params.out.parent_path();
    if (!parent.empty())
    {
        std::filesystem::create_directories(parent, ec);
        if (ec)
        {
            error = "cannot create output directory: " + ec.message();
            return false;
        }
    }

    const auto  normalizedBook = normalized_path(params.book);
    const auto  normalizedOut  = normalized_path(params.out);
    u64         bookSize       = 0;
    std::string bookHash;
    if (!fast_file_hash(normalizedBook, bookSize, bookHash, error))
        return false;

    std::vector<std::string> book;
    if (!load_book(normalizedBook, book, error))
        return false;

    ResumeMetadata         resumeMetadata;
    std::vector<ShardInfo> existingShards;
    u64                    survivorRecords = 0;
    if (params.resume)
    {
        if (!load_resume_metadata(params.out, resumeMetadata, error)
            || !validate_resume_metadata(params, resumeMetadata, normalizedBook, bookSize, bookHash,
                                         error))
            return false;
        if (!discover_shards(params.out, true, existingShards, error))
            return false;
        for (const auto& shard : existingShards)
        {
            if (survivorRecords > params.count || shard.records > params.count - survivorRecords)
            {
                error = "surviving shard record count exceeds target count";
                return false;
            }
            survivorRecords += shard.records;
        }
        if (survivorRecords > params.count)
        {
            error = "surviving shards contain " + std::to_string(survivorRecords)
                  + " records, exceeding target " + std::to_string(params.count);
            return false;
        }

        params.resumeNumber        = resumeMetadata.resumeCount + 1;
        resumeMetadata.resumeCount = params.resumeNumber;
        resumeMetadata.lastCommand = normalized_command(params, normalizedBook, normalizedOut);
        if (!write_resume_metadata(params.out, resumeMetadata, error))
            return false;

        for (const char* suffix : {".tmp", ".debug.txt.tmp", ".meta.json.tmp"})
        {
            const auto stale = with_suffix(params.out, suffix);
            if (std::filesystem::exists(stale, ec))
            {
                std::filesystem::remove(stale, ec);
                if (ec)
                {
                    error = "cannot remove stale resume temporary " + stale.string() + ": "
                          + ec.message();
                    return false;
                }
                sync_cout << "info string datagen resume: removed stale temporary "
                          << stale.string() << sync_endl;
            }
        }
    }
    else
    {
        if (std::filesystem::exists(with_suffix(params.out, ".meta"), ec)
            || std::filesystem::exists(with_suffix(params.out, ".meta.tmp"), ec)
            || std::filesystem::exists(with_suffix(params.out, ".meta.prev"), ec)
            || std::filesystem::exists(with_suffix(params.out, ".tmp"), ec)
            || std::filesystem::exists(with_suffix(params.out, ".debug.txt.tmp"), ec)
            || std::filesystem::exists(with_suffix(params.out, ".meta.json.tmp"), ec))
        {
            error = "output resume metadata or temporary sidecar already exists; use --resume";
            return false;
        }
        if (!discover_shards(params.out, false, existingShards, error))
            return false;
        if (!existingShards.empty())
        {
            error = "an output shard from an earlier run already exists; use --resume";
            return false;
        }
        resumeMetadata =
          make_resume_metadata(params, normalizedBook, normalizedOut, bookSize, bookHash);
        if (!write_resume_metadata(params.out, resumeMetadata, error))
            return false;
    }

    const u64 remaining    = params.count - survivorRecords;
    usize     firstShardId = 0;
    if (!existingShards.empty())
    {
        if (existingShards.back().id == std::numeric_limits<usize>::max())
        {
            error = "numeric shard id space exhausted";
            return false;
        }
        firstShardId = existingShards.back().id + 1;
    }
    if (params.threads - 1 > std::numeric_limits<usize>::max() - firstShardId)
    {
        error = "numeric shard id space exhausted";
        return false;
    }

    std::vector<WorkerStats>             stats(params.threads);
    std::vector<std::unique_ptr<Engine>> engines(params.threads);
    usize                                debugRemaining = params.debugSample;
    if (params.resume)
        debugRemaining -= std::min(debugRemaining, count_debug_records(existingShards));
    for (usize id = 0; id < params.threads; ++id)
    {
        stats[id].shardId     = firstShardId + id;
        stats[id].target      = remaining / params.threads + (id < remaining % params.threads);
        stats[id].seed        = splitmix_seed(params.seed, params.resumeNumber, id);
        stats[id].debugTarget = usize(std::min<u64>(stats[id].target, u64(debugRemaining)));
        debugRemaining -= stats[id].debugTarget;
        if (stats[id].target)
        {
            const auto shardPath = with_suffix(params.out, "." + std::to_string(stats[id].shardId));
            if (std::filesystem::exists(shardPath, ec)
                || (params.debugSample
                    && std::filesystem::exists(with_suffix(shardPath, ".debug"), ec)))
            {
                error = "new shard path already exists: " + shardPath.string();
                return false;
            }
            engines[id] = std::make_unique<Engine>(binaryPath);
            configure_engine(*engines[id], params.randomMultiPv);
        }
    }
    if (debugRemaining)
    {
        error = "resume cannot reconstruct " + std::to_string(debugRemaining)
              + " missing debug sample record(s) from the surviving binary shards";
        return false;
    }

    sync_cout << "info string datagen run7" << (params.resume ? " resume" : "") << ": "
              << params.count << " target positions, " << survivorRecords << " surviving, "
              << remaining << " remaining, " << params.threads << " independent threads, "
              << params.nodes << " nodes, " << book.size() << " book lines, base seed "
              << params.seed << ", session " << params.resumeNumber << sync_endl;
    sync_cout << "info string datagen shards are temporary; final merge order is numeric shard id"
              << sync_endl;

    std::atomic_bool         abort{false};
    std::atomic<u64>         globalRecords{survivorRecords};
    const TimePoint          start = now();
    std::atomic<TimePoint>   lastReport{start};
    std::mutex               reportMutex;
    std::vector<std::thread> workers;
    workers.reserve(params.threads);
    for (usize id = 0; id < params.threads; ++id)
    {
        if (!stats[id].target)
            continue;
        workers.emplace_back(generate_worker, std::cref(params), std::cref(book),
                             std::ref(*engines[id]), std::ref(stats[id]), std::ref(abort),
                             std::ref(globalRecords), std::ref(lastReport), start,
                             std::ref(reportMutex));
    }
    for (auto& worker : workers)
        worker.join();

    for (const auto& worker : stats)
        if (!worker.error.empty())
        {
            error = worker.error;
            return false;
        }
    for (const auto& worker : stats)
        if (worker.records != worker.target)
        {
            error = "generation stopped before every shard reached its target";
            return false;
        }

    u64 games = 0;
    for (const auto& worker : stats)
        games += worker.games;

    std::vector<ShardInfo> allShards;
    if (!discover_shards(params.out, false, allShards, error))
        return false;
    u64 totalRecords    = 0;
    u64 sourcePositions = 0;
    for (const auto& shard : allShards)
    {
        totalRecords += shard.records;
        sourcePositions += shard.sourcePositions;
    }
    if (totalRecords != params.count)
    {
        error = "generation produced " + std::to_string(totalRecords) + " records; target is "
              + std::to_string(params.count);
        return false;
    }

    if (!merge_shards(params, allShards, sourcePositions, error))
        return false;

    const double seconds = double(now() - start) / 1000.0;
    if (!write_metadata(params, stats, sourcePositions, games, survivorRecords, seconds, error))
        return false;

    const double positionsPerSecond = double(remaining) / std::max(seconds, 1e-9);
    const double perThread          = positionsPerSecond / double(params.threads);
    const double projection         = 50000000.0 / std::max(perThread * 24.0, 1e-9) / 3600.0;
    sync_cout << "info string datagen finished: " << params.count << " total positions, "
              << remaining << " generated this session, " << games << " session games, "
              << std::fixed << std::setprecision(2) << positionsPerSecond << " pos/s, " << perThread
              << " pos/s/thread, 50M@24 projection " << projection << " h -> "
              << params.out.string() << sync_endl;
    return true;
}

}  // namespace Stockfish::Datagen
