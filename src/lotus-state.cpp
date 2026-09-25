/*
 * SPDX-FileCopyrightText: 2022-2022 CSSlayer <wengxt@gmail.com>
 * SPDX-FileCopyrightText: 2025 Võ Ngô Hoàng Thành <thanhpy2009@gmail.com>
 * SPDX-FileCopyrightText: 2026 Nguyễn Hoàng Kỳ  <nhktmdzhg@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */
#include "lotus-state.h"
#include "lotus-engine.h"
#include "lotus-candidates.h"
#include "lotus-utils.h"
#include "lotus-surrounding.h"
#include "lotus.h"

#include <cstddef>
#include <fcitx-utils/log.h>
#include <fcitx-utils/utf8.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputpanel.h>
#include <fcitx/menu.h>
#include <fcitx/userinterface.h>

#include <algorithm>
#include <limits>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>

#include <thread>

namespace fcitx {
    constexpr uint64_t UINPUT_EMPTY_REPLACEMENT_FALLBACK_USEC = 120000;
    constexpr uint64_t UINPUT_READY_CHECK_INITIAL_USEC        = 24000;
    constexpr uint64_t UINPUT_READY_CHECK_INTERVAL_USEC       = 8000;
    constexpr uint64_t UINPUT_OBSERVED_BACKSPACE_COMMIT_USEC  = 43000;
    constexpr uint64_t UINPUT_TERMINAL_BACKSPACE_COMMIT_USEC = 8000;
    constexpr uint64_t UINPUT_INITIAL_HOLD_USEC              = 24000;
    // Server paces generated backspaces at 16 ms each; never give up before they can all arrive.
    constexpr uint64_t UINPUT_SERVER_BACKSPACE_INTERVAL_USEC = 16000;
    constexpr uint64_t UINPUT_BACKSPACE_ARRIVAL_SLACK_USEC   = 60000;
    // sd-event treats accuracy 0 as its 250 ms default, which makes these short timers fire far too late.
    constexpr uint64_t UINPUT_TIMER_ACCURACY_USEC            = 1000;
    constexpr uint64_t UINPUT_DIRECT_CONFIRM_POLL_USEC       = 2000;
    constexpr uint64_t UINPUT_DIRECT_CONFIRM_TIMEOUT_USEC    = 30000;
    constexpr bool     UINPUT_HOLD_INITIAL_PREEDIT            = true;

    static inline bool isWordBreak(uint32_t ucs4) {
        // Match Bamboo's punctuation-as-word-break behavior for surrounding-text rebuilds.
        return ucs4 == 0xFFFC || ucs4 == ' ' || ucs4 == '\t' || ucs4 == '\n' || ucs4 == '\r' || ucs4 == 0 || (ucs4 >= '!' && ucs4 <= '/') ||
               (ucs4 >= '0' && ucs4 <= '9') || (ucs4 >= ':' && ucs4 <= '@') || (ucs4 >= '[' && ucs4 <= '`') || (ucs4 >= '{' && ucs4 <= '~');
    }

    static inline bool endsWithWordBreak(const std::string& text) {
        return !text.empty() && static_cast<unsigned char>(text.back()) < 0x80 && isWordBreak(static_cast<unsigned char>(text.back()));
    }

    static inline bool containsObjectReplacementChar(const std::string& text) {
        return text.find("\xEF\xBF\xBC") != std::string::npos;
    }

    static inline std::string keySymToBufferedUtf8(KeySym sym) {
        if (sym == FcitxKey_space || sym == FcitxKey_KP_Space) {
            return " ";
        }
        return Key::keySymToUTF8(sym);
    }

    static inline bool canHoldInitialUinputKey(KeySym sym) {
        if (sym >= FcitxKey_A && sym <= FcitxKey_Z) {
            sym = static_cast<KeySym>(sym + (FcitxKey_a - FcitxKey_A));
        }
        return sym == FcitxKey_a || sym == FcitxKey_e || sym == FcitxKey_i || sym == FcitxKey_o || sym == FcitxKey_u || sym == FcitxKey_y || sym == FcitxKey_d;
    }

    static inline bool isLiteralBraceKey(KeySym sym, uint32_t state) {
        std::string keyUtf8 = Key::keySymToUTF8(sym);
        return keyUtf8 == "{" || keyUtf8 == "}" ||
               ((state & static_cast<uint32_t>(KeyState::Shift)) != 0U && (sym == FcitxKey_bracketleft || sym == FcitxKey_bracketright));
    }

    static inline bool isEnterKey(KeySym sym) {
        return sym == FcitxKey_Return || sym == FcitxKey_KP_Enter;
    }

    static inline bool shouldBufferPendingUinputKey(KeySym sym) {
        return !keySymToBufferedUtf8(sym).empty() || isEnterKey(sym);
    }

    static inline void forwardCurrentKeyDirect(KeyEvent& keyEvent) {
        keyEvent.filterAndAccept();
        keyEvent.inputContext()->forwardKey(keyEvent.rawKey(), false, keyEvent.time());
    }

    static inline void replayBufferedSpecialKey(InputContext* ic, KeySym sym, uint32_t state, int code) {
        Key key(sym, KeyStates(state), code);
        ic->forwardKey(key);
        ic->forwardKey(key, true);
    }

    static inline bool isAnonymousIbusContext(InputContext* ic) {
        return ic != nullptr && ic->program().empty() && getFrontendName(ic) == "ibus";
    }

    static inline bool isTerminalProgram(std::string appName) {
#if __cplusplus >= 202002L
        std::ranges::transform(appName, appName.begin(), ::tolower);
#else
        std::transform(appName.begin(), appName.end(), appName.begin(), ::tolower);
#endif
        return appName.find("terminal") != std::string::npos || appName.find("konsole") != std::string::npos || appName.find("alacritty") != std::string::npos ||
               appName.find("kitty") != std::string::npos || appName.find("wezterm") != std::string::npos || appName == "kgx" || appName == "foot" || appName == "xterm";
    }

    static inline bool shouldDebugAnonymousIbus(InputContext*) {
        return lotusTraceEnabled();
    }

    static inline bool canUseTrustedUnvalidatedSurroundingDelete(InputContext* ic) {
        if (ic == nullptr) {
            return false;
        }

        auto frontend = getFrontendName(ic);
#if __cplusplus >= 202002L
        std::ranges::transform(frontend, frontend.begin(), ::tolower);
#else
        std::transform(frontend.begin(), frontend.end(), frontend.begin(), ::tolower);
#endif
        return !frontend.empty() && frontend.find("wayland") == std::string::npos;
    }

    static inline bool deleteSurroundingTextBeforeCursorSafely(InputContext* ic, unsigned int charsToDelete, const std::string* expectedDeleted = nullptr,
                                                               bool allowTrustedUnvalidatedDelete = false) {
        if (ic == nullptr || charsToDelete == 0 || charsToDelete > static_cast<unsigned int>(std::numeric_limits<int>::max()) ||
            !ic->capabilityFlags().test(CapabilityFlag::SurroundingText)) {
            return false;
        }

        const auto& surrounding = ic->surroundingText();
        if (!surrounding.isValid() || surrounding.cursor() != surrounding.anchor()) {
            if (allowTrustedUnvalidatedDelete && canUseTrustedUnvalidatedSurroundingDelete(ic)) {
                ic->deleteSurroundingText(-static_cast<int>(charsToDelete), static_cast<int>(charsToDelete));
                return true;
            }
            return false;
        }

        const auto& text = surrounding.text();
        const auto  textLen = utf8::lengthValidated(text);
        const auto  cursor = surrounding.cursor();
        if (textLen == utf8::INVALID_LENGTH || cursor > textLen || cursor < charsToDelete) {
            if (allowTrustedUnvalidatedDelete && canUseTrustedUnvalidatedSurroundingDelete(ic)) {
                ic->deleteSurroundingText(-static_cast<int>(charsToDelete), static_cast<int>(charsToDelete));
                return true;
            }
            return false;
        }

        auto actualStart = utf8::nextNChar(text.begin(), cursor - charsToDelete);
        auto actualEnd = utf8::nextNChar(text.begin(), cursor);
        std::string actualDeleted(actualStart, actualEnd);
        if (containsObjectReplacementChar(actualDeleted)) {
            return false;
        }

        if (expectedDeleted != nullptr) {
            if (utf8::lengthValidated(*expectedDeleted) == utf8::INVALID_LENGTH || utf8::length(*expectedDeleted) != charsToDelete || actualDeleted != *expectedDeleted) {
                if (allowTrustedUnvalidatedDelete && canUseTrustedUnvalidatedSurroundingDelete(ic)) {
                    ic->deleteSurroundingText(-static_cast<int>(charsToDelete), static_cast<int>(charsToDelete));
                    return true;
                }
                return false;
            }
        }

        ic->deleteSurroundingText(-static_cast<int>(charsToDelete), static_cast<int>(charsToDelete));
        return true;
    }

    static inline bool surroundingTextBeforeCursorEndsWith(const SurroundingText& surrounding, const std::string& suffix) {
        if (!surrounding.isValid() || suffix.empty()) {
            return false;
        }

        const auto suffixLen = utf8::length(suffix);
        const auto cursor = surrounding.cursor();
        if (cursor < suffixLen) {
            return false;
        }

        const auto& text = surrounding.text();
        if (utf8::length(text) < cursor) {
            return false;
        }

        auto start = utf8::nextNChar(text.begin(), cursor - suffixLen);
        auto end = utf8::nextNChar(text.begin(), cursor);
        return std::string(start, end) == suffix;
    }

    static inline void debugAnonymousIbusTrace(const std::string& msg) {
        lotusTrace(msg);
    }

    static inline bool isUinputDebugEnabled() {
        return lotusTraceEnabled();
    }

#define debugUinputTrace(msg)      \
    do {                           \
        if (lotusTraceEnabled()) { \
            lotusTrace(msg);       \
        }                          \
    } while (false)

    static inline std::string previousWordFromSurrounding(const SurroundingText& surrounding) {
        if (!surrounding.isValid() || surrounding.cursor() != surrounding.anchor()) {
            return "";
        }

        return surroundingWordBeforeCursor(surrounding.text(), surrounding.cursor());
    }

    LotusState::LotusState(LotusEngine* engine, InputContext* ic) : engine_(engine), ic_(ic) {
        setEngine();
    }

    void LotusState::setEngine() {
        lotusEngine_.reset();
        realMode = engine_->config().mode.value();

        if (engine_->config().inputMethod.value() == "Custom") {
            const auto&        keymaps = *engine_->customKeymap().customKeymap;
            std::vector<char*> charArray;
            charArray.reserve((keymaps.size() * 2) + 1);
            for (const auto& keymap : keymaps) {
                charArray.push_back(const_cast<char*>(keymap.key->data()));   //NOLINT
                charArray.push_back(const_cast<char*>(keymap.value->data())); //NOLINT
            }
            charArray.push_back(nullptr);
            lotusEngine_.reset(NewCustomEngine(charArray.data(), engine_->dictionary(), engine_->macroTable()));
        } else {
            lotusEngine_.reset(NewEngine(engine_->config().inputMethod->data(), engine_->dictionary(), engine_->macroTable()));
        }
        setOption();
    }

    void LotusState::setOption() {
        if (!lotusEngine_)
            return;
        FcitxBambooEngineOption option = {
            .autoNonVnRestore    = *engine_->config().autoNonVnRestore,
            .ddFreeStyle         = *engine_->config().ddFreeStyle,
            .macroEnabled        = *engine_->config().enableMacro,
            .autoCapitalizeMacro = *engine_->config().capitalizeMacro,
            .spellCheckWithDicts = *engine_->config().spellCheck,
            .outputCharset       = engine_->config().outputCharset->data(),
            .modernStyle         = *engine_->config().modernStyle,
            .freeMarking         = *engine_->config().freeMarking,
            .w2u                 = static_cast<int>(*engine_->config().w2u),
            .bracketTransform    = static_cast<int>(*engine_->config().bracketTransform),
            .timeFormat          = engine_->config().timeFormat->data(),
            .dateFormat          = engine_->config().dateFormat->data(),
        };

        EngineSetOption(lotusEngine_.handle(), &option);
    }

    bool LotusState::connect_uinput_server() {
        if (uinput_client_fd_ >= 0)
            return true;
        const std::string current_path = buildSocketPath("kb_socket");
        int               current_fd   = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0);
        if (current_fd < 0) {
            LOTUS_ERROR("Failed to create socket: " + std::string(strerror(errno)));
            return false;
        }

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;

        addr.sun_path[0] = '\0';
        memcpy(&addr.sun_path[1], current_path.c_str(), current_path.length());
        socklen_t len = offsetof(struct sockaddr_un, sun_path) + current_path.length() + 1;

