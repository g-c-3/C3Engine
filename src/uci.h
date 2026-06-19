#pragma once

// ═══════════════════════════════════════════════════════════════════════════════
// uci.h — UCI protocol handler public interface for C3Engine
//
// C3Engine — JS → C++ translation
//
// The UCI layer owns:
//   • handleLine()  — dispatch one raw UCI line (called by uciLoop / WASM export)
//   • uciLoop()     — blocking stdin→handleLine loop (native build; main.cpp calls this)
//
// WASM entry points (c3_uci_command / c3_stop) are declared with
// EMSCRIPTEN_KEEPALIVE inside uci.cpp and listed under EXPORTED_FUNCTIONS
// in CMakeLists.txt — they are NOT declared here because extern "C" linkage
// is incompatible with the C++ #pragma once guard style used across this
// project. The JS glue calls them via cwrap() without needing a C++ header.
//
// ── Option state ─────────────────────────────────────────────────────────────
//   uciContempt — file-static in uci.cpp; set by "setoption name ContemptValue"
//   uciHashMB   — file-static in uci.cpp; set by "setoption name Hash"
//   uciC3Mode   — file-static in uci.cpp; set by "setoption name C3Mode"
//
//   These are not exposed as externs — they are accessed only through the UCI
//   setoption command. If search.cpp or eval.cpp ever need contempt it is
//   passed as a parameter, not read from a global.
//
// ═══════════════════════════════════════════════════════════════════════════════

#include <string>

// ─── handleLine ───────────────────────────────────────────────────────────────
// Dispatch one raw UCI input line to the correct handler.
// Trims leading whitespace; ignores blank lines and unknown commands.
// Called by uciLoop() in the native build, and by c3_uci_command() in WASM.
void handleLine(const std::string& rawLine);

// ─── uciLoop ──────────────────────────────────────────────────────────────────
// Blocking stdin → handleLine() loop for the native binary.
// Reads std::cin line by line until EOF or "quit".
// Not compiled / not called in the WASM build (main.cpp guards this).
void uciLoop();
