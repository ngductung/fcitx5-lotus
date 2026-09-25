// SPDX-License-Identifier: GPL-3.0-or-later
#include "lotus-surrounding.h"
#include "bamboo-core.h"
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <vector>

int main() {
    struct Case {
        std::string text;
        std::string word;
    };
    const std::vector<Case> cases = {
        {"", ""}, {"hello", "hello"}, {"hello world", "world"},
        {"test{", ""}, {"test]", ""}, {"test123", ""},
        {"\u0111\u01b0\u1ee3c", "\u0111\u01b0\u1ee3c"},
        {"\U0001F600", ""}, {"hello\U0001F600", ""},
        {"\U0001F600d", "d"}, {"\U0001F600\u0111", "\u0111"},
        {"\U0001F600c\u00f4ng", "c\u00f4ng"},
        {"\U0001F600m\u1ed7i", "m\u1ed7i"},
        {"\u2764\uFE0F", ""}, {"\u2764\uFE0Fa", "a"},
        {"\U0001F44D\U0001F3FD", ""}, {"\U0001F44D\U0001F3FDo", "o"},
        {"\U0001F469\u200D\U0001F4BB", ""},
        {"\U0001F469\u200D\U0001F4BBu", "u"},
        {"\U0001F1FB\U0001F1F3", ""}, {"\U0001F1FB\U0001F1F3a", "a"},
        {"1\uFE0F\u20E3", ""}, {"1\uFE0F\u20E3a", "a"},
        {"\uFFFC", ""}, {"\uFFFCa", "a"},
        {"\u00A0a", "a"}, {"\u200Ba", "a"},
        {"\U0001F600a\u0301", "a\u0301"},
        {"abcdefghijklmnop", "bcdefghijklmnop"},
    };
    int failures = 0;
    auto check = [&](const std::string& text, unsigned int cursor, const std::string& expected) {
        auto actual = fcitx::surroundingWordBeforeCursor(text, cursor);
        if (actual != expected) {
            std::cerr << "text='" << text << "' cursor=" << cursor << " expected='" << expected << "' actual='" << actual << "'\n";
            ++failures;
        }
    };
    for (const auto& test : cases) {
        check(test.text, fcitx::utf8::length(test.text), test.word);
    }
    check("\U0001F600abc", 0, "");
    check("\U0001F600abc", 1, "");
    check("\U0001F600abc", 2, "a");
    check("\U0001F600abc", 100, "");
    check("\xF0\x9F", 1, "");

    const int fd = open("/dev/null", O_RDONLY);
    if (fd < 0) {
        return 1;
    }
    const auto dictionary = NewDictionary(fd); // Bamboo owns and closes fd.
    char* macros[] = {nullptr};
    const auto table = NewMacroTable(macros);
    const auto engine = NewEngine("Telex", dictionary, table);
    FcitxBambooEngineOption options{};
    options.ddFreeStyle = true;
    options.outputCharset = "Unicode";
    options.timeFormat = "%H:%M";
    options.dateFormat = "%d/%m/%Y";
    EngineSetOption(engine, &options);
    const std::vector<Case> sequences = {
        {"dd", "\u0111"}, {"as", "\u00e1"}, {"af", "\u00e0"},
        {"ow", "\u01a1"}, {"uw", "\u01b0"}, {"oo", "\u00f4"},
        {"coong", "c\u00f4ng"}, {"mooxi", "m\u1ed7i"},
    };
    const std::vector<std::string> prefixes = {
        "", "\U0001F600", "text\U0001F600", "\u2764\uFE0F",
        "\U0001F44D\U0001F3FD", "\U0001F469\u200D\U0001F4BB", "\uFFFC",
    };
    for (const auto& prefix : prefixes) {
        for (const auto& sequence : sequences) {
            ResetEngine(engine);
            auto word = fcitx::surroundingWordBeforeCursor(prefix, fcitx::utf8::length(prefix));
            if (!word.empty()) {
                EngineRebuildFromText(engine, word.c_str());
            }
            std::string displayed = prefix;
            for (const unsigned char key : sequence.text) {
                EngineProcessKeyEvent(engine, key, 0);
                char* commit = EnginePullCommit(engine);
                char* preedit = EnginePullPreedit(engine);
                const std::string next = std::string(commit) + preedit;
                displayed.replace(displayed.size() - word.size(), word.size(), next);
                word = preedit;
                std::free(commit);
                std::free(preedit);
            }
            if (displayed != prefix + sequence.word) {
                std::cerr << "prefix='" << prefix << "' keys='" << sequence.text << "' result='" << displayed << "'\n";
                ++failures;
            }
        }
    }
    DeleteObject(engine);
    DeleteObject(table);
    DeleteObject(dictionary);
    return failures ? 1 : 0;
}
