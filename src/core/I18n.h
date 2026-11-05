// I18n.h — minimal prose-layer translation for the TUI and the updater.
//
// Design (see the TUI i18n design doc):
//   - The strings live in src/resources/i18n.json, not in this header: one
//     object keyed by id, each value { "en": ..., "zh-Hans": ... }. The file is
//     embedded at build time (CMake generates a source that carries it as a
//     raw string literal, MSBuild embeds it as RCDATA) and parsed once on the
//     first Tr() call, so no init call is needed in any entry point.
//   - A file named by the TCMT_I18N environment variable, or an i18n.json next
//     to the executable, replaces the embedded copy without a rebuild.
//   - ONLY the prose layer is translated (sentences / actions / states).
//     Panel titles, metric names, key names, units and short status tokens
//     (CPU / Use / S / GB/s / stale / FOLLOW / ON / N/A ...) stay English —
//     they are domain vocabulary and CJK would blow the 80x24 panel budget.
//   - Selection chain: TCMT_LANG > active locale (LC_ALL/LC_MESSAGES/LANG) > en.
//     A non-UTF-8 locale forces en (CJK cannot render).
#pragma once

#include <cstddef>
#include <cstring>

namespace tcmt {

enum class Lang { En, ZhHans };

// Single shared language state across translation units (C++17 inline var).
inline Lang g_lang = Lang::En;

// Look up a prose string by id. Unknown ids return the id itself (visible in
// development) rather than crashing or silently rendering empty.
const char* Tr(const char* id);

// The table compiled into the binary. The build provides it: a generated
// source under CMake, resources.rc under MSBuild.
const char* I18nEmbeddedJson(size_t& len);

// Selection chain: TCMT_LANG > active locale > en. Non-UTF-8 forces en.
// `localeName` is the value returned by setlocale(LC_ALL, nullptr) after
// setlocale(LC_ALL, "") — it already reflects LC_ALL/LC_MESSAGES/LANG.
inline void I18nSelect(const char* tcmtLang, const char* localeName, bool utf8Locale) {
    if (!utf8Locale) {
        g_lang = Lang::En;
        return;
    }
    const char* pick = (tcmtLang && *tcmtLang) ? tcmtLang : localeName;
    if (pick && std::strncmp(pick, "zh", 2) == 0)
        g_lang = Lang::ZhHans;
    else
        g_lang = Lang::En;
}

} // namespace tcmt
