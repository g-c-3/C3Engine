cmake_minimum_required(VERSION 3.20)
# 3.20+ required for:
#   - set_source_files_properties(... LANGUAGE C) on mixed CXX/C targets
#   - COMPILE_FLAGS per-source override working correctly alongside target C++ standard
project(C3Engine VERSION 1.0.0 LANGUAGES CXX C)

# ── Language standard ────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Source files ─────────────────────────────────────────────────────────────────
# Listed in dependency order (headers-first layers).
# Each .cpp has a matching .h except main.cpp (entry point only).
set(SOURCES
    src/bitboard.cpp     # uint64_t ops, attack tables, ray masks
    src/zobrist.cpp      # splitmix32 PRNG, Zobrist key arrays
    src/board.cpp        # Position state, make/unmake, isAttackedBy
    src/movegen.cpp      # variant-aware legal move generation
    src/tt.cpp           # dual-bucket transposition table + pawn hash
    src/history.cpp      # killer, butterfly, continuation, capture, correction history
    src/book.cpp         # opening book data + weighted-random lookup
    src/eval.cpp         # PSTs, all evaluation components, evaluate()
    src/search.cpp       # qsearch, alphaBeta, iterativeDeepening, g_stopFlag
    src/uci.cpp          # UCI parser, option state, Lazy SMP orchestrator
    src/main.cpp         # entry point — stdin/stdout loop + WASM exports
    src/syzygy.cpp       # Syzygy tablebase probing (native only; WASM stubs in syzygy.h)
    # searchthread.h is header-only; included by search.cpp and uci.cpp
    # syzygy.h is header-only for the WASM build (all inline stubs)
)

add_executable(c3engine ${SOURCES})
target_include_directories(c3engine PRIVATE src)

# ── Warnings (both builds) ───────────────────────────────────────────────────────
target_compile_options(c3engine PRIVATE
    -Wall
    -Wextra
    -Wshadow
    -Wno-unused-parameter   # generated tables trigger this harmlessly
)

