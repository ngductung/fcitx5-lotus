// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef FCITX5_LOTUS_SURROUNDING_H
#define FCITX5_LOTUS_SURROUNDING_H

#include <fcitx-utils/utf8.h>
#include <cstdint>
#include <string>

namespace fcitx {

inline bool isSurroundingWordCharacter(uint32_t ch) {
    // Only rebuild letters and combining accents. Emoji, their joiners and
    // variation selectors must never become part of a replaceable word.
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
           (ch >= 0x00C0 && ch <= 0x00D6) || (ch >= 0x00D8 && ch <= 0x00F6) ||
           (ch >= 0x00F8 && ch <= 0x024F) || (ch >= 0x0300 && ch <= 0x036F) ||
           (ch >= 0x1E00 && ch <= 0x1EFF);
}

inline std::string surroundingWordBeforeCursor(const std::string& text, unsigned int cursor) {
    const auto length = utf8::lengthValidated(text);
    if (length == utf8::INVALID_LENGTH || cursor == 0 || cursor > length) {
        return {};
    }

    auto start = utf8::nextNChar(text.begin(), cursor);
    const auto end = start;
    for (int count = 0; start != text.begin() && count < 15; ++count) {
        auto prev = start;
        --prev;
        while (prev != text.begin() && (static_cast<unsigned char>(*prev) & 0xC0) == 0x80) {
            --prev;
        }
        if (!isSurroundingWordCharacter(utf8::getChar(prev, text.end()))) {
            break;
        }
        start = prev;
    }
    return {start, end};
}

} // namespace fcitx
#endif