        if (connect(current_fd, (struct sockaddr*)&addr, len) == 0) {
            uinput_client_fd_ = current_fd;
            return true;
        }
        LOTUS_ERROR("Failed to connect to socket: " + std::string(strerror(errno)));
        int old_fd = uinput_client_fd_.exchange(-1);
        if (old_fd != -1) {
            close(old_fd);
        }
        return false;
    }

    int LotusState::setup_uinput() {
        return connect_uinput_server() ? uinput_client_fd_.load(std::memory_order_acquire) : -1;
    }

    void LotusState::send_backspace_uinput(int count) const {
        if (uinput_client_fd_ < 0 && !connect_uinput_server()) {
            LOTUS_ERROR("Cannot send backspace since cannot connect to uinput server");
            return;
        }

        ssize_t n = send(uinput_client_fd_, &count, sizeof(count), MSG_NOSIGNAL);

        if (n < 0) {
            LOTUS_WARN("Failed to send backspace: " + std::string(strerror(errno)));
            int old_fd = uinput_client_fd_.exchange(-1);
            if (old_fd != -1) {
                close(old_fd);
            }
            if (connect_uinput_server()) {
                LOTUS_DEBUG("Reconnected to uinput server successfully");
                send(uinput_client_fd_, &count, sizeof(count), MSG_NOSIGNAL);
            }
        }

        if (waitAck_) {
            LOTUS_DEBUG("Waiting for ack");
            std::this_thread::sleep_for(std::chrono::milliseconds(count * 5));
        }
    }

    bool LotusState::isAutofillCertain(const SurroundingText& s) {
        if (!s.isValid() || oldPreBuffer_.empty()) {
            return false;
        }

        const unsigned int cursor  = s.cursor();
        const unsigned int anchor  = s.anchor();
        const auto&        text    = s.text();
        const size_t       textLen = utf8::length(text);

        // Fix that surrounding text is delay update
        const size_t buffLen    = utf8::length(oldPreBuffer_);
        const size_t pb         = text.find(oldPreBuffer_);
        size_t       rangeStart = static_cast<size_t>(cursor) >= buffLen ? static_cast<size_t>(cursor) - buffLen : 0;
        const bool   sameprefix = pb != std::string::npos && pb >= rangeStart && pb <= static_cast<size_t>(cursor);

        // Detect browser autofill/autocomplete suggestions via selection.
        if (cursor != anchor) {
            unsigned int selectionStart = std::min(anchor, cursor);
            unsigned int selectionEnd   = std::max(anchor, cursor);

            // Only consider it browser autofill if the selection starts at the cursor
            // and extends to the end of the line (common address bar behavior).
            if (selectionStart >= cursor || (selectionStart < cursor && selectionEnd > cursor)) {
                if (!sameprefix)
                    return false;
                // If the selection contains a newline, it's likely a multiline editor (AI ghost text),
                // not a single-line URL/Search bar.
                size_t p = text.find('\n', selectionStart);
                return p == std::string::npos || p >= static_cast<size_t>(selectionEnd);
            }
        }

        if (textLen == static_cast<size_t>(cursor)) {
            realtextLen.store(textLen, std::memory_order_release);
            return false;
        }

        // Heuristic: rapid text growth in a single-line context.
        // Applied only when no newline is present after the cursor to distinguish from AI text in editors.
        // Gecko/Firefox: if buffLen > textLen, surrounding text is stale (async update race)
        if (buffLen > textLen) {
            return false;
        }
        if (textLen > static_cast<size_t>(cursor) + 1 && cursor == realtextLen.load(std::memory_order_acquire) && text.find('\n', cursor) == std::string::npos && sameprefix)
            return true;

        for (auto v = realtextLen.load(std::memory_order_acquire); v < cursor && !realtextLen.compare_exchange_weak(v, cursor, std::memory_order_acq_rel);)
            ;
        return false;
    }

    void LotusState::handlePreeditMode(KeyEvent& keyEvent, KeySym currentSym) {
        if (currentSym == FcitxKey_Return || currentSym == FcitxKey_KP_Enter) {
            commitBuffer();
            keyEvent.forward();
            return;
        }

        if (EngineProcessKeyEvent(lotusEngine_.handle(), currentSym, keyEvent.rawKey().states()) != 0U)
            keyEvent.filterAndAccept();
        if (auto commit = UniqueCPtr<char>(EnginePullCommit(lotusEngine_.handle()))) {
            if (commit && (*commit.get() != 0)) {
                LOTUS_DEBUG("Commit: " + std::string(commit.get()));
                ic_->commitString(commit.get());
            }
        }
        ic_->inputPanel().reset();
        UniqueCPtr<char> preedit(EnginePullPreedit(lotusEngine_.handle()));
        if (preedit && (*preedit.get() != 0)) {
            std::string_view view = preedit.get();
            Text             text;
            TextFormatFlags  fmt = TextFormatFlag::NoFlag;
            if (utf8::validate(view))
                text.append(std::string(view), fmt);
            text.setCursor(static_cast<int>(text.textLength()));
            if (*engine_->config().inlinePreedit && ic_->capabilityFlags().test(CapabilityFlag::Preedit))
                ic_->inputPanel().setClientPreedit(text);
            else
                ic_->inputPanel().setPreedit(text);
        }
        ic_->updatePreedit();
        ic_->updateUserInterface(UserInterfaceComponent::InputPanel);
    }

    void LotusState::updateEmojiPageStatus(CommonCandidateList* commonList) {
        if ((commonList == nullptr) || commonList->empty()) {
            return;
        }

        int pageSize = commonList->pageSize();
        if (pageSize <= 0) {
            pageSize = 9;
        }

        int         totalItems  = commonList->totalSize();
        int         currentPage = commonList->currentPage() + 1;
        int         totalPages  = (totalItems + pageSize - 1) / pageSize;

        std::string status = _("Page ") + std::to_string(currentPage) + "/" + std::to_string(totalPages);
        ic_->inputPanel().setAuxDown(Text(status));
    }

    void LotusState::handleEmojiMode(KeyEvent& keyEvent) {
        const KeySym currentSym      = keyEvent.rawKey().sym();
        bool         isCtrlBackspace = isBackspace(currentSym) && ((keyEvent.rawKey().states() & KeyState::Ctrl) != 0U);

        if (keyEvent.key().hasModifier() && !isCtrlBackspace) {
            keyEvent.forward();
            return;
        }

        auto baseList   = ic_->inputPanel().candidateList();
        auto commonList = std::dynamic_pointer_cast<CommonCandidateList>(baseList);
        if (commonList && currentSym >= FcitxKey_1 && currentSym <= FcitxKey_9) {
            int offset      = currentSym - FcitxKey_1;
            int globalIndex = (commonList->currentPage() * commonList->pageSize()) + offset;

            if (globalIndex < commonList->totalSize()) {
                commonList->candidateFromAll(globalIndex).select(ic_);
                keyEvent.filterAndAccept();
                return;
            }
        }

        if (commonList && !commonList->empty()) {
            int  globalCursorIndex = commonList->globalCursorIndex();
            int  totalSize         = commonList->totalSize();
            int  currentPage       = commonList->currentPage();
            int  pageSize          = commonList->pageSize();
            int  localCursorIndex  = globalCursorIndex - (currentPage * pageSize);

            bool handled = false;

            switch (currentSym) {
                case FcitxKey_Tab:
                case FcitxKey_Down: {
                    if (localCursorIndex < pageSize - 1 && globalCursorIndex < totalSize - 1) {
                        commonList->setGlobalCursorIndex(globalCursorIndex + 1);
                    } else {
                        commonList->setGlobalCursorIndex(currentPage * pageSize);
                    }
                    handled = true;
                    break;
                }

                case FcitxKey_ISO_Left_Tab:
                case FcitxKey_Up: {
                    if (localCursorIndex > 0) {
                        commonList->setGlobalCursorIndex(globalCursorIndex - 1);
                    } else {
                        int lastIndex = std::min((currentPage * pageSize) + pageSize - 1, totalSize - 1);
                        commonList->setGlobalCursorIndex(lastIndex);
                    }
                    handled = true;
                    break;
                }
                case FcitxKey_Page_Down:
                case FcitxKey_Right: {
                    if (commonList->hasNext()) {
                        commonList->next();
                        int newPage = commonList->currentPage();
                        commonList->setGlobalCursorIndex(newPage * pageSize);
                        handled = true;
                    }
                    break;
                }
                case FcitxKey_Page_Up:
                case FcitxKey_Left: {
                    if (commonList->hasPrev()) {
                        commonList->prev();
                        int newPage = commonList->currentPage();
                        commonList->setGlobalCursorIndex(newPage * pageSize);
                        handled = true;
                    }
                    break;
                }
                default: break;
            }

            if (handled) {
                updateEmojiPageStatus(commonList.get());
                ic_->updateUserInterface(UserInterfaceComponent::InputPanel);
                keyEvent.filterAndAccept();
                return;
            }
        }

        if (isBackspace(currentSym)) {
            if (!emojiBuffer_.empty()) {
                if (isCtrlBackspace) {
                    emojiBuffer_.clear();
                } else {
                    emojiBuffer_.pop_back();
                    while (!emojiBuffer_.empty() && (emojiBuffer_.back() & 0xC0) == 0x80) {
                        emojiBuffer_.pop_back();
                    }
                }
                keyEvent.filterAndAccept();
            } else {
                keyEvent.forward();
            }
            updateEmojiPreedit();
            return;
        }

        switch (currentSym) {
            case FcitxKey_space:
            case FcitxKey_Return: {
                if (commonList && !commonList->empty()) {
                    int globalIdx = commonList->globalCursorIndex();
                    commonList->candidateFromAll(globalIdx).select(ic_);
                    keyEvent.filterAndAccept();
                } else if (currentSym == FcitxKey_Return && !emojiBuffer_.empty()) {
                    ic_->commitString(emojiBuffer_);
                    emojiBuffer_.clear();
                    updateEmojiPreedit();
                    keyEvent.filterAndAccept();
                } else {
                    keyEvent.forward();
                }
                return;
            }

            case FcitxKey_Escape: {
                emojiBuffer_.clear();
                emojiCandidates_.clear();
                ic_->inputPanel().reset();
                ic_->updateUserInterface(UserInterfaceComponent::InputPanel);
                keyEvent.filterAndAccept();
                return;
            }

            default: break;
        }

        {
            std::string utf8Char = Key::keySymToUTF8(currentSym);
            if (!utf8Char.empty()) {
                emojiBuffer_.append(utf8Char);
                keyEvent.filterAndAccept();
                updateEmojiPreedit();
            } else {
                keyEvent.forward();
            }
        }
    }
    void LotusState::updateEmojiPreedit() {
        if (emojiBuffer_.empty()) {
            emojiCandidates_ = engine_->emojiLoader().history();
            if (emojiCandidates_.empty()) {
                ic_->inputPanel().reset();
                ic_->updatePreedit();
                ic_->updateUserInterface(UserInterfaceComponent::InputPanel);
                return;
            }
        } else {
            emojiCandidates_ = engine_->emojiLoader().search(emojiBuffer_);
        }

        if (!emojiBuffer_.empty()) {
            Text preeditText;
            preeditText.append(emojiBuffer_, TextFormatFlag::Underline);
            preeditText.setCursor(static_cast<int>(preeditText.textLength()));
            if (ic_->capabilityFlags().test(CapabilityFlag::Preedit))
                ic_->inputPanel().setClientPreedit(preeditText);
            else
                ic_->inputPanel().setPreedit(preeditText);
        } else {
            ic_->inputPanel().setClientPreedit(Text());
            ic_->inputPanel().setPreedit(Text());
        }

        if (!emojiCandidates_.empty()) {
            auto candidateList = std::make_unique<CommonCandidateList>();
            candidateList->setLayoutHint(CandidateLayoutHint::Vertical);
            candidateList->setPageSize(9);

            for (size_t i = 0; i < emojiCandidates_.size(); ++i) {
                size_t localIndex = (i % 9) + 1;
                Text   displayLabel;
                if (emojiBuffer_.empty()) {
                    displayLabel.append(std::to_string(localIndex) + ": " + emojiCandidates_[i].output, TextFormatFlag::NoFlag);
                } else {
                    displayLabel.append(std::to_string(localIndex) + ": " + emojiCandidates_[i].trigger + " " + emojiCandidates_[i].output, TextFormatFlag::NoFlag);
                }
                candidateList->append(std::make_unique<EmojiCandidateWord>(displayLabel, this, emojiCandidates_[i]));
            }
            candidateList->setGlobalCursorIndex(0);

            ic_->inputPanel().setCandidateList(std::move(candidateList));
            auto currentList = std::dynamic_pointer_cast<CommonCandidateList>(ic_->inputPanel().candidateList());
            updateEmojiPageStatus(currentList.get());
        } else {
            ic_->inputPanel().setCandidateList(nullptr);
        }

        ic_->updatePreedit();
        ic_->updateUserInterface(UserInterfaceComponent::InputPanel);
    }

    bool LotusState::handleUInputKeyPress(KeyEvent& event, KeySym currentSym, int sleepTime) {
        if (!is_deleting_.load()) {
            return false;
        }
        if (isBackspace(currentSym)) {
            current_backspace_count_ += 1;
            if (shouldDebugAnonymousIbus(ic_)) {
                std::ostringstream oss;
                oss << "observed_backspace count=" << current_backspace_count_ << "/" << expected_backspaces_ << " timer=" << timer_driven_replacement_
                    << " realLen=" << realtextLen.load(std::memory_order_acquire) << " pending='" << pending_commit_string_ << "'";
                debugAnonymousIbusTrace(oss.str());
            }
            if (isUinputDebugEnabled()) {
                std::ostringstream oss;
                oss << "observed_backspace count=" << current_backspace_count_ << "/" << expected_backspaces_ << " timer=" << timer_driven_replacement_
                    << " realLen=" << realtextLen.load(std::memory_order_acquire) << " pending='" << pending_commit_string_ << "'";
                debugUinputTrace(oss.str());
            }
            if (timer_driven_replacement_) {
                if (realtextLen.load(std::memory_order_acquire) > 0) {
                    realtextLen.fetch_sub(1, std::memory_order_acq_rel);
                }
                if (current_backspace_count_ >= expected_backspaces_) {
                    if (isTerminalProgram(ic_->program())) {
                        debugUinputTrace("observed_all_backspaces schedule_terminal_commit_delay_usec=" + std::to_string(UINPUT_TERMINAL_BACKSPACE_COMMIT_USEC));
                        schedulePendingReplacementFallback(expected_backspaces_, true, realtextLen.load(std::memory_order_acquire), false,
                                                           UINPUT_TERMINAL_BACKSPACE_COMMIT_USEC);
                    } else if (pending_replacement_may_empty_input_) {
                        debugUinputTrace("observed_all_backspaces keep_empty_input_fallback_usec=" + std::to_string(UINPUT_EMPTY_REPLACEMENT_FALLBACK_USEC));
                    } else {
                        debugUinputTrace("observed_all_backspaces schedule_commit_delay_usec=" + std::to_string(UINPUT_OBSERVED_BACKSPACE_COMMIT_USEC));
                        schedulePendingReplacementFallback(expected_backspaces_, true, realtextLen.load(std::memory_order_acquire), false, UINPUT_OBSERVED_BACKSPACE_COMMIT_USEC);
                    }
                }
                return false;
            }
            if (current_backspace_count_ < expected_backspaces_) {
                if (realtextLen.load(std::memory_order_acquire) > 0) {
                    realtextLen.fetch_sub(1, std::memory_order_acq_rel);
                }
                return false; // Allow intermediate backspaces to reach the app to clear autofill/old text.
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepTime));
            // Validate surr cursor pos should match realtextLen after all BS applied
            const auto& surr = ic_->surroundingText();
            if (surr.isValid() && surr.cursor() == realtextLen.load(std::memory_order_acquire)) {
                LOTUS_DEBUG("Skip retry");
            } else {
                // Retry x3 (2 ms each), khi can (chromium,electron,...)
                for (int retry = 0; retry < 3; ++retry) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    const auto& surr2 = ic_->surroundingText();
                    if (surr2.isValid() && surr2.cursor() == realtextLen.load(std::memory_order_acquire)) {
                        break;
                    }
                }
            }
            event.filterAndAccept(); // Filter out the final trigger backspace.
            finishPendingReplacement(true);
            return true;
        }
        return false;
    }

    void LotusState::finishPendingReplacement(bool replayBuffered, bool resetTimer) {
        if (resetTimer) {
            pending_commit_fallback_timer_.reset();
        }
        const auto commitString = pending_commit_string_;
        if (shouldDebugAnonymousIbus(ic_)) {
            std::ostringstream oss;
            oss << "finish_pending commit='" << commitString << "' replay=" << replayBuffered << " resetTimer=" << resetTimer
                << " resetEngine=" << pending_reset_engine_after_commit_ << " realLenBefore=" << realtextLen.load(std::memory_order_acquire)
                << " buffered=" << buffered_keys_.size();
            debugAnonymousIbusTrace(oss.str());
        }
        if (isUinputDebugEnabled()) {
            std::ostringstream oss;
            oss << "finish_pending commit='" << commitString << "' replay=" << replayBuffered << " resetTimer=" << resetTimer << " realLenBefore="
                << realtextLen.load(std::memory_order_acquire) << " buffered=" << buffered_keys_.size();
            debugUinputTrace(oss.str());
        }
        if (!commitString.empty()) {
            ic_->commitString(commitString);
            LOTUS_DEBUG("Commit: " + commitString);
            realtextLen.fetch_add(static_cast<unsigned int>(utf8::length(commitString)), std::memory_order_acq_rel);
        }
        const bool awaitCommit = !commitString.empty() && needsSerializedOutput();
        if (pending_reset_engine_after_commit_) {
            hasHistory_ = false;
            ResetEngine(lotusEngine_.handle());
            oldPreBuffer_.clear();
        }

        expected_backspaces_     = 0;
        current_backspace_count_ = 0;
        pending_commit_string_.clear();
        timer_driven_replacement_ = false;
        pending_replacement_may_empty_input_ = false;
        pending_initial_hold_timer_.reset();
        holding_initial_uinput_preedit_ = false;
        pending_reset_engine_after_commit_ = false;
        // We just committed the word break ourselves; surrounding text may still show the previous word.
        suppress_surrounding_seed_once_ = oldPreBuffer_.empty() && endsWithWordBreak(commitString);
        is_deleting_.store(false, std::memory_order_release);
        if (awaitCommit) {
            startDirectAwait();
        }

        if (replayBuffered) {
            scheduleReplayBufferedKeys();
        }
    }

    void LotusState::cancelPendingReplacement(bool replayBuffered, bool resetTimer) {
        if (isUinputDebugEnabled()) {
            std::ostringstream oss;
            oss << "cancel_pending commit='" << pending_commit_string_ << "' replay=" << replayBuffered << " resetTimer=" << resetTimer << " backspaces="
                << current_backspace_count_ << "/" << expected_backspaces_ << " realLen=" << realtextLen.load(std::memory_order_acquire);
            debugUinputTrace(oss.str());
        }
        if (resetTimer) {
            pending_commit_fallback_timer_.reset();
        }
        expected_backspaces_     = 0;
        current_backspace_count_ = 0;
        pending_commit_string_.clear();
        timer_driven_replacement_ = false;
        pending_replacement_may_empty_input_ = false;
        pending_initial_hold_timer_.reset();
        holding_initial_uinput_preedit_ = false;
        pending_reset_engine_after_commit_ = false;
        suppress_surrounding_seed_once_ = false;
        is_deleting_.store(false, std::memory_order_release);

        if (replayBuffered) {
            scheduleReplayBufferedKeys();
        }
    }

    void LotusState::scheduleReplayBufferedKeys() {
        pending_replay_event_.reset();
        if (needsSerializedOutput()) {
            // Replaying commits back-to-back would drop all but the last one; feed them one by one instead.
            pending_replay_scheduled_ = false;
            direct_keys_.insert(direct_keys_.begin(), buffered_keys_.begin(), buffered_keys_.end());
            buffered_keys_.clear();
            if (!direct_awaiting_) {
                scheduleDirectDrain();
            }
            return;
        }
        if (buffered_keys_.empty()) {
            pending_replay_scheduled_ = false;
            debugUinputTrace("schedule_replay skipped empty_buffer");
            return;
        }

        pending_replay_scheduled_ = true;
        debugUinputTrace("schedule_replay buffered=" + std::to_string(buffered_keys_.size()));
        pending_replay_event_ = engine_->instance()->eventLoop().addPostEvent([this, icRef = ic_->watch()](EventSource*) {
            pending_replay_scheduled_ = false;
            if (auto* ic = icRef.get(); ic && ic->hasFocus()) {
                replayBufferedKeys();
            } else {
                buffered_keys_.clear();
            }
            return false;
        });
    }

    void LotusState::releaseReplacementOnFocusChange() {
        if (is_deleting_.load(std::memory_order_acquire) && !pending_commit_fallback_timer_) {
            cancelPendingReplacement(false);
        }
    }

    void LotusState::flushHeldInitialUinput(bool resetTimer) {
        if (resetTimer) {
            pending_initial_hold_timer_.reset();
        }

        if (!holding_initial_uinput_preedit_) {
            return;
        }

        const auto commitString = oldPreBuffer_;
        {
            std::ostringstream oss;
            oss << "flush_initial_hold commit='" << commitString << "' realLenBefore=" << realtextLen.load(std::memory_order_acquire);
            debugUinputTrace(oss.str());
        }

        holding_initial_uinput_preedit_ = false;
        if (!commitString.empty()) {
            ic_->commitString(commitString);
            realtextLen.fetch_add(static_cast<unsigned int>(utf8::length(commitString)), std::memory_order_acq_rel);
            LOTUS_DEBUG("Commit: " + commitString);
        }
        hasHistory_ = !commitString.empty();
    }

    void LotusState::schedulePendingReplacementFallback(int deletedChars, bool requireBackspaceEvents, unsigned int targetCursor, bool allowEarlyReadyCheck, uint64_t fallbackUsec) {
        pending_commit_fallback_timer_.reset();
        if (deletedChars <= 0 || expected_backspaces_ < deletedChars) {
            if (isUinputDebugEnabled()) {
                std::ostringstream oss;
                oss << "schedule_fallback skipped deletedChars=" << deletedChars << " expected=" << expected_backspaces_;
                debugUinputTrace(oss.str());
            }
            return;
        }

        if (isUinputDebugEnabled()) {
            std::ostringstream oss;
            oss << "schedule_fallback deletedChars=" << deletedChars << " requireBackspaceEvents=" << requireBackspaceEvents << " targetCursor=" << targetCursor
                << " allowEarlyReadyCheck=" << allowEarlyReadyCheck << " fallbackUsec=" << fallbackUsec << " currentBackspaces=" << current_backspace_count_
                << " expected=" << expected_backspaces_;
            debugUinputTrace(oss.str());
        }
        if (const int outstanding = expected_backspaces_ - current_backspace_count_; outstanding > 0) {
            fallbackUsec = std::max(fallbackUsec, (static_cast<uint64_t>(outstanding) * UINPUT_SERVER_BACKSPACE_INTERVAL_USEC) + UINPUT_BACKSPACE_ARRIVAL_SLACK_USEC);
        }
        const auto now      = ::fcitx::now(CLOCK_MONOTONIC);
        const auto deadline = now + fallbackUsec;
        const auto timeout  = now + (allowEarlyReadyCheck ? UINPUT_READY_CHECK_INITIAL_USEC : fallbackUsec);
        pending_commit_fallback_timer_ =
            engine_->instance()->eventLoop().addTimeEvent(CLOCK_MONOTONIC, timeout, UINPUT_TIMER_ACCURACY_USEC, [this, icRef = ic_->watch(), deletedChars, requireBackspaceEvents, targetCursor, allowEarlyReadyCheck, deadline](EventSourceTime* source, uint64_t) {
                if (!is_deleting_.load(std::memory_order_acquire)) {
                    debugUinputTrace("fallback_tick stop not_deleting");
                    return false;
                }
                if (pending_commit_string_.empty()) {
                    debugUinputTrace("fallback_tick cancel empty_pending");
                    cancelPendingReplacement(true, false);
                    return false;
                }

                const auto now = ::fcitx::now(CLOCK_MONOTONIC);
                if (auto* ic = icRef.get(); !ic || !ic->hasFocus()) {
                    if (now < deadline) {
                        source->setNextInterval(UINPUT_READY_CHECK_INTERVAL_USEC);
                        source->setOneShot();
                        return true;
                    }
                    if (ic && current_backspace_count_ >= deletedChars) {
                        // The old text is already gone; committing is the only way not to lose it.
                        debugUinputTrace("fallback_tick finish no_focus backspaces_observed");
                        finishPendingReplacement(true, false);
                        return false;
                    }
                    debugUinputTrace("fallback_tick cancel no_focus");
                    cancelPendingReplacement(false, false);
                    return false;
                }

                if (requireBackspaceEvents && current_backspace_count_ < deletedChars) {
                    if (now < deadline) {
                        source->setNextInterval(UINPUT_READY_CHECK_INTERVAL_USEC);
                        source->setOneShot();
                        return true;
                    }
                    debugUinputTrace("fallback_tick cancel missing_backspace_events");
                    cancelPendingReplacement(true, false);
                    return false;
                }

                bool ready = now >= deadline;
                if (!ready && allowEarlyReadyCheck) {
                    const auto& surr = ic_->surroundingText();
                    ready = surr.isValid() && surr.cursor() == surr.anchor() && surr.cursor() == targetCursor;
                }

                if (!ready) {
                    source->setNextInterval(UINPUT_READY_CHECK_INTERVAL_USEC);
                    source->setOneShot();
                    return true;
                }

                debugUinputTrace("fallback_tick finish ready deadline_or_cursor");
                LOTUS_WARN("Fallback commit pending uinput replacement after missing trigger backspace");
                finishPendingReplacement(true, false);
                return false;
            });
    }

    void LotusState::performReplacement(const std::string& deletedPart, const std::string& addedPart) {
        LOTUS_DEBUG("Perform replacement: " + deletedPart + " -> " + addedPart); //NOLINT
        current_backspace_count_ = 0;
        pending_commit_string_   = addedPart;
        const auto deletedChars  = static_cast<int>(utf8::length(deletedPart));
        int        realBackspaces = deletedChars;
        timer_driven_replacement_ = (realMode == LotusMode::Smooth || realMode == LotusMode::SuperSmooth);
        expected_backspaces_      = deletedChars;
        const auto currentLen     = realtextLen.load(std::memory_order_acquire);
        const auto targetCursor   = currentLen > static_cast<unsigned int>(deletedChars) ? currentLen - static_cast<unsigned int>(deletedChars) : 0U;
        bool mayEmptyInput        = currentLen <= static_cast<unsigned int>(deletedChars);
        pending_replacement_may_empty_input_ = mayEmptyInput;
        bool surrValid            = false;
        unsigned int surrCursor   = 0;
        unsigned int surrAnchor   = 0;
        size_t surrTextLen        = 0;
        if (realMode != LotusMode::Minecraft) {
            if (!timer_driven_replacement_) {
                ++expected_backspaces_;
            }
            if (realMode != LotusMode::SuperSmooth) {
                const auto& surrounding = ic_->surroundingText();
                surrValid               = surrounding.isValid();
                if (surrValid) {
                    surrCursor  = surrounding.cursor();
                    surrAnchor  = surrounding.anchor();
                    surrTextLen = utf8::lengthValidated(surrounding.text());
                    if (surrTextLen == utf8::INVALID_LENGTH) {
                        surrValid = false;
                    }
                }
                if (surrounding.isValid() && surrounding.cursor() == surrounding.anchor() && surrounding.cursor() <= static_cast<unsigned int>(deletedChars)) {
                    mayEmptyInput = true;
                    pending_replacement_may_empty_input_ = true;
                }
                // Enable Autofill detection for all frontends (Wayland/IBus).
                // This fixes the "toôi" duplication bug in Chromium-based search bars.
                // The isAutofillCertain function has been optimized to differentiate
                // between browser autofill and AI ghost text.
                if (isAutofillCertain(surrounding)) {
                    ++realBackspaces;
                    ++expected_backspaces_;
                }
            }
        }

        bool canReplaceWithSurrounding = timer_driven_replacement_ && realMode != LotusMode::Minecraft && surrValid && surrCursor == surrAnchor &&
                                         surrCursor >= static_cast<unsigned int>(deletedChars) && surrTextLen >= surrCursor;
        if (canReplaceWithSurrounding) {
            const auto& surrounding = ic_->surroundingText();
            auto deleteStart = utf8::nextNChar(surrounding.text().begin(), surrCursor - static_cast<unsigned int>(deletedChars));
            auto deleteEnd   = utf8::nextNChar(surrounding.text().begin(), surrCursor);
            canReplaceWithSurrounding = std::string(deleteStart, deleteEnd) == deletedPart;
        }

        if (canReplaceWithSurrounding) {
            if (shouldDebugAnonymousIbus(ic_)) {
                std::ostringstream oss;
                oss << "performReplacement surrounding deleted='" << deletedPart << "' added='" << addedPart << "' cursor=" << surrCursor << " anchor=" << surrAnchor
                    << " textLen=" << surrTextLen << " trust=" << trust_unvalidated_surrounding_delete_;
                debugAnonymousIbusTrace(oss.str());
            }
            if (isUinputDebugEnabled()) {
                std::ostringstream oss;
                oss << "surrounding_replace_in_smooth deleted='" << deletedPart << "' added='" << addedPart << "' cursor=" << surrCursor << " deletedChars=" << deletedChars;
                debugUinputTrace(oss.str());
            }
            if (!deleteSurroundingTextBeforeCursorSafely(ic_, static_cast<unsigned int>(deletedChars), &deletedPart, trust_unvalidated_surrounding_delete_)) {
                canReplaceWithSurrounding = false;
            } else {
                if (!addedPart.empty()) {
                    ic_->commitString(addedPart);
                    LOTUS_DEBUG("Commit: " + addedPart);
                }
                realtextLen.store((surrCursor - static_cast<unsigned int>(deletedChars)) + static_cast<unsigned int>(utf8::length(addedPart)), std::memory_order_release);
                debugUinputTrace("surrounding_replace_done");
                expected_backspaces_     = 0;
                current_backspace_count_ = 0;
                pending_commit_string_.clear();
                timer_driven_replacement_ = false;
                pending_replacement_may_empty_input_ = false;
                pending_reset_engine_after_commit_ = false;
                return;
            }
        }

        if (shouldDebugAnonymousIbus(ic_)) {
            std::ostringstream oss;
            oss << "performReplacement fallback_uinput deleted='" << deletedPart << "' added='" << addedPart << "' deletedChars=" << deletedChars << " currentLen=" << currentLen
                << " targetCursor=" << targetCursor << " mayEmptyInput=" << mayEmptyInput << " timer=" << timer_driven_replacement_ << " expected=" << expected_backspaces_
                << " realBackspaces=" << realBackspaces << " surrValid=" << surrValid << " surrCursor=" << surrCursor << " surrAnchor=" << surrAnchor << " surrTextLen="
                << surrTextLen << " wa=" << wa_chromium_flag << " trust=" << trust_unvalidated_surrounding_delete_;
            debugAnonymousIbusTrace(oss.str());
        }

        if (isUinputDebugEnabled()) {
            std::ostringstream oss;
            oss << "perform_replacement deleted='" << deletedPart << "' added='" << addedPart << "' deletedChars=" << deletedChars << " currentLen=" << currentLen
                << " targetCursor=" << targetCursor << " mayEmptyInput=" << mayEmptyInput << " timer=" << timer_driven_replacement_
                << " expectedBeforeTimerOverride=" << expected_backspaces_ << " realBackspaces=" << realBackspaces << " surrValid=" << surrValid
                << " surrCursor=" << surrCursor << " surrAnchor=" << surrAnchor << " surrTextLen=" << surrTextLen;
            debugUinputTrace(oss.str());
        }

        if (realMode == LotusMode::SuperSmooth) {
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }

        is_deleting_.store(true, std::memory_order_release);
        if (timer_driven_replacement_) {
            expected_backspaces_ = realBackspaces;
            schedulePendingReplacementFallback(deletedChars, false, targetCursor, false, UINPUT_EMPTY_REPLACEMENT_FALLBACK_USEC);
        } else if (mayEmptyInput && realMode != LotusMode::Minecraft) {
            schedulePendingReplacementFallback(deletedChars, true, targetCursor, false, UINPUT_EMPTY_REPLACEMENT_FALLBACK_USEC);
        }
        send_backspace_uinput(expected_backspaces_);
        debugUinputTrace("sent_backspaces expected=" + std::to_string(expected_backspaces_));
        LOTUS_DEBUG("Send " + std::to_string(expected_backspaces_) + " backspaces");
    }

    bool LotusState::checkForwardSpecialKey(KeyEvent& keyEvent, KeySym& currentSym) {
        if (keyEvent.key().isCursorMove() || currentSym == FcitxKey_Tab || currentSym == FcitxKey_KP_Tab || currentSym == FcitxKey_ISO_Left_Tab || currentSym == FcitxKey_Escape ||
            keyEvent.key().hasModifier()) {
            is_deleting_.store(false, std::memory_order_release);
            expected_backspaces_     = 0;
            current_backspace_count_ = 0;
            pending_commit_string_.clear();
            pending_commit_fallback_timer_.reset();
            pending_replay_event_.reset();
            pending_replay_scheduled_ = false;
            pending_initial_hold_timer_.reset();
            timer_driven_replacement_ = false;
            pending_replacement_may_empty_input_ = false;
            pending_reset_engine_after_commit_ = false;
            holding_initial_uinput_preedit_ = false;
            suppress_surrounding_seed_once_ = false;
            hasHistory_ = false;
            ResetEngine(lotusEngine_.handle());
            oldPreBuffer_.clear();
            return true;
        }

        if (currentSym == FcitxKey_Delete) {
            return true;
        }

        if (currentSym >= FcitxKey_KP_0 && currentSym <= FcitxKey_KP_9) {
            currentSym = static_cast<KeySym>(FcitxKey_0 + (currentSym - FcitxKey_KP_0));
            return false;
        }

        switch (currentSym) {
            case FcitxKey_KP_Add: {
                currentSym = FcitxKey_plus;
                break;
            }
            case FcitxKey_KP_Subtract: {
                currentSym = FcitxKey_minus;
                break;
            }
            case FcitxKey_KP_Divide: {
                currentSym = FcitxKey_slash;
                break;
            }
            case FcitxKey_KP_Multiply: {
                currentSym = FcitxKey_asterisk;
                break;
            }
            case FcitxKey_KP_Decimal: {
                currentSym = FcitxKey_period;
                break;
            }
            case FcitxKey_KP_Enter: {
                currentSym = FcitxKey_Return;
                break;
            }
            case FcitxKey_KP_Equal: {
                currentSym = FcitxKey_equal;
                break;
            }
            case FcitxKey_KP_Space: {
                currentSym = FcitxKey_space;
                break;
            }
            default: break;
        }
        return false;
    }

    void LotusState::handleUinputMode(KeyEvent& keyEvent, KeySym currentSym) {
        if (checkForwardSpecialKey(keyEvent, currentSym)) {
            keyEvent.forward();
            return;
        }

        if (uinput_client_fd_ < 0) {
            setup_uinput();
        }

        if (isBackspace(currentSym) || currentSym == FcitxKey_Return) {
            if (holding_initial_uinput_preedit_) {
                if (isBackspace(currentSym)) {
                    debugUinputTrace("cancel_initial_hold_by_backspace oldPre='" + oldPreBuffer_ + "'");
                    pending_initial_hold_timer_.reset();
                    holding_initial_uinput_preedit_ = false;
                    hasHistory_ = false;
                    ResetEngine(lotusEngine_.handle());
                    oldPreBuffer_.clear();
                    keyEvent.filterAndAccept();
                    return;
                }
                flushHeldInitialUinput();
            }
            if (isBackspace(currentSym)) {
                EngineProcessKeyEvent(lotusEngine_.handle(), FcitxKey_BackSpace, 0);
                UniqueCPtr<char> preeditC(EnginePullPreedit(lotusEngine_.handle()));
                oldPreBuffer_ = (preeditC && (*preeditC.get() != 0)) ? preeditC.get() : "";
                hasHistory_ = !oldPreBuffer_.empty();
                if (oldPreBuffer_.empty()) {
                    suppress_surrounding_seed_once_ = true;
                    ResetEngine(lotusEngine_.handle());
                }
                if (realtextLen.load(std::memory_order_acquire) > 0) {
                    realtextLen.fetch_sub(1, std::memory_order_acq_rel);
                }
            } else {
                hasHistory_ = false;
                ResetEngine(lotusEngine_.handle());
                oldPreBuffer_.clear();
                suppress_surrounding_seed_once_ = true;
            }
            if (currentSym == FcitxKey_Return) {
                forwardCurrentKeyDirect(keyEvent);
            } else {
                keyEvent.forward();
            }
            return;
        }

        std::string keyUtf8 = Key::keySymToUTF8(currentSym);
        if (keyUtf8.empty()) {
            flushHeldInitialUinput();
            keyEvent.forward();
            return;
        }

        if (isLiteralBraceKey(currentSym, keyEvent.rawKey().states())) {
            flushHeldInitialUinput();
            hasHistory_ = false;
            ResetEngine(lotusEngine_.handle());
            oldPreBuffer_.clear();
            keyEvent.forward();
            realtextLen.fetch_add(static_cast<unsigned int>(utf8::length(keyUtf8)), std::memory_order_acq_rel);
            return;
        }

        if (oldPreBuffer_.empty() && suppress_surrounding_seed_once_) {
            suppress_surrounding_seed_once_ = false;
        } else if (oldPreBuffer_.empty()) {
            std::string oldWord = previousWordFromSurrounding(ic_->surroundingText());
            if (!oldWord.empty()) {
                EngineRebuildFromText(lotusEngine_.handle(), oldWord.c_str());
                oldPreBuffer_ = oldWord;
                hasHistory_   = true;
                debugUinputTrace("seed_uinput_from_surrounding oldWord='" + oldWord + "'");
            }
        }

        const bool wasHoldingInitial = holding_initial_uinput_preedit_;
        if (wasHoldingInitial) {
            pending_initial_hold_timer_.reset();
            holding_initial_uinput_preedit_ = false;
            debugUinputTrace("consume_initial_hold oldPre='" + oldPreBuffer_ + "' nextKey='" + keyUtf8 + "'");
        }
        const bool canHoldInitialUinput =
            UINPUT_HOLD_INITIAL_PREEDIT && !wasHoldingInitial && (realMode == LotusMode::Smooth || realMode == LotusMode::SuperSmooth) && oldPreBuffer_.empty() &&
            canHoldInitialUinputKey(currentSym) && !wa_chromium_flag;

        if (isUinputDebugEnabled()) {
            std::ostringstream oss;
            oss << "handle_key sym=" << currentSym << " key='" << keyUtf8 << "' oldPre='" << oldPreBuffer_ << "' hasHistory=" << hasHistory_
                << " realLen=" << realtextLen.load(std::memory_order_acquire) << " canHoldInitial=" << canHoldInitialUinput << " wasHoldingInitial=" << wasHoldingInitial;
            debugUinputTrace(oss.str());
        }
        auto replaceWithSurroundingRequest = [this](const std::string& deletedPart, const std::string& addedPart) {
            auto deletedChars = static_cast<int>(utf8::length(deletedPart));
            if (deletedChars <= 0 || containsObjectReplacementChar(deletedPart) || !ic_->capabilityFlags().test(CapabilityFlag::SurroundingText)) {
                if (shouldDebugAnonymousIbus(ic_)) {
                    std::ostringstream oss;
                    oss << "direct_surrounding skip_basic deleted='" << deletedPart << "' added='" << addedPart << "' deletedChars=" << deletedChars << " capSurr="
                        << ic_->capabilityFlags().test(CapabilityFlag::SurroundingText);
                    debugAnonymousIbusTrace(oss.str());
                }
                return false;
            }

            const auto& surrounding = ic_->surroundingText();
            const auto  surrTextLen = surrounding.isValid() ? utf8::lengthValidated(surrounding.text()) : 0;
            const bool  trustUnvalidatedDeleteRequest = trust_unvalidated_surrounding_delete_;
            const bool  replaceWithForwardSelection = surrounding.isValid() && surrounding.cursor() < surrounding.anchor() && !addedPart.empty();
            auto currentLen = realtextLen.load(std::memory_order_acquire);

            if (shouldDebugAnonymousIbus(ic_)) {
                std::ostringstream oss;
                oss << "direct_surrounding apply deleted='" << deletedPart << "' added='" << addedPart << "' trust=" << trustUnvalidatedDeleteRequest
                    << " surrValid=" << surrounding.isValid() << " cursor=" << surrounding.cursor() << " anchor=" << surrounding.anchor() << " textLen=" << surrTextLen
                    << " currentLen=" << currentLen << " replaceForwardSelection=" << replaceWithForwardSelection;
                debugAnonymousIbusTrace(oss.str());
            }
            if (replaceWithForwardSelection) {
                const int backspacesToSend = deletedChars + 1;
                current_backspace_count_ = 0;
                expected_backspaces_ = backspacesToSend;
                pending_commit_string_ = addedPart;
                timer_driven_replacement_ = true;
                pending_replacement_may_empty_input_ = false;
                pending_reset_engine_after_commit_ = true;
                is_deleting_.store(true, std::memory_order_release);
                schedulePendingReplacementFallback(backspacesToSend, false, 0, false, UINPUT_OBSERVED_BACKSPACE_COMMIT_USEC);
                if (shouldDebugAnonymousIbus(ic_)) {
                    debugAnonymousIbusTrace("direct_surrounding forward_selection_uinput_backspace deletedChars=" + std::to_string(deletedChars) +
                                            " backspaces=" + std::to_string(backspacesToSend) + " added='" + addedPart + "'");
                }
                send_backspace_uinput(backspacesToSend);
                return true;
            }
            if (!deleteSurroundingTextBeforeCursorSafely(ic_, static_cast<unsigned int>(deletedChars), &deletedPart, trustUnvalidatedDeleteRequest)) {
                if (shouldDebugAnonymousIbus(ic_)) {
                    std::ostringstream oss;
                    oss << "direct_surrounding skip_unsafe_delete deleted='" << deletedPart << "' added='" << addedPart << "' surrValid=" << surrounding.isValid()
                        << " cursor=" << surrounding.cursor() << " anchor=" << surrounding.anchor() << " textLen=" << surrTextLen;
                    debugAnonymousIbusTrace(oss.str());
                }
                return false;
            }
            auto removedLen = static_cast<unsigned int>(deletedChars);
            if (currentLen >= removedLen) {
                currentLen -= removedLen;
            } else {
                currentLen = 0;
            }
            currentLen += static_cast<unsigned int>(utf8::length(addedPart));

            if (!addedPart.empty()) {
                ic_->commitString(addedPart);
            }
            realtextLen.store(currentLen, std::memory_order_release);
            return true;
        };
        bool processed = EngineProcessKeyEvent(lotusEngine_.handle(), currentSym, keyEvent.rawKey().states()) != 0U;

        auto commitF = UniqueCPtr<char>(EnginePullCommit(lotusEngine_.handle()));
        if (commitF && (*commitF.get() != 0)) {
            std::string commitStr = commitF.get();
            std::string deletedPart;
            std::string addedPart;
            compareAndSplitStrings(oldPreBuffer_, commitStr, deletedPart, addedPart);
            if (isUinputDebugEnabled()) {
                std::ostringstream oss;
                oss << "handle_commit processed=" << processed << " commit='" << commitStr << "' deleted='" << deletedPart << "' added='" << addedPart
                    << "' oldPre='" << oldPreBuffer_ << "' key='" << keyUtf8 << "'";
                debugUinputTrace(oss.str());
            }

            if (wasHoldingInitial) {
                if (!deletedPart.empty()) {
                    debugUinputTrace("direct_commit_after_initial_hold commit='" + commitStr + "'");
                    ic_->commitString(commitStr);
                    realtextLen.fetch_add(static_cast<unsigned int>(utf8::length(commitStr)), std::memory_order_acq_rel);
                    LOTUS_DEBUG("Commit: " + commitStr);
                    keyEvent.filterAndAccept();
                } else {
                    debugUinputTrace("flush_initial_then_commit_added commit='" + oldPreBuffer_ + "' added='" + addedPart + "'");
                    if (!oldPreBuffer_.empty()) {
                        ic_->commitString(oldPreBuffer_);
                        realtextLen.fetch_add(static_cast<unsigned int>(utf8::length(oldPreBuffer_)), std::memory_order_acq_rel);
                    }
                    if (!addedPart.empty()) {
                        ic_->commitString(addedPart);
                        realtextLen.fetch_add(static_cast<unsigned int>(utf8::length(addedPart)), std::memory_order_acq_rel);
                        keyEvent.filterAndAccept();
                    } else {
                        keyEvent.forward();
                        realtextLen.fetch_add(static_cast<unsigned int>(utf8::length(keyUtf8)), std::memory_order_acq_rel);
                    }
                }
                hasHistory_ = false;
                ResetEngine(lotusEngine_.handle());
                oldPreBuffer_.clear();
                suppress_surrounding_seed_once_ = true;
                return;
            }

            if (!deletedPart.empty()) {
                if (containsObjectReplacementChar(deletedPart)) {
                    ResetEngine(lotusEngine_.handle());
                    oldPreBuffer_.clear();
                    hasHistory_ = false;
                    if (wa_chromium_flag) {
                        ic_->commitString(keyUtf8);
                        keyEvent.filterAndAccept();
                    } else {
                        keyEvent.forward();
                    }
                    realtextLen.fetch_add(static_cast<unsigned int>(utf8::length(keyUtf8)), std::memory_order_acq_rel);
                    return;
                }
                if (wa_chromium_flag && replaceWithSurroundingRequest(deletedPart, addedPart)) {
                    keyEvent.filterAndAccept();
                } else {
                    performReplacement(deletedPart, addedPart);
                    keyEvent.filterAndAccept();
                }
            } else {
                bool wasAutoCapitalized = (currentSym != keyEvent.rawKey().sym());
                if (!addedPart.empty() && (keyUtf8 != addedPart || wasAutoCapitalized)) {
                    // Prevent auto-capitalized character replacement from stripping out Vietnamese chars
                    if (addedPart.size() > 1 && addedPart.back() == ' ') {
                        // Stripping the trigger key (space) from addedPart
#if __cplusplus >= 202002L
                        addedPart.resize(addedPart.size() - 1);
#else
                        addedPart = addedPart.substr(0, addedPart.size() - 1);
#endif
                    }
                    ic_->commitString(addedPart);
                    realtextLen.fetch_add(static_cast<unsigned int>(utf8::length(addedPart)), std::memory_order_acq_rel);
                    LOTUS_DEBUG("Commit: " + addedPart);
                    keyEvent.filterAndAccept();
                } else {
                    keyEvent.forward();
                    realtextLen.fetch_add(static_cast<unsigned int>(utf8::length(keyUtf8)), std::memory_order_acq_rel);
                }
            }

            hasHistory_ = false;
            ResetEngine(lotusEngine_.handle());
            oldPreBuffer_.clear();
            // The word just ended here; don't rebuild it from not-yet-updated surrounding text.
            suppress_surrounding_seed_once_ = true;

            return;
        }

        if (!processed) {
            UniqueCPtr<char> preeditC(EnginePullPreedit(lotusEngine_.handle()));
            if (!preeditC || (*preeditC.get() == 0)) {
                hasHistory_ = false;
                ResetEngine(lotusEngine_.handle());
                oldPreBuffer_.clear();
                suppress_surrounding_seed_once_ = true;
                keyEvent.forward();
                realtextLen.fetch_add(static_cast<unsigned int>(utf8::length(keyUtf8)), std::memory_order_acq_rel);
            }
            return;
        }

        hasHistory_ = true;

        UniqueCPtr<char> preeditC(EnginePullPreedit(lotusEngine_.handle()));
        std::string      preeditStr = (preeditC && (*preeditC.get() != 0)) ? preeditC.get() : "";

        std::string      deletedPart;
        std::string      addedPart;

        if (wa_chromium_flag)
            keyEvent.filterAndAccept();

        if (compareAndSplitStrings(oldPreBuffer_, preeditStr, deletedPart, addedPart) != 0) {
            if (isUinputDebugEnabled()) {
                std::ostringstream oss;
                oss << "handle_preedit processed=" << processed << " preedit='" << preeditStr << "' deleted='" << deletedPart << "' added='" << addedPart
                    << "' oldPre='" << oldPreBuffer_ << "' key='" << keyUtf8 << "'";
                debugUinputTrace(oss.str());
            }
            if (wasHoldingInitial) {
                if (!deletedPart.empty()) {
                    debugUinputTrace("direct_preedit_after_initial_hold preedit='" + preeditStr + "'");
                    ic_->commitString(preeditStr);
                    realtextLen.fetch_add(static_cast<unsigned int>(utf8::length(preeditStr)), std::memory_order_acq_rel);
                    LOTUS_DEBUG("Commit: " + preeditStr);
                    keyEvent.filterAndAccept();
                    oldPreBuffer_ = preeditStr;
                    hasHistory_ = true;
                } else {
                    debugUinputTrace("flush_initial_then_forward_added commit='" + oldPreBuffer_ + "' added='" + addedPart + "'");
                    if (!oldPreBuffer_.empty()) {
                        ic_->commitString(oldPreBuffer_);
                        realtextLen.fetch_add(static_cast<unsigned int>(utf8::length(oldPreBuffer_)), std::memory_order_acq_rel);
                    }
                    if (!addedPart.empty() && addedPart != keyUtf8) {
                        ic_->commitString(addedPart);
                        realtextLen.fetch_add(static_cast<unsigned int>(utf8::length(addedPart)), std::memory_order_acq_rel);
                        keyEvent.filterAndAccept();
                    } else {
                        keyEvent.forward();
                        realtextLen.fetch_add(static_cast<unsigned int>(utf8::length(keyUtf8)), std::memory_order_acq_rel);
                    }
                    oldPreBuffer_ = preeditStr;
                    hasHistory_ = true;
                }
                return;
            }
            if (deletedPart.empty()) {
                bool isCommit           = false;
                bool wasAutoCapitalized = (currentSym != keyEvent.rawKey().sym());
                if (!addedPart.empty()) {
                    if (canHoldInitialUinput && addedPart == keyUtf8 && !wa_chromium_flag && !wasAutoCapitalized) {
                        oldPreBuffer_ = preeditStr;
                        holding_initial_uinput_preedit_ = true;
                        pending_initial_hold_timer_.reset();
                        pending_initial_hold_timer_ = engine_->instance()->eventLoop().addTimeEvent(
                            CLOCK_MONOTONIC, ::fcitx::now(CLOCK_MONOTONIC) + UINPUT_INITIAL_HOLD_USEC, UINPUT_TIMER_ACCURACY_USEC, [this, icRef = ic_->watch()](EventSourceTime*, uint64_t) {
                                if (auto* ic = icRef.get(); ic && ic->hasFocus()) {
                                    flushHeldInitialUinput(false);
                                } else {
                                    holding_initial_uinput_preedit_ = false;
                                    hasHistory_ = false;
                                    ResetEngine(lotusEngine_.handle());
                                    oldPreBuffer_.clear();
                                }
                                return false;
                            });
                        debugUinputTrace("hold_initial_preedit oldPre='" + oldPreBuffer_ + "' usec=" + std::to_string(UINPUT_INITIAL_HOLD_USEC));
                        keyEvent.filterAndAccept();
                        return;
                    }
                    oldPreBuffer_ = preeditStr;
                    if (wa_chromium_flag || wasAutoCapitalized || addedPart != keyUtf8) {
                        ic_->commitString(addedPart);
                        realtextLen.fetch_add(static_cast<unsigned int>(utf8::length(addedPart)), std::memory_order_acq_rel);
                        LOTUS_DEBUG("Commit: " + addedPart);
                        if (!wa_chromium_flag) {
                            keyEvent.filterAndAccept();
                            isCommit = true;
                        }
                    }
                }
                if (!wa_chromium_flag && !isCommit) {
                    keyEvent.forward();
                    realtextLen.fetch_add(static_cast<unsigned int>(utf8::length(keyUtf8)), std::memory_order_acq_rel);
                }
            } else {
                if (containsObjectReplacementChar(deletedPart)) {
                    ResetEngine(lotusEngine_.handle());
                    oldPreBuffer_.clear();
                    hasHistory_ = false;
                    if (wa_chromium_flag) {
                        ic_->commitString(keyUtf8);
                        keyEvent.filterAndAccept();
                    } else {
                        keyEvent.forward();
                    }
                    realtextLen.fetch_add(static_cast<unsigned int>(utf8::length(keyUtf8)), std::memory_order_acq_rel);
                    return;
                }
                if (wa_chromium_flag && replaceWithSurroundingRequest(deletedPart, addedPart)) {
                    oldPreBuffer_ = preeditStr;
                    hasHistory_   = true;
                    return;
                }

                if (uinput_client_fd_ < 0) {
                    LOTUS_ERROR("Cannot connect to uinput server, commit rawkey");
                    std::string rawKey = keyEvent.key().toString();
                    if (!rawKey.empty()) {
                        ic_->commitString(rawKey);
                        realtextLen.fetch_add(static_cast<unsigned int>(utf8::length(rawKey)), std::memory_order_acq_rel);
                    }
                    return;
                }

                if (is_deleting_.load()) {
                    is_deleting_.store(false, std::memory_order_release);
                }

                if (!wa_chromium_flag)
                    keyEvent.filterAndAccept();
                performReplacement(deletedPart, addedPart);
                oldPreBuffer_ = preeditStr;
            }
        }
    }

    void LotusState::handleSurroundingText(KeyEvent& keyEvent, KeySym currentSym) {
        if (checkForwardSpecialKey(keyEvent, currentSym)) {
            keyEvent.forward();
            return;
        }
        auto* ic = keyEvent.inputContext();
        if ((ic == nullptr) || !ic->capabilityFlags().test(CapabilityFlag::SurroundingText)) {
            LOTUS_WARN("Surrounding text not supported");
            keyEvent.forward();
            return;
        }

        const auto& surrounding = ic->surroundingText();
        if (!surrounding.isValid()) {
            LOTUS_WARN("Surrounding text is invalid");
            keyEvent.forward();
            return;
        }

        if (isBackspace(keyEvent.rawKey().sym())) {
            ResetEngine(lotusEngine_.handle());
            oldPreBuffer_.clear();
            hasHistory_ = false;
            suppress_surrounding_seed_once_ = true;
            keyEvent.forward();
            return;
        }

        if (isLiteralBraceKey(currentSym, keyEvent.rawKey().states())) {
            ResetEngine(lotusEngine_.handle());
            keyEvent.forward();
            return;
        }

        const std::string& text   = surrounding.text();
        unsigned int       cursor = std::min(surrounding.anchor(), surrounding.cursor());

        size_t             textLen = utf8::lengthValidated(text);

        if (textLen == utf8::INVALID_LENGTH || cursor <= 0 || cursor > textLen) {
            processNormalKey(keyEvent, currentSym);
            return;
        }

        {
            std::string oldWord = surroundingWordBeforeCursor(text, cursor);

            if (oldWord.empty()) {
                processNormalKey(keyEvent, currentSym);
                return;
            }

            EngineRebuildFromText(lotusEngine_.handle(), oldWord.c_str());

            bool processed = EngineProcessKeyEvent(lotusEngine_.handle(), currentSym, keyEvent.rawKey().states()) != 0U;

            if (!processed) {
                keyEvent.forward();
                ResetEngine(lotusEngine_.handle());
                return;
            }

            auto        commitPtr  = UniqueCPtr<char>(EnginePullCommit(lotusEngine_.handle()));
            auto        preeditPtr = UniqueCPtr<char>(EnginePullPreedit(lotusEngine_.handle()));

            std::string newWord;
            if (commitPtr && (*commitPtr.get() != 0))
                newWord += commitPtr.get();
            if (preeditPtr && (*preeditPtr.get() != 0))
                newWord += preeditPtr.get();

            std::string deletedPart;
            std::string addedPart;
            compareAndSplitStrings(oldWord, newWord, deletedPart, addedPart);
            if (containsObjectReplacementChar(deletedPart)) {
                ResetEngine(lotusEngine_.handle());
                keyEvent.forward();
                return;
            }
            if ((deletedPart.empty() || deletedPart == oldWord) && addedPart == keyEvent.key().toString()) {
                ResetEngine(lotusEngine_.handle());
                keyEvent.forward();
                return;
            }

            if (!deletedPart.empty() || !addedPart.empty()) {
                size_t charsToDelete = utf8::length(deletedPart);

                if (charsToDelete > 0 && !deleteSurroundingTextBeforeCursorSafely(ic, static_cast<unsigned int>(charsToDelete), &deletedPart)) {
                    ResetEngine(lotusEngine_.handle());
                    keyEvent.forward();
                    return;
                }

                if (!addedPart.empty()) {
                    ic->commitString(addedPart);
                    LOTUS_DEBUG("Commit: " + addedPart);
                }

                ResetEngine(lotusEngine_.handle());
                keyEvent.filterAndAccept();
                return;
            }

            ResetEngine(lotusEngine_.handle());
            keyEvent.filterAndAccept();
            return;
        }
    }

    void LotusState::processNormalKey(KeyEvent& keyEvent, KeySym currentSym) {
        auto* ic = keyEvent.inputContext();
        ResetEngine(lotusEngine_.handle());
        bool processed = EngineProcessKeyEvent(lotusEngine_.handle(), currentSym, keyEvent.rawKey().states()) != 0U;
        if (processed) {
            auto        commitPtr  = UniqueCPtr<char>(EnginePullCommit(lotusEngine_.handle()));
            auto        preeditPtr = UniqueCPtr<char>(EnginePullPreedit(lotusEngine_.handle()));
            std::string out;
            if (commitPtr && (*commitPtr.get() != 0))
                out += commitPtr.get();
            if (preeditPtr && (*preeditPtr.get() != 0))
                out += preeditPtr.get();

            if (!out.empty()) {
                LOTUS_DEBUG("Commit: " + out);
                ic->commitString(out);
            }

            ResetEngine(lotusEngine_.handle());
            keyEvent.filterAndAccept();
        } else {
            keyEvent.forward();
        }
    }

    void LotusState::handleDoubleSpaceReplacement() {
        switch (realMode) {
            case LotusMode::SurroundingText: {
                const std::string deletedPart = " ";
                if (deleteSurroundingTextBeforeCursorSafely(ic_, 1, &deletedPart)) {
                    ic_->commitString(". ");
                    LOTUS_DEBUG("Commit: . ");
                } else {
                    ic_->commitString(" ");
                }

                break;
            }
            default: { // Uinput, Smooth, Preedit, etc.
                performReplacement(" ", ". ");
                LOTUS_DEBUG("Commit: . ");
                break;
            }
        }
        if (*engine_->config().autoCapitalizeAfterPunctuation) {
            isPrevPunctuation_ = true;
            shouldCapitalize_  = true;
        }
    }

    void LotusState::handleDoubleHyphenReplacement() {
        // Em-dash (U+2014)
        std::string emDash = "—";
        switch (realMode) {
            case LotusMode::SurroundingText: {
                const std::string deletedPart = "-";
                if (deleteSurroundingTextBeforeCursorSafely(ic_, 1, &deletedPart)) {
                    ic_->commitString(emDash);
                    LOTUS_DEBUG("Commit: — (em-dash)");
                } else {
                    ic_->commitString("-");
                }
                break;
            }
            default: { // Uinput, Smooth, Preedit, etc.
                performReplacement("-", emDash);
                LOTUS_DEBUG("Commit: — (em-dash)");
                break;
            }
        }
    }

    void LotusState::handleOffModeMacro(KeyEvent& keyEvent, KeySym currentSym) {
        if (checkForwardSpecialKey(keyEvent, currentSym)) {
            keyEvent.forward();
            return;
        }

        if (uinput_client_fd_ < 0) {
            connect_uinput_server();
        }

        if (isBackspace(currentSym)) {
            EngineProcessKeyEvent(lotusEngine_.handle(), FcitxKey_BackSpace, 0);
            auto preeditC = UniqueCPtr<char>(EnginePullPreedit(lotusEngine_.handle()));
            oldPreBuffer_ = (preeditC && (*preeditC.get() != 0)) ? preeditC.get() : "";
            keyEvent.forward();
            return;
        }

        if (currentSym == FcitxKey_Return) {
            if (!oldPreBuffer_.empty()) {
                ResetEngine(lotusEngine_.handle());
                oldPreBuffer_.clear();
            }
            keyEvent.forward();
            return;
        }

        std::string keyUtf8 = Key::keySymToUTF8(currentSym);

        bool        processed = EngineProcessKeyEvent(lotusEngine_.handle(), currentSym, keyEvent.rawKey().states()) != 0U;

        auto        commitPtr = UniqueCPtr<char>(EnginePullCommit(lotusEngine_.handle()));
        if (processed && commitPtr && (*commitPtr.get() != 0)) {
            std::string commitStr = commitPtr.get();

            // Determine if this is a macro expansion or just confirmed typed text
            bool isMacroExpansion = false;
            if (keyUtf8.empty()) {
                isMacroExpansion = (commitStr != oldPreBuffer_);
            } else {
                isMacroExpansion = (commitStr != oldPreBuffer_ + keyUtf8);
            }

            if (isMacroExpansion) {
                LOTUS_DEBUG("Macro expansion: '" + oldPreBuffer_ + "' -> '" + commitStr + "'");
                // Try uinput replacement first, fallback to deleteSurroundingText, then plain commit
                if (uinput_client_fd_ >= 0 && !oldPreBuffer_.empty()) {
                    performReplacement(oldPreBuffer_, commitStr);
                } else if (ic_->capabilityFlags().test(CapabilityFlag::SurroundingText)) {
                    const auto& surrounding = ic_->surroundingText();
                    if (surrounding.isValid()) {
                        size_t oldLen = utf8::length(oldPreBuffer_);
                        if (oldLen > 0 && !deleteSurroundingTextBeforeCursorSafely(ic_, static_cast<unsigned int>(oldLen), &oldPreBuffer_)) {
                            ResetEngine(lotusEngine_.handle());
                            oldPreBuffer_.clear();
                            hasHistory_ = false;
                            keyEvent.forward();
                            return;
                        }
                        ic_->commitString(commitStr);
                    } else {
                        ic_->commitString(commitStr);
                    }
                } else {
                    ic_->commitString(commitStr);
                }
                keyEvent.filterAndAccept();
            } else {
                // No macro: typed text confirmed by engine, just forward trigger key
                keyEvent.forward();
            }

            oldPreBuffer_.clear();
            hasHistory_ = false;
            return;
        }

        // 8. No commit or engine rejected the key
        if (processed || (commitPtr && (*commitPtr.get() != 0))) {
            // Engine processed the key (building shadow state)
            // OR engine rejected the key but committed old text (non-processable key)
            auto preeditPtr = UniqueCPtr<char>(EnginePullPreedit(lotusEngine_.handle()));
            oldPreBuffer_   = (preeditPtr && (*preeditPtr.get() != 0)) ? preeditPtr.get() : "";
            if (!processed) {
                // Engine committed old text but didn't process the new key → forward the key
                oldPreBuffer_.clear();
                hasHistory_ = false;
            }
            keyEvent.forward();
        } else {
            // Engine didn't handle this key
            if (!oldPreBuffer_.empty()) {
                ResetEngine(lotusEngine_.handle());
                oldPreBuffer_.clear();
            }
            keyEvent.forward();
        }
    }

    bool LotusState::needsSerializedOutput() const {
        return isAnonymousIbusContext(ic_) &&
               (realMode == LotusMode::Uinput || realMode == LotusMode::Smooth || realMode == LotusMode::Minecraft || realMode == LotusMode::SuperSmooth);
    }

    void LotusState::startDirectAwait() {
        direct_awaiting_ = true;
        const auto& surr          = ic_->surroundingText();
        const bool  snapValid     = surr.isValid();
        std::string snapText      = surr.text();
        const auto  snapCursor    = surr.cursor();
        const auto  snapAnchor    = surr.anchor();
        const auto  now           = ::fcitx::now(CLOCK_MONOTONIC);
        const auto  deadline      = now + UINPUT_DIRECT_CONFIRM_TIMEOUT_USEC;
        direct_await_deadline_    = deadline;
        debugUinputTrace("direct_await_start");
        direct_await_timer_ = engine_->instance()->eventLoop().addTimeEvent(
            CLOCK_MONOTONIC, now + UINPUT_DIRECT_CONFIRM_POLL_USEC, UINPUT_TIMER_ACCURACY_USEC,
            [this, icRef = ic_->watch(), snapValid, snapText = std::move(snapText), snapCursor, snapAnchor, deadline](EventSourceTime* source, uint64_t) {
                if (!icRef.isValid()) {
                    return false;
                }
                const auto& surr    = ic_->surroundingText();
                const bool  changed = surr.isValid() != snapValid || surr.cursor() != snapCursor || surr.anchor() != snapAnchor || surr.text() != snapText;
                if (!changed && ::fcitx::now(CLOCK_MONOTONIC) < deadline) {
                    source->setNextInterval(UINPUT_DIRECT_CONFIRM_POLL_USEC);
                    source->setOneShot();
                    return true;
                }
                debugUinputTrace(std::string("direct_await_done changed=") + (changed ? "1" : "0"));
                direct_awaiting_ = false;
                scheduleDirectDrain();
                return false;
            });
    }

    void LotusState::scheduleDirectDrain() {
        if (direct_draining_ || direct_keys_.empty()) {
            return;
        }
        direct_drain_event_ = engine_->instance()->eventLoop().addPostEvent([this, icRef = ic_->watch()](EventSource*) {
            if (auto* ic = icRef.get(); ic && ic->hasFocus()) {
                drainDirectKeys();
            } else {
                direct_keys_.clear();
            }
            return false;
        });
    }

    void LotusState::drainDirectKeys() {
        direct_draining_ = true;
        while (!direct_keys_.empty() && !direct_awaiting_ && !is_deleting_.load(std::memory_order_acquire)) {
            const auto entry = direct_keys_.front();
            direct_keys_.erase(direct_keys_.begin());
            KeyEvent event(ic_, Key(static_cast<KeySym>(entry.sym), KeyStates(entry.state), entry.code));
            if (isUinputDebugEnabled()) {
                debugUinputTrace("direct_drain_key sym=" + std::to_string(entry.sym) + " left=" + std::to_string(direct_keys_.size()));
            }
            keyEvent(event);
            if (!event.filtered()) {
                const auto text = keySymToBufferedUtf8(event.rawKey().sym());
                if (!text.empty() && !event.rawKey().states().testAny(KeyStates{KeyState::Ctrl, KeyState::Alt, KeyState::Super})) {
                    // Plain text keys (space, digits...) are committed, not re-injected: that keeps
                    // them ordered with our other commits and does not depend on forwardKey.
                    ic_->commitString(text);
                    realtextLen.fetch_add(static_cast<unsigned int>(utf8::length(text)), std::memory_order_acq_rel);
                    startDirectAwait();
                } else {
                    ic_->forwardKey(event.rawKey());
                    ic_->forwardKey(event.rawKey(), true);
                }
            }
        }
        direct_draining_ = false;
    }

    void LotusState::keyEvent(KeyEvent& keyEvent) {
        if (!lotusEngine_ || keyEvent.isRelease() || keyEvent.rawKey().isModifier())
            return;
        if (!needsSerializedOutput()) {
            processKeyEvent(keyEvent);
            return;
        }

        // gnome-shell/mutter defers text-input "done" to idle and GTK keeps only the last commit_string
        // before it, so a commit sent before the previous one is applied replaces it ("án" -> "n").
        // Hold keys until the client has applied our last output, then feed them through in order.
        const KeySym sym                 = keyEvent.rawKey().sym();
        const bool   generatedBackspace  = is_deleting_.load(std::memory_order_acquire) && isBackspace(sym);
        if (direct_awaiting_ && !direct_draining_ && ::fcitx::now(CLOCK_MONOTONIC) >= direct_await_deadline_ + UINPUT_DIRECT_CONFIRM_TIMEOUT_USEC) {
            // Safety net: never keep keys hostage if the confirmation timer did not run.
            debugUinputTrace("direct_await_expired");
            direct_awaiting_ = false;
            direct_await_timer_.reset();
            drainDirectKeys();
        }
        if (!direct_draining_ && !generatedBackspace && (direct_awaiting_ || !direct_keys_.empty())) {
            if (direct_keys_.size() < MAX_BUFFERED_KEYS) {
                debugUinputTrace("direct_hold_key sym=" + std::to_string(sym));
                direct_keys_.push_back({.sym = sym, .state = keyEvent.rawKey().states(), .code = keyEvent.rawKey().code()});
            }
            keyEvent.filterAndAccept();
            return;
        }

        processKeyEvent(keyEvent);
        if (keyEvent.filtered() && !generatedBackspace && !is_deleting_.load(std::memory_order_acquire) && !pending_replay_scheduled_) {
            startDirectAwait();
        }
    }

    void LotusState::processKeyEvent(KeyEvent& keyEvent) {
        if (uinput_client_fd_ < 0) {
            LOTUS_WARN("Cannot connect to uinput server, reconnecting....");
            connect_uinput_server();
        }
        if (!timer_driven_replacement_ && expected_backspaces_ > 0 && current_backspace_count_ >= expected_backspaces_ && is_deleting_.load()) {
            is_deleting_.store(false);
            current_backspace_count_ = 0;
            expected_backspaces_     = 0;
            pending_commit_fallback_timer_.reset();
            pending_replay_event_.reset();
            pending_replay_scheduled_ = false;
            pending_initial_hold_timer_.reset();
            timer_driven_replacement_ = false;
            pending_replacement_may_empty_input_ = false;
            pending_reset_engine_after_commit_ = false;
            holding_initial_uinput_preedit_ = false;
            if (!buffered_keys_.empty()) {
                scheduleReplayBufferedKeys();
            }
        }
        const bool replacementInFlight = is_deleting_.load(std::memory_order_acquire) || pending_replay_scheduled_ || direct_awaiting_ || !direct_keys_.empty();
        if ((needEngineReset.load() || g_mouse_clicked.load(std::memory_order_acquire)) && !replacementInFlight) {
            // The held letter was typed before the click; commit it instead of dropping it.
            flushHeldInitialUinput();
        }
        if (needEngineReset.load() && realMode != LotusMode::Off && !replacementInFlight) {
            LOTUS_DEBUG("Need engine reset");
            oldPreBuffer_.clear();
            hasHistory_ = false;
            pending_initial_hold_timer_.reset();
            pending_reset_engine_after_commit_ = false;
            holding_initial_uinput_preedit_ = false;
            ResetEngine(lotusEngine_.handle());
            is_deleting_.store(false);
            current_backspace_count_ = 0;
            isPrevSpace_             = false;
            shouldCapitalize_        = false;
            isPrevPunctuation_       = false;
            needEngineReset.store(false);
        }

        if (g_mouse_clicked.load(std::memory_order_acquire) && !replacementInFlight) {
            g_mouse_clicked.store(false, std::memory_order_release);
            clearAllBuffers();
        }
        KeySym currentSym = keyEvent.rawKey().sym();
        if (isUinputDebugEnabled()) {
            const auto&        surr = ic_->surroundingText();
            std::ostringstream oss;
            oss << "key_event ic=" << ic_ << " prog='" << ic_->program() << "' fe=" << getFrontendName(ic_) << " sym=" << currentSym << " key='"
                << keySymToBufferedUtf8(currentSym) << "' deleting=" << is_deleting_.load() << " bs=" << current_backspace_count_ << "/" << expected_backspaces_
                << " pending='" << pending_commit_string_ << "' replayPending=" << pending_replay_scheduled_ << " buffered=" << buffered_keys_.size() << " oldPre='"
                << oldPreBuffer_ << "' hold=" << holding_initial_uinput_preedit_ << " realLen=" << realtextLen.load() << " wa=" << wa_chromium_flag
                << " surrValid=" << surr.isValid() << " cursor=" << surr.cursor() << " anchor=" << surr.anchor() << " surrText='" << surr.text() << "'";
            debugUinputTrace(oss.str());
        }
        const bool anonymousIbusDirectCommit = isAnonymousIbusContext(ic_) &&
                                               (realMode == LotusMode::Uinput || realMode == LotusMode::Smooth || realMode == LotusMode::Minecraft ||
                                                realMode == LotusMode::SuperSmooth);
        if (anonymousIbusDirectCommit) {
            wa_chromium_flag = true;
            trust_unvalidated_surrounding_delete_ = ic_->capabilityFlags().test(CapabilityFlag::SurroundingText) && !ic_->surroundingText().isValid();
            if (shouldDebugAnonymousIbus(ic_)) {
                std::ostringstream oss;
                oss << "keyEvent anonymous sym=" << currentSym << " key='" << keySymToBufferedUtf8(currentSym) << "' trust="
                    << trust_unvalidated_surrounding_delete_ << " surrValid=" << ic_->surroundingText().isValid() << " cursor=" << ic_->surroundingText().cursor()
                    << " anchor=" << ic_->surroundingText().anchor() << " realLen=" << realtextLen.load(std::memory_order_acquire) << " oldPre='" << oldPreBuffer_ << "'";
                debugAnonymousIbusTrace(oss.str());
            }
            waitAck_         = false;
        }
        if (isEnterKey(currentSym) && (is_deleting_.load(std::memory_order_acquire) || pending_replay_scheduled_)) {
            pending_commit_fallback_timer_.reset();
            pending_replay_event_.reset();
            pending_replay_scheduled_ = false;
            buffered_keys_.clear();

            if (!pending_commit_string_.empty() && !surroundingTextBeforeCursorEndsWith(ic_->surroundingText(), pending_commit_string_)) {
                ic_->commitString(pending_commit_string_);
                realtextLen.fetch_add(static_cast<unsigned int>(utf8::length(pending_commit_string_)), std::memory_order_acq_rel);
            }

            expected_backspaces_ = 0;
            current_backspace_count_ = 0;
            pending_commit_string_.clear();
            timer_driven_replacement_ = false;
            pending_replacement_may_empty_input_ = false;
            pending_initial_hold_timer_.reset();
            holding_initial_uinput_preedit_ = false;
            pending_reset_engine_after_commit_ = false;
            is_deleting_.store(false, std::memory_order_release);
            hasHistory_ = false;
            ResetEngine(lotusEngine_.handle());
            oldPreBuffer_.clear();
            forwardCurrentKeyDirect(keyEvent);
            return;
        }
        if (pending_replay_scheduled_) {
            if (shouldBufferPendingUinputKey(currentSym) && buffered_keys_.size() < MAX_BUFFERED_KEYS) {
                debugUinputTrace("buffer_key_while_replay_pending sym=" + std::to_string(currentSym) + " key='" + keySymToBufferedUtf8(currentSym) + "'");
                buffered_keys_.push_back({.sym = currentSym, .state = keyEvent.rawKey().states(), .code = keyEvent.rawKey().code()});
                keyEvent.filterAndAccept();
                return;
            }
        }
        if (*engine_->config().autoCapitalizeAfterPunctuation && realMode != LotusMode::Off) {
            // Ignore auto-capitalize side-effects if we're processing automated replacement backspaces
            bool isAutomatedBackspace = is_deleting_.load(std::memory_order_acquire) && isBackspace(currentSym);

            if (!isAutomatedBackspace) {
                if (shouldCapitalize_) {
                    if (currentSym >= FcitxKey_a && currentSym <= FcitxKey_z) {
                        auto upperSym = static_cast<KeySym>(currentSym - (FcitxKey_a - FcitxKey_A));
                        currentSym    = upperSym;
                        keyEvent.setKey(Key(upperSym, keyEvent.rawKey().states()));
                        shouldCapitalize_ = false;
                    } else if (currentSym != FcitxKey_space) {
                        shouldCapitalize_ = false;
                    }
                }

                switch (currentSym) {
                    case FcitxKey_period:
                    case FcitxKey_exclam:
                    case FcitxKey_question: isPrevPunctuation_ = true; break;
                    case FcitxKey_Return:
                    case FcitxKey_KP_Enter:
                        shouldCapitalize_  = true;
                        isPrevPunctuation_ = false;
                        break;
                    case FcitxKey_space:
                        if (isPrevPunctuation_) {
                            shouldCapitalize_  = true;
                            isPrevPunctuation_ = false;
                        }
                        break;
                    default:
                        if (currentSym != FcitxKey_space) {
                            isPrevPunctuation_ = false;
                        }
                        break;
                }
            }
        }

        if (is_deleting_.load(std::memory_order_acquire)) {
            if (isBackspace(currentSym)) {
                if (handleUInputKeyPress(keyEvent, currentSym, (realMode == LotusMode::Smooth || realMode == LotusMode::SuperSmooth) ? 5 : 20)) {
                    return;
                }
            } else {
                if (shouldBufferPendingUinputKey(currentSym) && buffered_keys_.size() < MAX_BUFFERED_KEYS) {
                    LOTUS_DEBUG("Typing so fast, add key to queue");
                    buffered_keys_.push_back({.sym = currentSym, .state = keyEvent.rawKey().states(), .code = keyEvent.rawKey().code()});
                }
                keyEvent.filterAndAccept();
            }
            return;
        }

        if (*engine_->config().doubleSpaceToPeriod && realMode != LotusMode::Off) {
            bool isSpaceKey = (currentSym == FcitxKey_space || currentSym == FcitxKey_KP_Space);
            if (isSpaceKey && !keyEvent.key().hasModifier()) {
                if (isPrevSpace_) {
                    keyEvent.filterAndAccept();
                    handleDoubleSpaceReplacement();
                    isPrevSpace_ = false;
                    return;
                }
                isPrevSpace_ = true;
            } else {
                isPrevSpace_ = false;
            }
        }

        if (*engine_->config().doubleHyphenToEmDash && realMode != LotusMode::Off) {
            bool isHyphenKey = (currentSym == FcitxKey_minus || currentSym == FcitxKey_KP_Subtract);
            if (isHyphenKey && !keyEvent.key().hasModifier()) {
                if (isPrevHyphen_) {
                    keyEvent.filterAndAccept();
                    handleDoubleHyphenReplacement();
                    isPrevHyphen_ = false;
                    return;
                }
                isPrevHyphen_ = true;
            } else {
                isPrevHyphen_ = false;
            }
        }

        switch (realMode) {
            case LotusMode::Uinput:
            case LotusMode::Smooth:
            case LotusMode::Minecraft:
            case LotusMode::SuperSmooth: {
                handleUinputMode(keyEvent, currentSym);
                break;
            }
            case LotusMode::SurroundingText: {
                handleSurroundingText(keyEvent, currentSym);
                break;
            }
            case LotusMode::Preedit: {
                if (!*engine_->config().inlinePreedit && ic_->capabilityFlags().test(CapabilityFlag::SurroundingText)) {
                    handleSurroundingText(keyEvent, currentSym);
                } else {
                    handlePreeditMode(keyEvent, currentSym);
                }
                break;
            }
            case LotusMode::Emoji: {
                handleEmojiMode(keyEvent);
                break;
            }
            default: {
                if (*engine_->config().enableMacroInOffMode && *engine_->config().enableMacro) {
                    handleOffModeMacro(keyEvent, currentSym);
                }
                break;
            }
        }
    }

    void LotusState::reset(bool isFocusOut) {
        const auto& surrounding = ic_->surroundingText();
        const auto& text        = surrounding.text();
        size_t      textLen     = utf8::length(text);
        realtextLen.store(textLen, std::memory_order_release);
        if (is_deleting_.load(std::memory_order_acquire) || pending_replay_scheduled_ || direct_awaiting_ || !direct_keys_.empty()) {
            return;
        }

        const auto mode = realMode.load(std::memory_order_acquire);
        const bool preserveUinputPreedit =
            !isFocusOut && !oldPreBuffer_.empty() && (mode == LotusMode::Uinput || mode == LotusMode::Smooth || mode == LotusMode::SuperSmooth || mode == LotusMode::Minecraft);
        if (isUinputDebugEnabled()) {
            std::ostringstream oss;
            oss << "reset isFocusOut=" << isFocusOut << " preserveUinputPreedit=" << preserveUinputPreedit << " oldPre='" << oldPreBuffer_ << "' hasHistory=" << hasHistory_
                << " holdInitial=" << holding_initial_uinput_preedit_ << " surrValid=" << surrounding.isValid() << " surrTextLen=" << textLen
                << " realLen=" << realtextLen.load(std::memory_order_acquire);
            debugUinputTrace(oss.str());
        }

        if (isFocusOut && holding_initial_uinput_preedit_) {
            flushHeldInitialUinput();
        }

        if (lotusEngine_) {
            isPrevSpace_       = false;
            isPrevHyphen_      = false;
            shouldCapitalize_  = false;
            isPrevPunctuation_ = false;
            if (realMode == LotusMode::Preedit && isFocusOut) {
                EngineCommitPreedit(lotusEngine_.handle());
                UniqueCPtr<char> commit(EnginePullCommit(lotusEngine_.handle()));
                if (commit && (*commit.get() != 0)) {
                    ic_->commitString(commit.get());
                    LOTUS_DEBUG("Commit: " + std::string(commit.get()));
                }
            }
            if (!preserveUinputPreedit) {
                ResetEngine(lotusEngine_.handle());
                oldPreBuffer_.clear();
                hasHistory_ = false;
            }
        }
        if (!preserveUinputPreedit && getFrontendName(ic_) != "dbus") {
            const bool suppressSeed = !isFocusOut && suppress_surrounding_seed_once_;
            clearAllBuffers();
            suppress_surrounding_seed_once_ = suppressSeed;
        }

        switch (realMode) {
            case LotusMode::Preedit: {
                ic_->inputPanel().reset();
                ic_->updateUserInterface(UserInterfaceComponent::InputPanel);
                ic_->updatePreedit();
                break;
            }
            case LotusMode::SurroundingText:
            case LotusMode::Uinput:
            case LotusMode::Smooth:
            case LotusMode::Minecraft:
            case LotusMode::SuperSmooth: {
                ic_->inputPanel().reset();
                break;
            }
            case LotusMode::Emoji: {
                ic_->inputPanel().reset();
                ic_->updateUserInterface(UserInterfaceComponent::InputPanel);
                ic_->updatePreedit();
                break;
            }
            default: {
                break;
            }
        }
    }

    void LotusState::commitBuffer() {
        switch (realMode) {
            case LotusMode::Preedit: {
                ic_->inputPanel().reset();
                if (lotusEngine_) {
                    EngineCommitPreedit(lotusEngine_.handle());
                    UniqueCPtr<char> commit(EnginePullCommit(lotusEngine_.handle()));
                    if (commit && (*commit.get() != 0))
                        ic_->commitString(commit.get());
                    ResetEngine(lotusEngine_.handle());
                }
                ic_->updateUserInterface(UserInterfaceComponent::InputPanel);
                ic_->updatePreedit();
                break;
            }
            case LotusMode::Uinput:
            case LotusMode::Smooth:
            case LotusMode::SurroundingText:
            case LotusMode::Minecraft:
            case LotusMode::SuperSmooth: {
                if (lotusEngine_) {
                    ResetEngine(lotusEngine_.handle());
                }
                break;
            }
            default: {
                break;
            }
        }
    }

    void LotusState::clearAllBuffers(bool force) {
        LOTUS_DEBUG("Clear all buffers");
        if (!force && is_deleting_.load(std::memory_order_acquire)) {
            return;
        }
        if (force) {
            is_deleting_.store(false, std::memory_order_release);
            const auto& surrounding = ic_->surroundingText();
            if (surrounding.isValid() && surrounding.cursor() == surrounding.anchor()) {
                realtextLen.store(surrounding.cursor(), std::memory_order_release);
            } else {
                realtextLen.store(0, std::memory_order_release);
            }
        }
        oldPreBuffer_.clear();
        hasHistory_ = false;
        expected_backspaces_     = 0;
        current_backspace_count_ = 0;
        pending_commit_string_.clear();
        pending_commit_fallback_timer_.reset();
        pending_replay_event_.reset();
        pending_replay_scheduled_ = false;
        pending_initial_hold_timer_.reset();
        timer_driven_replacement_ = false;
        pending_replacement_may_empty_input_ = false;
        pending_reset_engine_after_commit_ = false;
        holding_initial_uinput_preedit_ = false;
        suppress_surrounding_seed_once_ = force;
        emojiBuffer_.clear();
        emojiCandidates_.clear();
        buffered_keys_.clear();
        if (!direct_draining_) {
            direct_keys_.clear();
            direct_awaiting_ = false;
            direct_await_timer_.reset();
            direct_drain_event_.reset();
        }
        shouldCapitalize_  = false;
        isPrevSpace_       = false;
        isPrevHyphen_      = false;
        isPrevPunctuation_ = false;
        if (lotusEngine_)
            ResetEngine(lotusEngine_.handle());
    }

    bool LotusState::isEmptyHistory() const {
        return !hasHistory_;
    }

    void LotusState::replayBufferedKeys() {
        LOTUS_DEBUG("Starting replay buffered keys");
        if (buffered_keys_.empty()) {
            debugUinputTrace("replay_start empty");
            return;
        }
        debugUinputTrace("replay_start count=" + std::to_string(buffered_keys_.size()));
        auto keys = std::move(buffered_keys_);
        buffered_keys_.clear();
        for (size_t i = 0; i < keys.size(); ++i) {
            auto        sym     = static_cast<KeySym>(keys[i].sym);
            uint32_t    state   = keys[i].state;
            if (isEnterKey(sym)) {
                flushHeldInitialUinput();
                hasHistory_ = false;
                ResetEngine(lotusEngine_.handle());
                oldPreBuffer_.clear();
                suppress_surrounding_seed_once_ = true;
                replayBufferedSpecialKey(ic_, sym, state, keys[i].code);
                continue;
            }

            std::string keyUtf8 = keySymToBufferedUtf8(sym);
            if (keyUtf8.empty()) {
                continue;
            }

            if (isLiteralBraceKey(sym, state)) {
                hasHistory_ = false;
                ResetEngine(lotusEngine_.handle());
                oldPreBuffer_.clear();
                ic_->commitString(keyUtf8);
                realtextLen.fetch_add(static_cast<unsigned int>(utf8::length(keyUtf8)), std::memory_order_acq_rel);
                continue;
            }

            bool processed = EngineProcessKeyEvent(lotusEngine_.handle(), sym, state) != 0U;
            if (isUinputDebugEnabled()) {
                std::ostringstream oss;
                oss << "replay_key index=" << i << " sym=" << sym << " key='" << keyUtf8 << "' processed=" << processed << " oldPre='" << oldPreBuffer_
                    << "' realLen=" << realtextLen.load(std::memory_order_acquire);
                debugUinputTrace(oss.str());
            }

            auto commitF = UniqueCPtr<char>(EnginePullCommit(lotusEngine_.handle()));
            if (commitF && (*commitF.get() != 0)) {
                std::string commitStr = commitF.get();
                std::string deletedPart;
                std::string addedPart;
                compareAndSplitStrings(oldPreBuffer_, commitStr, deletedPart, addedPart);
                if (isUinputDebugEnabled()) {
                    std::ostringstream oss;
                    oss << "replay_commit commit='" << commitStr << "' deleted='" << deletedPart << "' added='" << addedPart << "'";
                    debugUinputTrace(oss.str());
                }

                if (!deletedPart.empty()) {
                    // Re-buffer remaining keys for next replay cycle.
                    for (size_t j = i + 1; j < keys.size(); ++j) {
                        if (buffered_keys_.size() < MAX_BUFFERED_KEYS) {
                            buffered_keys_.push_back(keys[j]);
                        }
                    }
                    performReplacement(deletedPart, addedPart);
                    hasHistory_ = false;
                    ResetEngine(lotusEngine_.handle());
                    oldPreBuffer_.clear();
                    if (!is_deleting_.load(std::memory_order_acquire)) {
                        // Replaced via surrounding text; nothing will trigger the next replay cycle.
                        scheduleReplayBufferedKeys();
                    }
                    return;
                }
                if (!addedPart.empty()) {
                    ic_->commitString(addedPart);
                    realtextLen.fetch_add(static_cast<unsigned int>(utf8::length(addedPart)), std::memory_order_acq_rel);
                }

                hasHistory_ = false;
                ResetEngine(lotusEngine_.handle());
                oldPreBuffer_.clear();
                suppress_surrounding_seed_once_ = true;
                continue;
            }

            if (!processed) {
                suppress_surrounding_seed_once_ = oldPreBuffer_.empty();
                ic_->commitString(keyUtf8);
                realtextLen.fetch_add(static_cast<unsigned int>(utf8::length(keyUtf8)), std::memory_order_acq_rel);
                continue;
            }

            hasHistory_ = true;

            UniqueCPtr<char> preeditC(EnginePullPreedit(lotusEngine_.handle()));
            std::string      preeditStr = (preeditC && (*preeditC.get() != 0)) ? preeditC.get() : "";

            std::string      deletedPart;
            std::string      addedPart;
            if (compareAndSplitStrings(oldPreBuffer_, preeditStr, deletedPart, addedPart) != 0) {
                if (isUinputDebugEnabled()) {
                    std::ostringstream oss;
                    oss << "replay_preedit preedit='" << preeditStr << "' deleted='" << deletedPart << "' added='" << addedPart << "'";
                    debugUinputTrace(oss.str());
                }
                if (deletedPart.empty()) {
                    if (!addedPart.empty()) {
                        ic_->commitString(addedPart);
                        realtextLen.fetch_add(static_cast<unsigned int>(utf8::length(addedPart)), std::memory_order_acq_rel);
                        oldPreBuffer_ = preeditStr;
                    } else {
                        ic_->commitString(keyUtf8);
                        realtextLen.fetch_add(static_cast<unsigned int>(utf8::length(keyUtf8)), std::memory_order_acq_rel);
                    }
                } else {
                    if (uinput_client_fd_ < 0) {
                        ic_->commitString(keyUtf8);
                        realtextLen.fetch_add(static_cast<unsigned int>(utf8::length(keyUtf8)), std::memory_order_acq_rel);
                        continue;
                    }

                    if (is_deleting_.load()) {
                        is_deleting_.store(false, std::memory_order_release);
                    }

                    // Re-buffer remaining keys for next replay cycle.
                    for (size_t j = i + 1; j < keys.size(); ++j) {
                        if (buffered_keys_.size() < MAX_BUFFERED_KEYS) {
                            buffered_keys_.push_back(keys[j]);
                        }
                    }
                    performReplacement(deletedPart, addedPart);
                    oldPreBuffer_ = preeditStr;
                    if (!is_deleting_.load(std::memory_order_acquire)) {
                        // Replaced via surrounding text; nothing will trigger the next replay cycle.
                        scheduleReplayBufferedKeys();
                    }
                    return;
                }
            } else {
                debugUinputTrace("replay_no_diff commit_raw key='" + keyUtf8 + "'");
                ic_->commitString(keyUtf8);
                realtextLen.fetch_add(static_cast<unsigned int>(utf8::length(keyUtf8)), std::memory_order_acq_rel);
            }
        }
        LOTUS_DEBUG("Replay buffered keys done");
    }
} // namespace fcitx