# ════════════════════════════════════════════════════════════════════════════════
# WASM build  (cmake -DCMAKE_TOOLCHAIN_FILE=.../Emscripten.cmake)
# ════════════════════════════════════════════════════════════════════════════════
if(EMSCRIPTEN)

    set_target_properties(c3engine PROPERTIES
        OUTPUT_NAME "c3engine"
        SUFFIX ".js"              # Emscripten emits c3engine.js + c3engine.wasm
    )

    target_compile_options(c3engine PRIVATE
        -O3
        -DNDEBUG
        -fno-rtti               # no RTTI needed; saves ~10 KB
        -fno-exceptions         # no exceptions; engine uses error codes

        # Lazy SMP (Upgrade 6) uses std::thread, which requires Emscripten's
        # pthread support (-s USE_PTHREADS=1 + SharedArrayBuffer headers).
        # That is not enabled here — the WASM build is intentionally
        # single-threaded (Web Worker provides sufficient concurrency for the
        # browser use-case and avoids the COOP/COEP header requirements).
        # C3_NO_SMP tells uci.cpp to clamp Threads to 1 and omit the
        # "option name Threads" line from the uci response, so GUIs never
        # try to set a thread count that would fail at runtime.
        -DC3_NO_SMP

        # Syzygy tablebase probing is native-only (browser has no filesystem
        # access).  syzygy.h provides inline no-op stubs for the WASM build
        # so all callers compile cleanly without any #ifdef at the call site.
        # tbprobe.c is NOT added to SOURCES for the WASM target — the
        # __EMSCRIPTEN__ guard in syzygy.cpp excludes the entire implementation.

        # book.cpp uses `static thread_local std::mt19937 rng` which is valid
        # in single-threaded WASM (thread_local degrades to static) but
        # triggers an Emscripten warning about non-trivial thread_local
        # constructors without USE_PTHREADS.  Suppress it — the behaviour is
        # correct and intentional.
        -Wno-pthreads-mem-growth
    )

    # ── Emscripten linker flags ──────────────────────────────────────────────────
    # SHELL: prevents CMake from de-duplicating -s flags that look identical.
    target_link_options(c3engine PRIVATE
        -O3

        # Memory — start lean; ALLOW_MEMORY_GROWTH handles setoption Hash expansion.
        # C3Engine has no NNUE network, so initial footprint is small:
        #   16 MB TT (setoption Hash default) + ~8 MB code/stack/data = ~24 MB needed.
        # 32 MB gives comfortable headroom without pre-committing unnecessary RAM.
        # (Stockfish's larger figure includes a 40 MB NNUE network — not applicable here.)
        "SHELL:-s WASM=1"
        "SHELL:-s ALLOW_MEMORY_GROWTH=1"
        "SHELL:-s INITIAL_MEMORY=33554432"     # 32 MB  (16 MB TT + code/stack headroom)
        "SHELL:-s MAXIMUM_MEMORY=536870912"    # 512 MB (setoption Hash 512 ceiling)
        "SHELL:-s STACK_SIZE=1048576"          # 1 MB stack

        # Use emmalloc instead of dlmalloc.
        # emmalloc is Emscripten's lightweight single-threaded allocator — smaller
        # code size (~2 KB vs ~10 KB) and faster for the simple alloc/free pattern
        # in ttResize().  tt.cpp uses plain std::malloc() in the WASM path
        # (the aligned_alloc fix) which emmalloc handles correctly.
        "SHELL:-s MALLOC=emmalloc"

        # Exported C functions (must be prefixed with _)
        "SHELL:-s EXPORTED_FUNCTIONS=_main,_c3_uci_command,_c3_stop"

        # Runtime helpers exposed to JS
        "SHELL:-s EXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,stringToUTF8,lengthBytesUTF8"

        # Web Worker environment — no window, no document
        "SHELL:-s ENVIRONMENT=worker,node"

        # Wrap the output in a factory function so multiple instances are possible
        "SHELL:-s MODULARIZE=1"
        "SHELL:-s EXPORT_NAME=C3Engine"

        # Keep the runtime alive after main() returns (UCI loop is infinite in native;
        # in WASM we return from main() and drive the loop via emscripten_set_main_loop)
        "SHELL:-s NO_EXIT_RUNTIME=1"

        # Separate .wasm file (do NOT use SINGLE_FILE — .wasm can be 1+ MB and
        # inlining it as base64 bloats the .js and prevents streaming compilation)
        "SHELL:-s SINGLE_FILE=0"

        # Assertions off in release; set to 1 locally when debugging memory issues
        "SHELL:-s ASSERTIONS=0"
    )

    # ── Generate glue file banner ────────────────────────────────────────────────
    # Banner is injected into the output JS files as a post-build step in CI
    # (build.yml: "Inject copyright banner") by prepending c3engine_banner.js
    # using cat. The file is written here at configure time so it is available.
    #
    # The banner is a JS block comment — safe to prepend to any Emscripten glue
    # file regardless of MODULARIZE mode.  It does not execute any code.
    file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/src/c3engine_banner.js"
