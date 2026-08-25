/*
 * SPDX-FileCopyrightText: 2026 Nguyễn Hoàng Kỳ  <nhktmdzhg@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

/**
 * @file ack-apps.h
 * @brief List of applications requiring acknowledgment workaround.
 *
 * These browsers need special handling for uinput mode to work correctly.
 */

#include <string>
#include <vector>

/**
 * @brief List of application names requiring ACK workaround.
 *
 * Chromium-based browsers that need special handling for text replacement.
 */
static std::vector<std::string> ack_apps = {"chrome", "chromium", "brave", "edge", "vivaldi", "opera", "coccoc", "cromite", "helium", "thorium", "slimjet", "yandex"};

/**
 * @brief List of application names that should receive committed text directly.
 *
 * These applications become visually choppy when normal typing is forwarded
 * and then rewritten with uinput backspaces.
 */
static std::vector<std::string> direct_commit_apps = {"chrome",                "chromium", "brave",          "edge",    "vivaldi", "opera", "coccoc",
                                                      "cromite",               "helium",   "thorium",        "slimjet", "yandex",  "viber", "viberpc",
                                                      "gnome-terminal-server", "kgx",      "konsole",        "tilix",   "alacritty",
                                                      "kitty",                 "wezterm",  "org.wezfurlong", "foot",    "xterm",  "telegram"};
