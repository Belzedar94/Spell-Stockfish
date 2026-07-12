/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
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

#ifndef THREAD_WIN32_OSX_H_INCLUDED
#define THREAD_WIN32_OSX_H_INCLUDED

#include <thread>

// Spell chess: every search ply holds a MovePicker frame with a MAX_MOVES
// (32k) move buffer, so a deep search needs MAX_PLY * ~264KB ≈ 64MB of
// stack — far beyond the 512KB (macOS) / 1MB (Windows) / 8MB (Linux)
// defaults. Search threads are therefore created with an explicit 128MB
// stack on every platform that supports pthreads (the memory is reserved
// lazily, only pages actually touched are committed).

#if defined(__APPLE__) || defined(__MINGW32__) || defined(__MINGW64__) || defined(__linux__) \
  || defined(USE_PTHREADS)

    #include <pthread.h>
    #include <functional>

namespace Stockfish {

class NativeThread {
    pthread_t thread;

    static constexpr usize TH_STACK_SIZE = 128 * 1024 * 1024;

   public:
    template<class Function, class... Args>
    explicit NativeThread(Function&& fun, Args&&... args) {
        auto func = new std::function<void()>(
          std::bind(std::forward<Function>(fun), std::forward<Args>(args)...));

        pthread_attr_t attr_storage, *attr = &attr_storage;
        pthread_attr_init(attr);
        pthread_attr_setstacksize(attr, TH_STACK_SIZE);

        auto start_routine = [](void* ptr) -> void* {
            auto f = reinterpret_cast<std::function<void()>*>(ptr);
            // Call the function
            (*f)();
            delete f;
            return nullptr;
        };

        pthread_create(&thread, attr, start_routine, func);
    }

    void join() { pthread_join(thread, nullptr); }
};

}  // namespace Stockfish

#else  // Default case: use STL classes

namespace Stockfish {

using NativeThread = std::thread;

}  // namespace Stockfish

#endif

#endif  // #ifndef THREAD_WIN32_OSX_H_INCLUDED
