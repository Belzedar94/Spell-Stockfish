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

#ifndef DATAGEN_H_INCLUDED
#define DATAGEN_H_INCLUDED

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>

namespace Stockfish::Datagen {

// Parse and execute the P2-a run7 self-play command. The binary path is used
// only to let each independent search engine resolve its embedded/default net.
// On failure error contains a diagnostic; incomplete shards and the durable
// <out>.meta manifest are kept so --resume can sanitize and continue them.
bool run(std::istream&                               args,
         const std::optional<std::filesystem::path>& binaryPath,
         std::string&                                error);

}  // namespace Stockfish::Datagen

#endif  // #ifndef DATAGEN_H_INCLUDED