"/*\n\
 * ============================================================================\n\
 * C3Engine — Chess Variant Game Engine\n\
 * ============================================================================\n\
 *\n\
 * Copyright (c) Gokul Chandar, Coimbatore, Tamil Nadu, India.\n\
 * All rights reserved.\n\
 *\n\
 * The variant game logic, opening position generation, rule adaptations,\n\
 * and engine integration are original works authored by the owner.\n\
 * Built on top of Stockfish architecture and other open-source chess engines\n\
 * whose contributions are gratefully acknowledged.\n\
 *\n\
 * ── Game Rules ───────────────────────────────────────────────────────────────\n\
 *\n\
 *  1. Kings start in their standard chess positions (e1 / e8).  All other\n\
 *     15 pieces per side are placed randomly within their respective\n\
 *     allotted ranks at the start of each game.\n\
 *\n\
 *  2. A pawn that begins the game on a back rank (ranks 1–2 for White,\n\
 *     ranks 7–8 for Black) retains its double-step privilege and en passant\n\
 *     rights regardless of its current rank, as long as it has never moved.\n\
 *\n\
 *  3. Castling is available under the same conditions as standard chess:\n\
 *     the king and the relevant rook must both be on their standard starting\n\
 *     squares (e1+h1 / e1+a1 for White; e8+h8 / e8+a8 for Black), neither\n\
 *     must have previously moved, and all standard castling conditions apply.\n\
 *\n\
 *  4. Win, loss, draw, stalemate, and insufficient-material draw are\n\
 *     determined by standard chess rules without modification.\n\
 *\n\
 *  5. All other rules — piece movement, capture, check, checkmate,\n\
 *     promotion, the fifty-move rule, and threefold repetition — follow\n\
 *     standard chess exactly.\n\
 *\n\
 * ── Opening Positions ────────────────────────────────────────────────────────\n\
 *\n\
 *  The variant produces exactly 16,435,321,302,500 (over 16.4 trillion)\n\
 *  distinct legal opening positions, derived from the random placement of\n\
 *  the 15 non-king pieces per side within their allotted ranks.\n\
 *\n\
 * ── License & Distribution ───────────────────────────────────────────────────\n\
 *\n\
 *  C3Engine is free software released under the GNU General Public License\n\
 *  version 3 (GPL v3) or any later version.  You are free to use, study,\n\
 *  modify, and distribute this software and its source code under the terms\n\
 *  of the GPL v3.  A copy of the license is available at:\n\
 *    https://www.gnu.org/licenses/gpl-3.0.html\n\
 *\n\
 *  This engine is offered as a free and open contribution to the global\n\
 *  chess programming community — one of over 16.4 trillion reasons to\n\
 *  explore it.\n\
 *\n\
 * ── Acknowledgements ─────────────────────────────────────────────────────────\n\
 *\n\
 *  This engine builds upon concepts and techniques from:\n\
 *    • Stockfish (https://stockfishchess.org) — search architecture,\n\
 *      pruning techniques, and magic bitboard foundations.\n\
 *    • Fathom (https://github.com/jdart1/Fathom) — Syzygy tablebase\n\
 *      probing library (native build only).\n\
 *    • Tord Romstad — fancy magic bitboard numbers (public domain).\n\
 *    • The open-source chess programming community at\n\
 *      https://www.chessprogramming.org\n\
 *\n\
 *  Special thanks to Claude (Anthropic) for contributing to the C++ engine\n\
 *  implementation — including the JS-to-C++ translation, magic bitboard\n\
 *  integration, Lazy SMP, staged move generation, correction history,\n\
 *  TT improvements, Syzygy tablebase integration, and the suite of\n\
 *  search and evaluation upgrades that shaped this engine.\n\
 *\n\
 * ============================================================================\n\
 */\n"
    )

    # ── Single-file target (standalone JS with inlined WASM) ─────────────────────
    # c3engine_single.js has the WASM binary base64-inlined — no separate .wasm
    # fetch needed. Used to produce the standalone c3engine.min.js output.
    add_executable(c3engine_single ${SOURCES})
    target_include_directories(c3engine_single PRIVATE src)

    target_compile_options(c3engine_single PRIVATE
        -O3 -DNDEBUG -fno-rtti -fno-exceptions
        -DC3_NO_SMP
        -Wno-pthreads-mem-growth
    )

    set_target_properties(c3engine_single PROPERTIES
        OUTPUT_NAME "c3engine_single"
        SUFFIX ".js"
    )

    target_link_options(c3engine_single PRIVATE
        -O3
        "SHELL:-s WASM=1"
        "SHELL:-s ALLOW_MEMORY_GROWTH=1"
        "SHELL:-s INITIAL_MEMORY=33554432"
        "SHELL:-s MAXIMUM_MEMORY=536870912"
        "SHELL:-s STACK_SIZE=1048576"
        "SHELL:-s MALLOC=emmalloc"
        "SHELL:-s EXPORTED_FUNCTIONS=_main,_c3_uci_command,_c3_stop"
        "SHELL:-s EXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,stringToUTF8,lengthBytesUTF8"
        "SHELL:-s ENVIRONMENT=worker,node"
        "SHELL:-s MODULARIZE=1"
        "SHELL:-s EXPORT_NAME=C3Engine"
        "SHELL:-s NO_EXIT_RUNTIME=1"
        "SHELL:-s SINGLE_FILE=1"
        "SHELL:-s ASSERTIONS=0"
    )

