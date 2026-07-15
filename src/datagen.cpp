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

constexpr usize Run7HeaderSize = 32;
constexpr usize Run7RecordSize = 44;
constexpr int   MateTarget     = 32000;

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
    u64                target          = 0;
    u64                records         = 0;
    u64                sourcePositions = 0;
    u64                games           = 0;
    u64                whiteWins       = 0;
    u64                blackWins       = 0;
    u64                draws           = 0;
    u64                seed            = 0;
    double             seconds         = 0.0;
    std::map<u64, u64> recordsPerGame;
    std::string        error;
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

u64 splitmix_seed(u64 seed, usize threadId) {
    u64 z = seed + 0x9E3779B97F4A7C15ULL * (u64(threadId) + 1);
    z     = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z     = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
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
    else if (!params.threads)
        error = "threads must be greater than zero";
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

bool write_metadata(const Params&                   params,
                    const std::vector<WorkerStats>& stats,
                    u64                             sourcePositions,
                    u64                             games,
                    double                          seconds,
                    std::string&                    error) {
    const auto    path = with_suffix(params.out, ".meta.json");
    std::ofstream file(path, std::ios::trunc);
    if (!file)
    {
        error = "cannot write metadata " + path.string();
        return false;
    }

    const double positionsPerSecond = double(params.count) / std::max(seconds, 1e-9);
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

    file
      << std::fixed << std::setprecision(6) << "{\n"
      << "  \"format\": \"run7\",\n"
      << "  \"version\": 1,\n"
      << "  \"records\": " << params.count << ",\n"
      << "  \"source_positions\": " << sourcePositions << ",\n"
      << "  \"games\": " << games << ",\n"
      << "  \"game_results\": {\"white_win\": " << whiteWins << ", \"black_win\": " << blackWins
      << ", \"draw\": " << draws << "},\n"
      << "  \"games_with_records\": " << nonemptyGames << ",\n"
      << "  \"zero_record_games\": " << zeroRecordGames << ",\n"
      << "  \"records_per_game_mean\": " << (games ? double(params.count) / double(games) : 0.0)
      << ",\n"
      << "  \"records_per_nonempty_game_mean\": "
      << (nonemptyGames ? double(params.count) / double(nonemptyGames) : 0.0) << ",\n"
      << "  \"seconds\": " << seconds << ",\n"
      << "  \"threads\": " << params.threads << ",\n"
      << "  \"nodes\": " << params.nodes << ",\n"
      << "  \"seed\": " << params.seed << ",\n"
      << "  \"positions_per_second\": " << positionsPerSecond << ",\n"
      << "  \"positions_per_second_per_thread\": " << perThread << ",\n"
      << "  \"projected_hours_50m_24_threads\": " << projectionHours << ",\n"
      << "  \"debug_sample\": " << params.debugSample << ",\n"
      << "  \"shard_policy\": \"temporary per-thread run7 shards; merge by thread id; remove after verified merge\",\n"
      << "  \"records_per_game_histogram\": {";
    usize histogramIndex = 0;
    for (const auto& [records, gameCount] : recordsPerGame)
        file << (histogramIndex++ ? ", " : "") << '\"' << records << "\": " << gameCount;
    file << "},\n"
         << "  \"workers\": [\n";
    for (usize i = 0; i < stats.size(); ++i)
    {
        const auto& worker = stats[i];
        file << "    {\"id\": " << i << ", \"seed\": " << worker.seed
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

bool merge_shards(const Params&                   params,
                  const std::vector<WorkerStats>& stats,
                  u64                             sourcePositions,
                  std::string&                    error) {
    const auto    temporary = with_suffix(params.out, ".tmp");
    std::ofstream merged(temporary, std::ios::binary | std::ios::trunc);
    if (!merged)
    {
        error = "cannot create merged output " + temporary.string();
        return false;
    }
    write_header(merged, params.count, sourcePositions);

    std::array<char, 1024 * 1024> buffer{};
    for (usize id = 0; id < stats.size(); ++id)
    {
        if (!stats[id].target)
            continue;
        const auto    shardPath = with_suffix(params.out, "." + std::to_string(id));
        std::ifstream shard(shardPath, std::ios::binary);
        if (!shard)
        {
            error = "cannot read shard " + shardPath.string();
            return false;
        }
        shard.seekg(std::streamoff(Run7HeaderSize));
        u64 remaining = stats[id].records * Run7RecordSize;
        while (remaining)
        {
            const usize chunk = usize(std::min<u64>(remaining, buffer.size()));
            shard.read(buffer.data(), std::streamsize(chunk));
            if (shard.gcount() != std::streamsize(chunk))
            {
                error = "truncated shard " + shardPath.string();
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

    std::error_code ec;
    std::filesystem::rename(temporary, params.out, ec);
    if (ec)
    {
        error = "cannot publish merged output: " + ec.message();
        return false;
    }

    if (params.debugSample)
    {
        const auto    debugTemporary = with_suffix(params.out, ".debug.txt.tmp");
        const auto    debugFinal     = with_suffix(params.out, ".debug.txt");
        std::ofstream debug(debugTemporary, std::ios::trunc);
        if (!debug)
        {
            error = "cannot create debug sidecar";
            return false;
        }

        usize remaining = params.debugSample;
        for (usize id = 0; id < stats.size() && remaining; ++id)
        {
            if (!stats[id].target)
                continue;
            const auto    debugShard = with_suffix(params.out, "." + std::to_string(id) + ".debug");
            std::ifstream input(debugShard);
            std::string   line;
            while (remaining && std::getline(input, line))
            {
                debug << line << '\n';
                --remaining;
            }
        }
        debug.close();
        if (remaining || !debug)
        {
            error = "debug shards did not contain the requested sample";
            return false;
        }

        std::filesystem::rename(debugTemporary, debugFinal, ec);
        if (ec)
        {
            error = "cannot publish debug sidecar: " + ec.message();
            return false;
        }
    }

    // Shards are retained on every failure. Only a fully published output and
    // sidecar make it safe to reclaim their duplicate disk space.
    for (usize id = 0; id < stats.size(); ++id)
    {
        if (!stats[id].target)
            continue;
        std::filesystem::remove(with_suffix(params.out, "." + std::to_string(id)), ec);
        if (params.debugSample)
            std::filesystem::remove(with_suffix(params.out, "." + std::to_string(id) + ".debug"),
                                    ec);
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

void generate_worker(usize                           id,
                     const Params&                   params,
                     const std::vector<std::string>& book,
                     Engine&                         engine,
                     WorkerStats&                    stats,
                     std::atomic_bool&               abort,
                     std::atomic<u64>&               globalRecords,
                     std::atomic<TimePoint>&         lastReport,
                     TimePoint                       globalStart,
                     std::mutex&                     reportMutex) {
    const auto    shardPath = with_suffix(params.out, "." + std::to_string(id));
    std::ofstream shard(shardPath, std::ios::binary | std::ios::trunc);
    if (!shard)
    {
        stats.error = "cannot create shard " + shardPath.string();
        abort       = true;
        return;
    }
    write_header(shard, stats.target, 0);

    std::ofstream debug;
    if (params.debugSample)
    {
        debug.open(with_suffix(params.out, "." + std::to_string(id) + ".debug"), std::ios::trunc);
        if (!debug)
        {
            stats.error = "cannot create debug shard";
            abort       = true;
            return;
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
            if (params.debugSample && debugLines < params.debugSample)
            {
                const bool stmBlack = item.record[24] & 1;
                const int stmResult = whiteResult == 0 ? 0 : (whiteResult > 0) != stmBlack ? 1 : -1;
                debug << item.fen << " | " << item.score << " | " << stmResult << '\n';
                ++debugLines;
            }
            ++stats.records;
            ++writtenThisGame;
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
        || std::filesystem::exists(with_suffix(params.out, ".tmp"), ec)
        || std::filesystem::exists(with_suffix(params.out, ".meta.json"), ec)
        || std::filesystem::exists(with_suffix(params.out, ".meta.json.tmp"), ec)
        || (params.debugSample
            && (std::filesystem::exists(with_suffix(params.out, ".debug.txt"), ec)
                || std::filesystem::exists(with_suffix(params.out, ".debug.txt.tmp"), ec))))
    {
        error = "output or one of its final sidecars already exists";
        return false;
    }
    for (usize id = 0; id < params.threads; ++id)
        if (std::filesystem::exists(with_suffix(params.out, "." + std::to_string(id)), ec)
            || (params.debugSample
                && std::filesystem::exists(
                  with_suffix(params.out, "." + std::to_string(id) + ".debug"), ec)))
        {
            error = "an output shard from an earlier run already exists";
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

    std::vector<std::string> book;
    if (!load_book(params.book, book, error))
        return false;

    std::vector<WorkerStats>             stats(params.threads);
    std::vector<std::unique_ptr<Engine>> engines(params.threads);
    for (usize id = 0; id < params.threads; ++id)
    {
        stats[id].target = params.count / params.threads + (id < params.count % params.threads);
        stats[id].seed   = splitmix_seed(params.seed, id);
        if (stats[id].target)
        {
            engines[id] = std::make_unique<Engine>(binaryPath);
            configure_engine(*engines[id], params.randomMultiPv);
        }
    }

    sync_cout << "info string datagen run7: " << params.count << " positions, " << params.threads
              << " independent threads, " << params.nodes << " nodes, " << book.size()
              << " book lines, seed " << params.seed << sync_endl;
    sync_cout << "info string datagen shards are temporary; final merge order is thread id"
              << sync_endl;

    std::atomic_bool         abort{false};
    std::atomic<u64>         globalRecords{0};
    const TimePoint          start = now();
    std::atomic<TimePoint>   lastReport{start};
    std::mutex               reportMutex;
    std::vector<std::thread> workers;
    workers.reserve(params.threads);
    for (usize id = 0; id < params.threads; ++id)
    {
        if (!stats[id].target)
            continue;
        workers.emplace_back(generate_worker, id, std::cref(params), std::cref(book),
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

    u64 sourcePositions = 0;
    u64 games           = 0;
    for (const auto& worker : stats)
    {
        sourcePositions += worker.sourcePositions;
        games += worker.games;
    }

    if (!merge_shards(params, stats, sourcePositions, error))
        return false;

    const double seconds = double(now() - start) / 1000.0;
    if (!write_metadata(params, stats, sourcePositions, games, seconds, error))
        return false;

    const double positionsPerSecond = double(params.count) / std::max(seconds, 1e-9);
    const double perThread          = positionsPerSecond / double(params.threads);
    const double projection         = 50000000.0 / std::max(perThread * 24.0, 1e-9) / 3600.0;
    sync_cout << "info string datagen finished: " << params.count << " positions, " << games
              << " games, " << std::fixed << std::setprecision(2) << positionsPerSecond
              << " pos/s, " << perThread << " pos/s/thread, 50M@24 projection " << projection
              << " h -> " << params.out.string() << sync_endl;
    return true;
}

}  // namespace Stockfish::Datagen
