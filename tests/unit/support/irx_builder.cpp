// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#include "support/irx_builder.h"

#include <cstring>
#include <stdexcept>
#include <utility>

namespace dq8::test {

namespace {

constexpr std::uint32_t align_up(std::uint32_t v, std::uint32_t a) {
    return (v + a - 1) & ~(a - 1);
}

void put16(std::vector<std::uint8_t>& b, std::size_t off, std::uint16_t v) {
    if (b.size() < off + 2) {
        b.resize(off + 2, 0);
    }
    b[off] = static_cast<std::uint8_t>(v);
    b[off + 1] = static_cast<std::uint8_t>(v >> 8);
}

void put32(std::vector<std::uint8_t>& b, std::size_t off, std::uint32_t v) {
    if (b.size() < off + 4) {
        b.resize(off + 4, 0);
    }
    for (unsigned i = 0; i < 4; ++i) {
        b[off + i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}

constexpr std::uint8_t r_mips_32 = 2;
constexpr std::uint8_t r_mips_26 = 4;
constexpr std::uint8_t r_mips_hi16 = 5;
constexpr std::uint8_t r_mips_lo16 = 6;

} // namespace

IrxBuilder::IrxBuilder(std::string name, std::uint16_t version) : name_(std::move(name)), version_(version) {}

IrxBuilder::Label IrxBuilder::label() {
    labels_.push_back(LabelInfo{});
    return Label{static_cast<std::uint32_t>(labels_.size() - 1)};
}

void IrxBuilder::bind(Label l) {
    LabelInfo& info = labels_.at(l.id);
    if (info.section != Section::none) {
        throw std::logic_error("label bound twice");
    }
    info.section = Section::text;
    info.offset = text_size();
}

IrxBuilder::Label IrxBuilder::here() {
    const Label l = label();
    bind(l);
    return l;
}

void IrxBuilder::emit(std::uint32_t word) {
    text_.push_back(word);
}

void IrxBuilder::imports(const std::string& library, std::initializer_list<std::uint16_t> ordinals,
                         std::uint16_t version) {
    emit(0x41E00000u);
    emit(0);
    emit(version);
    char name[8] = {};
    std::memcpy(name, library.data(), std::min<std::size_t>(library.size(), 8));
    for (int i = 0; i < 2; ++i) {
        std::uint32_t w = 0;
        std::memcpy(&w, name + 4 * i, 4);
        emit(w);
    }
    for (const std::uint16_t ordinal : ordinals) {
        stubs_[{library, ordinal}] = here();
        emit(mips::jr(mips::ra));
        emit(0x24000000u | ordinal);
    }
    emit(0);
    emit(0);
}

IrxBuilder::Label IrxBuilder::stub(std::string_view library, std::uint16_t ordinal) const {
    const auto it = stubs_.find({std::string(library), ordinal});
    if (it == stubs_.end()) {
        throw std::logic_error("no import stub for " + std::string(library) + " " + std::to_string(ordinal));
    }
    return it->second;
}

void IrxBuilder::jal(Label target) {
    text_fixups_.push_back(TextFixup{text_size(), Fix::j26, target});
    emit(mips::jal(0));
    emit(mips::nop());
}

void IrxBuilder::j(Label target) {
    text_fixups_.push_back(TextFixup{text_size(), Fix::j26, target});
    emit(mips::j(0));
    emit(mips::nop());
}

void IrxBuilder::la(mips::Reg r, Label target) {
    text_fixups_.push_back(TextFixup{text_size(), Fix::hi16, target});
    emit(mips::lui(r, 0));
    text_fixups_.push_back(TextFixup{text_size(), Fix::lo16, target});
    emit(mips::addiu(r, r, 0));
}

void IrxBuilder::li(mips::Reg r, std::uint32_t value) {
    const auto s = static_cast<std::int32_t>(value);
    if (s >= -32768 && s <= 32767) {
        emit(mips::addiu(r, mips::zero, s));
    } else {
        emit(mips::lui(r, value >> 16));
        emit(mips::ori(r, r, value & 0xFFFFu));
    }
}

void IrxBuilder::beq(mips::Reg rs, mips::Reg rt, Label target) {
    text_fixups_.push_back(TextFixup{text_size(), Fix::branch16, target});
    emit(mips::beq(rs, rt, 0));
    emit(mips::nop());
}

void IrxBuilder::bne(mips::Reg rs, mips::Reg rt, Label target) {
    text_fixups_.push_back(TextFixup{text_size(), Fix::branch16, target});
    emit(mips::bne(rs, rt, 0));
    emit(mips::nop());
}

void IrxBuilder::b(Label target) {
    beq(mips::zero, mips::zero, target);
}

void IrxBuilder::call(Label target, std::initializer_list<std::uint32_t> args) {
    if (args.size() > 4) {
        throw std::logic_error("call() passes at most four arguments in registers");
    }
    unsigned i = 0;
    for (const std::uint32_t a : args) {
        li(static_cast<mips::Reg>(mips::a0 + i++), a);
    }
    jal(target);
}

void IrxBuilder::ret() {
    emit(mips::jr(mips::ra));
    emit(mips::nop());
}

void IrxBuilder::enter(std::uint32_t frame) {
    const auto f = static_cast<std::int32_t>(frame);
    emit(mips::addiu(mips::sp, mips::sp, -f));
    emit(mips::sw(mips::ra, f - 4, mips::sp));
    emit(mips::sw(mips::s0, f - 8, mips::sp));
    emit(mips::sw(mips::s1, f - 12, mips::sp));
}

void IrxBuilder::leave(std::uint32_t frame) {
    const auto f = static_cast<std::int32_t>(frame);
    emit(mips::lw(mips::ra, f - 4, mips::sp));
    emit(mips::lw(mips::s0, f - 8, mips::sp));
    emit(mips::lw(mips::s1, f - 12, mips::sp));
    emit(mips::jr(mips::ra));
    emit(mips::addiu(mips::sp, mips::sp, f));
}

void IrxBuilder::store(Label target, std::uint32_t offset, mips::Reg r) {
    la(mips::t9, target);
    emit(mips::sw(r, static_cast<std::int32_t>(offset), mips::t9));
}

void IrxBuilder::load(mips::Reg r, Label target, std::uint32_t offset) {
    la(mips::t9, target);
    emit(mips::lw(r, static_cast<std::int32_t>(offset), mips::t9));
    emit(mips::nop());
}

void IrxBuilder::append(Label log, mips::Reg r) {
    la(mips::t9, log);
    emit(mips::lw(mips::t8, 0, mips::t9));
    emit(mips::nop());
    emit(mips::addiu(mips::t7, mips::t8, 1));
    emit(mips::sw(mips::t7, 0, mips::t9));
    emit(mips::sll(mips::t8, mips::t8, 2));
    emit(mips::addu(mips::t8, mips::t8, mips::t9));
    emit(mips::sw(r, 4, mips::t8));
}

void IrxBuilder::append_value(Label log, std::uint32_t value) {
    li(mips::t6, value);
    append(log, mips::t6);
}

IrxBuilder::Label IrxBuilder::data(std::span<const std::uint8_t> bytes, std::uint32_t align) {
    data_.resize(align_up(static_cast<std::uint32_t>(data_.size()), align), 0);
    const Label l = label();
    labels_[l.id] = LabelInfo{Section::data, static_cast<std::uint32_t>(data_.size())};
    data_.insert(data_.end(), bytes.begin(), bytes.end());
    return l;
}

IrxBuilder::Label IrxBuilder::words(std::initializer_list<std::uint32_t> values) {
    std::vector<std::uint8_t> bytes;
    for (const std::uint32_t v : values) {
        put32(bytes, bytes.size(), v);
    }
    return data(bytes, 4);
}

IrxBuilder::Label IrxBuilder::zeros(std::uint32_t size, std::uint32_t align) {
    const std::vector<std::uint8_t> bytes(size, 0);
    return data(bytes, align);
}

IrxBuilder::Label IrxBuilder::string(std::string_view text) {
    std::vector<std::uint8_t> bytes(text.begin(), text.end());
    bytes.push_back(0);
    return data(bytes, 4);
}

void IrxBuilder::pointer(Label at, std::uint32_t offset, Label target) {
    const LabelInfo& info = labels_.at(at.id);
    if (info.section != Section::data) {
        throw std::logic_error("pointer() needs a data label");
    }
    data_fixups_.push_back(DataFixup{info.offset + offset, target});
}

IrxBuilder::Label IrxBuilder::exports(const std::string& library, std::initializer_list<Label> functions,
                                      std::uint16_t version) {
    std::vector<std::uint8_t> bytes;
    put32(bytes, 0, 0x41C00000u);
    put32(bytes, 4, 0);
    put32(bytes, 8, version);
    bytes.resize(20, 0);
    std::memcpy(bytes.data() + 12, library.data(), std::min<std::size_t>(library.size(), 8));
    bytes.resize(20 + 4 * (functions.size() + 1), 0);
    const Label table = data(bytes, 16);
    std::uint32_t off = 20;
    for (const Label f : functions) {
        pointer(table, off, f);
        off += 4;
    }
    return table;
}

IrxBuilder::Label IrxBuilder::bss(std::uint32_t size, std::uint32_t align) {
    bss_size_ = align_up(bss_size_, align);
    const Label l = label();
    labels_[l.id] = LabelInfo{Section::bss, bss_size_};
    bss_size_ += size;
    return l;
}

void IrxBuilder::raw_relocation(std::uint32_t text_offset, std::uint8_t type) {
    raw_.emplace_back(text_offset, type);
}

IrxBuilder::Layout IrxBuilder::layout() const {
    Layout l;
    l.data_base = align_up(text_size(), 16);
    l.bss_base = l.data_base + align_up(static_cast<std::uint32_t>(data_.size()), 16);
    l.mem_size = l.bss_base + align_up(bss_size_, 16);
    return l;
}

std::uint32_t IrxBuilder::address(Label l, const Layout& layout) const {
    const LabelInfo& info = labels_.at(l.id);
    switch (info.section) {
    case Section::text:
        return info.offset;
    case Section::data:
        return layout.data_base + info.offset;
    case Section::bss:
        return layout.bss_base + info.offset;
    case Section::none:
        break;
    }
    throw std::logic_error("a label is used but never bound");
}

std::uint32_t IrxBuilder::offset_of(Label l) const {
    return address(l, layout());
}

std::vector<std::uint8_t> IrxBuilder::build() const {
    const Layout lay = layout();
    std::vector<std::uint32_t> text = text_;
    std::vector<std::uint8_t> data = data_;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> rel_text; // offset, info
    std::vector<std::pair<std::uint32_t, std::uint32_t>> rel_data;
    for (const TextFixup& f : text_fixups_) {
        const std::uint32_t a = address(f.target, lay);
        std::uint32_t& w = text[f.offset / 4];
        switch (f.kind) {
        case Fix::j26:
            w |= (a >> 2) & 0x03FFFFFFu;
            rel_text.emplace_back(f.offset, r_mips_26);
            break;
        case Fix::hi16:
            w |= ((a + 0x8000u) >> 16) & 0xFFFFu;
            rel_text.emplace_back(f.offset, r_mips_hi16);
            break;
        case Fix::lo16:
            w |= a & 0xFFFFu;
            rel_text.emplace_back(f.offset, r_mips_lo16);
            break;
        case Fix::branch16: {
            const auto delta = (static_cast<std::int64_t>(a) - static_cast<std::int64_t>(f.offset + 4)) / 4;
            w |= static_cast<std::uint32_t>(delta) & 0xFFFFu;
            break;
        }
        }
    }
    for (const auto& [offset, type] : raw_) {
        rel_text.emplace_back(offset, type);
    }
    for (const DataFixup& f : data_fixups_) {
        put32(data, f.offset, address(f.target, lay));
        rel_data.emplace_back(lay.data_base + f.offset, r_mips_32);
    }

    // The image: text, data, up to the bss.
    std::vector<std::uint8_t> image;
    for (const std::uint32_t w : text) {
        put32(image, image.size(), w);
    }
    image.resize(lay.data_base, 0);
    image.insert(image.end(), data.begin(), data.end());
    image.resize(lay.bss_base, 0);

    // The .iopmod record.
    std::vector<std::uint8_t> iopmod;
    put32(iopmod, 0, 0xFFFFFFFFu);
    put32(iopmod, 4, address(entry_, lay));
    put32(iopmod, 8, lay.data_base + 0x7FF0u);
    put32(iopmod, 12, text_size());
    put32(iopmod, 16, lay.bss_base - lay.data_base);
    put32(iopmod, 20, lay.mem_size - lay.bss_base);
    put16(iopmod, 24, version_);
    iopmod.insert(iopmod.end(), name_.begin(), name_.end());
    iopmod.push_back(0);

    const std::string shstrtab =
        std::string("\0.iopmod\0.text\0.data\0.bss\0.rel.text\0.rel.data\0.shstrtab\0", 57);
    const std::uint32_t iopmod_off = 52 + 2 * 32;
    const std::uint32_t image_off = align_up(iopmod_off + static_cast<std::uint32_t>(iopmod.size()), 16);
    const std::uint32_t rel_text_off = image_off + static_cast<std::uint32_t>(image.size());
    const std::uint32_t rel_data_off = rel_text_off + static_cast<std::uint32_t>(rel_text.size() * 8);
    const std::uint32_t shstr_off = rel_data_off + static_cast<std::uint32_t>(rel_data.size() * 8);
    const std::uint32_t sh_off = align_up(shstr_off + static_cast<std::uint32_t>(shstrtab.size()), 4);

    std::vector<std::uint8_t> f;
    const std::uint8_t ident[16] = {0x7F, 'E', 'L', 'F', 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    f.insert(f.end(), ident, ident + 16);
    put16(f, 16, elf_type_);
    put16(f, 18, 8); // MIPS
    put32(f, 20, 1);
    put32(f, 24, address(entry_, lay));
    put32(f, 28, 52);
    put32(f, 32, sh_off);
    put32(f, 36, 0);
    put16(f, 40, 52);
    put16(f, 42, 32);
    put16(f, 44, 2);
    put16(f, 46, 40);
    put16(f, 48, 8);
    put16(f, 50, 7);
    // Program headers: .iopmod, then the image.
    const std::uint32_t ph[2][8] = {
        {0x70000080u, iopmod_off, 0, 0, static_cast<std::uint32_t>(iopmod.size()), 0, 4, 4},
        {1, image_off, 0, 0, lay.bss_base, lay.mem_size, 7, 16},
    };
    for (int i = 0; i < 2; ++i) {
        for (int k = 0; k < 8; ++k) {
            put32(f, 52 + 32 * static_cast<std::size_t>(i) + 4 * static_cast<std::size_t>(k), ph[i][k]);
        }
    }
    f.resize(iopmod_off, 0);
    f.insert(f.end(), iopmod.begin(), iopmod.end());
    f.resize(image_off, 0);
    f.insert(f.end(), image.begin(), image.end());
    for (const auto& [offset, info] : rel_text) {
        put32(f, f.size(), offset);
        put32(f, f.size(), info);
    }
    for (const auto& [offset, info] : rel_data) {
        put32(f, f.size(), offset);
        put32(f, f.size(), info);
    }
    f.insert(f.end(), shstrtab.begin(), shstrtab.end());
    f.resize(sh_off, 0);
    // Section headers: name, type, flags, addr, offset, size, link, info, addralign, entsize.
    const std::uint32_t sh[8][10] = {
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {1, 0x70000080u, 0, 0, iopmod_off, static_cast<std::uint32_t>(iopmod.size()), 0, 0, 4, 0},
        {9, 1, 6, 0, image_off, text_size(), 0, 0, 16, 0},
        {15, 1, 3, lay.data_base, image_off + lay.data_base, lay.bss_base - lay.data_base, 0, 0, 16, 0},
        {21, 8, 3, lay.bss_base, image_off + lay.bss_base, lay.mem_size - lay.bss_base, 0, 0, 16, 0},
        {26, 9, 0, 0, rel_text_off, static_cast<std::uint32_t>(rel_text.size() * 8), 0, 2, 4, 8},
        {36, 9, 0, 0, rel_data_off, static_cast<std::uint32_t>(rel_data.size() * 8), 0, 3, 4, 8},
        {46, 3, 0, 0, shstr_off, static_cast<std::uint32_t>(shstrtab.size()), 0, 0, 1, 0},
    };
    for (const auto& s : sh) {
        for (const std::uint32_t v : s) {
            put32(f, f.size(), v);
        }
    }
    return f;
}

} // namespace dq8::test
