// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <string>
#include <string_view>
#include <vector>

// Just enough Markdown for the About page to show THIRD_PARTY_NOTICES.md: headings, paragraphs, bullets,
// rules and pipe tables. Inline markup is reduced to its text: [text](url) keeps the text and the URL;
// `code` and **bold** lose their marks.
namespace dq8::launcher {

struct MdBlock {
    enum class Kind { heading, paragraph, bullet, table, rule };
    Kind kind = Kind::paragraph;
    int level = 0;                              // heading: 1 for "#"
    std::string text;                           // heading, paragraph, bullet
    std::vector<std::vector<std::string>> rows; // table: the header row first; the separator row dropped
};

std::vector<MdBlock> parse_markdown(std::string_view text);
std::string plain_inline(std::string_view text);

} // namespace dq8::launcher
