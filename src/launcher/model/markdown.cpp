// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "model/markdown.h"

#include <algorithm>

namespace dq8::launcher {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

bool is_table_separator(std::string_view line) {
    line = trim(line);
    if (line.size() < 3 || line.front() != '|') {
        return false;
    }
    return std::all_of(line.begin(), line.end(),
                       [](char c) { return c == '|' || c == '-' || c == ':' || c == ' '; });
}

std::vector<std::string> table_cells(std::string_view line) {
    line = trim(line);
    if (!line.empty() && line.front() == '|') {
        line.remove_prefix(1);
    }
    if (!line.empty() && line.back() == '|') {
        line.remove_suffix(1);
    }
    std::vector<std::string> cells;
    std::string cell;
    bool in_code = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '`') {
            in_code = !in_code;
        }
        if (c == '\\' && i + 1 < line.size() && line[i + 1] == '|') {
            cell += '|';
            ++i;
            continue;
        }
        if (c == '|' && !in_code) {
            cells.push_back(plain_inline(trim(cell)));
            cell.clear();
            continue;
        }
        cell += c;
    }
    cells.push_back(plain_inline(trim(cell)));
    return cells;
}

constexpr std::size_t no_block = static_cast<std::size_t>(-1);

bool is_rule(std::string_view line) {
    line = trim(line);
    return line.size() >= 3 && std::all_of(line.begin(), line.end(), [](char c) { return c == '-'; });
}

} // namespace

std::string plain_inline(std::string_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '`') {
            continue;
        }
        if (c == '*' && i + 1 < text.size() && text[i + 1] == '*') {
            ++i;
            continue;
        }
        if (c == '[') {
            const std::size_t close = text.find(']', i + 1);
            if (close != std::string_view::npos && close + 1 < text.size() && text[close + 1] == '(') {
                const std::size_t end = text.find(')', close + 2);
                if (end != std::string_view::npos) {
                    const std::string_view label = text.substr(i + 1, close - i - 1);
                    const std::string_view url = text.substr(close + 2, end - close - 2);
                    out += plain_inline(label);
                    if (!url.empty() && url != label && url.find("://") != std::string_view::npos) {
                        out += " <" + std::string(url) + ">";
                    }
                    i = end;
                    continue;
                }
            }
        }
        out += c;
    }
    return out;
}

std::vector<MdBlock> parse_markdown(std::string_view text) {
    std::vector<MdBlock> blocks;
    std::vector<std::string_view> lines;
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        lines.push_back(text.substr(0, nl));
        if (nl == std::string_view::npos) {
            break;
        }
        text.remove_prefix(nl + 1);
    }
    std::size_t open = no_block; // a paragraph or bullet that continues on the next line
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string_view line = trim(lines[i]);
        if (line.empty()) {
            open = no_block;
            continue;
        }
        if (line.front() == '#') {
            const std::size_t level = line.find_first_not_of('#');
            if (level != std::string_view::npos && line[level] == ' ') {
                MdBlock b;
                b.kind = MdBlock::Kind::heading;
                b.level = static_cast<int>(level);
                b.text = plain_inline(trim(line.substr(level)));
                blocks.push_back(std::move(b));
                open = no_block;
                continue;
            }
        }
        if (is_rule(line)) {
            blocks.push_back({MdBlock::Kind::rule, 0, {}, {}});
            open = no_block;
            continue;
        }
        if (line.front() == '|') {
            MdBlock b;
            b.kind = MdBlock::Kind::table;
            for (; i < lines.size() && !trim(lines[i]).empty() && trim(lines[i]).front() == '|'; ++i) {
                if (!is_table_separator(lines[i])) {
                    b.rows.push_back(table_cells(lines[i]));
                }
            }
            --i;
            blocks.push_back(std::move(b));
            open = no_block;
            continue;
        }
        if ((line.front() == '-' || line.front() == '*') && line.size() > 1 && line[1] == ' ') {
            MdBlock b;
            b.kind = MdBlock::Kind::bullet;
            b.text = plain_inline(trim(line.substr(2)));
            blocks.push_back(std::move(b));
            open = blocks.size() - 1;
            continue;
        }
        if (open != no_block) {
            blocks[open].text += ' ';
            blocks[open].text += plain_inline(line);
            continue;
        }
        MdBlock b;
        b.kind = MdBlock::Kind::paragraph;
        b.text = plain_inline(line);
        blocks.push_back(std::move(b));
        open = blocks.size() - 1;
    }
    return blocks;
}

} // namespace dq8::launcher