# ════════════════════════════════════════════════════════════════════════════════
# Native build  (cmake -DCMAKE_BUILD_TYPE=Release)
# ════════════════════════════════════════════════════════════════════════════════
else()

    target_compile_options(c3engine PRIVATE
        -O3
        -DNDEBUG
        -march=native           # use host CPU features (BMI2, POPCNT, etc.)
        # Lazy SMP is enabled in the native build by default — it is gated on
        # the ABSENCE of C3_NO_SMP (which the WASM build defines), not on any
        # positive define here.  No extra flag needed.
    )

    # ── Syzygy tablebase support (Fathom library) ────────────────────────────────
    # Fathom is a public-domain C library for Syzygy WDL + DTZ probing.
    # Source: https://github.com/jdart1/Fathom
    # Place tbprobe.c and tbprobe.h in src/ alongside the engine sources.
    #
    # Fathom is a C file, not C++ — it must be compiled as C99 (not C++17).
    # We add it as a separate source with an overriding language property so
    # the C++ standard flag on the main target does not affect it.
    #
    # TB_PROBE_LIMIT: maximum piece count for which interior TB probing is
    # enabled.  Default 6 (covers all 3–6 piece TBs).  Override with:
    #   cmake -DTB_PROBE_LIMIT=5 ..
    # to restrict to 5-piece TBs (~1 GB total) and skip the 6-piece files
    # (~150 GB total).
    if(NOT DEFINED TB_PROBE_LIMIT)
        set(TB_PROBE_LIMIT 6)
    endif()

    # Check whether Fathom source is present; skip gracefully if absent.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/tbprobe.c")
        target_sources(c3engine PRIVATE src/tbprobe.c)

        # Compile tbprobe.c as C (it uses C99 designated initialisers and
        # VLAs that are not valid C++).
        set_source_files_properties(src/tbprobe.c PROPERTIES LANGUAGE C)

        # Fathom needs C99 and position-independent code.
        # -DTB_NO_THREADS: Fathom has optional thread-local storage; we use our
        # own g_tbHits counter and don't need Fathom's internal TLS.
        set_source_files_properties(src/tbprobe.c PROPERTIES
            COMPILE_FLAGS "-std=c99 -O3 -DTB_NO_THREADS"
        )

        target_compile_definitions(c3engine PRIVATE
            C3_HAS_SYZYGY=1             # syzygy.cpp uses this to confirm Fathom is present
            TB_PROBE_LIMIT=${TB_PROBE_LIMIT}
        )

        message(STATUS "Syzygy/Fathom: enabled (TB_PROBE_LIMIT=${TB_PROBE_LIMIT})")
    else()
        message(STATUS "Syzygy/Fathom: tbprobe.c not found in src/ — TB probing disabled")
        message(STATUS "  To enable: place tbprobe.c + tbprobe.h from https://github.com/jdart1/Fathom into src/")
    endif()

    # pthreads — required for std::thread (Lazy SMP, Upgrade 6) and
    # std::chrono on some Linux toolchains.
    find_package(Threads REQUIRED)
    target_link_libraries(c3engine PRIVATE Threads::Threads)

endif()

# ── Install (optional — useful for packaging the WASM pair) ─────────────────────
if(EMSCRIPTEN)
    install(FILES
        "${CMAKE_BINARY_DIR}/c3engine.js"
        "${CMAKE_BINARY_DIR}/c3engine.wasm"
        "${CMAKE_BINARY_DIR}/c3engine_single.js"
        DESTINATION dist
    )
endif()
