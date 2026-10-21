// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

// The SIF protocol as the EE's SDK libraries speak it (Docs/milestones/M3-boots.md, "Protocol notes"): the
// register numbers of the sceSifGetReg/SetReg kernel calls, the SMFLG handshake bits, the sceSifSetDma
// transfer descriptor, the command packet header, the system command ids and the RPC packet layouts. Offsets
// are our own tables, written from the protocol's observable behaviour; all values are little-endian words.
namespace dq8::rt::sif {

static_assert(std::endian::native == std::endian::little, "SIF packets are read as host words");

// Register numbers of sceSifGetReg / sceSifSetReg.
inline constexpr std::uint32_t reg_mscom = 1; // EE -> IOP mailbox word
inline constexpr std::uint32_t reg_smcom = 2; // IOP -> EE mailbox word: the IOP's command buffer address
inline constexpr std::uint32_t reg_msflg = 3; // EE -> IOP flags; an EE write sets bits
inline constexpr std::uint32_t reg_smflg = 4; // IOP -> EE flags; an EE write clears bits
// Numbers with the top bit set name the kernel's software registers (index = low bits).
inline constexpr std::uint32_t reg_software = 0x80000000u;
inline constexpr std::uint32_t software_reg_count = 32;
// Software registers the SDK uses: the IOP command buffer as the EE last learned it, the EE command layer's
// own data block, and an "RPC initialised" marker. An IOP reset clears 0 and 2.
inline constexpr std::uint32_t sreg_iop_command_buffer = 0;
inline constexpr std::uint32_t sreg_ee_command_data = 1;
inline constexpr std::uint32_t sreg_rpc_ready = 2;

// SMFLG bits the IOP raises during boot; the EE waits on them and clears them before an IOP reset.
inline constexpr std::uint32_t flag_sif_ready = 0x00010000u; // the IOP's SIF is up and SMCOM is valid
inline constexpr std::uint32_t flag_cmd_ready = 0x00020000u; // the IOP command layer is initialised
inline constexpr std::uint32_t flag_boot_done = 0x00040000u; // the IOP (re)boot has finished
inline constexpr std::uint32_t boot_flags = flag_sif_ready | flag_cmd_ready | flag_boot_done;

// The EE's view of the SIF registers: 0x1000F200 + offset.
inline constexpr std::uint32_t mmio_base = 0x1000F200u;
inline constexpr std::uint32_t mmio_size = 0x70u;
inline constexpr std::uint32_t mmio_mscom = 0x00;
inline constexpr std::uint32_t mmio_smcom = 0x10;
inline constexpr std::uint32_t mmio_msflg = 0x20;
inline constexpr std::uint32_t mmio_smflg = 0x30;
inline constexpr std::uint32_t mmio_ctrl = 0x40;
inline constexpr std::uint32_t mmio_bd6 = 0x60;

// IOP RAM as the EE sees it.
inline constexpr std::uint32_t iop_ram_phys = 0x1C000000u;

// One sceSifSetDma transfer descriptor (16 bytes): EE source, IOP destination, byte count, attributes.
inline constexpr std::uint32_t dma_desc_size = 16;
inline constexpr std::uint32_t dma_desc_src = 0x0;
inline constexpr std::uint32_t dma_desc_dest = 0x4;
inline constexpr std::uint32_t dma_desc_size_field = 0x8;
inline constexpr std::uint32_t dma_desc_attr = 0xC;
inline constexpr std::uint32_t dma_attr_int_ee = 0x02;  // interrupt the EE at the end
inline constexpr std::uint32_t dma_attr_int_iop = 0x04; // interrupt the IOP at the end (command packets)
inline constexpr std::uint32_t dma_attr_end = 0x40;     // end of the IOP-side chain
inline constexpr std::uint32_t dma_max_descriptors = 32;

// Command packet header (16 bytes). Word 0: packet size in bytes (bits 0-7, header included) and the size of
// the extra data sent ahead of the packet (bits 8-31); word 1: where that extra data went; word 2: command
// id; word 3: an option word.
inline constexpr std::uint32_t header_size = 16;
inline constexpr std::uint32_t hdr_sizes = 0x0;
inline constexpr std::uint32_t hdr_extra_dest = 0x4;
inline constexpr std::uint32_t hdr_cid = 0x8;
inline constexpr std::uint32_t hdr_opt = 0xC;
inline constexpr std::uint32_t max_packet_size = 0x70; // the largest packet either side's buffer holds

// Command ids. The top bit marks the system commands the SIF layers handle themselves.
inline constexpr std::uint32_t cid_system = 0x80000000u;
inline constexpr std::uint32_t cid_change_receive = 0x80000000u; // +0x10: the sender's new receive buffer
inline constexpr std::uint32_t cid_set_sreg = 0x80000001u;       // +0x10: index, +0x14: value
inline constexpr std::uint32_t cid_init = 0x80000002u;           // +0x10: the EE receive buffer (opt == 0)
inline constexpr std::uint32_t cid_reset = 0x80000003u;          // reset the IOP (layout below)
inline constexpr std::uint32_t cid_rpc_end = 0x80000008u;
inline constexpr std::uint32_t cid_rpc_bind = 0x80000009u;
inline constexpr std::uint32_t cid_rpc_call = 0x8000000Au;
inline constexpr std::uint32_t cid_rpc_rdata = 0x8000000Cu;
inline constexpr std::uint32_t cid_fileio_done = 0x80000011u; // fileio completion record is in EE memory

// Init's option word: 0 from the EE's command layer (with its receive buffer at +0x10), 1 from its RPC layer,
// which then waits for the IOP to set the EE's software register 0 to a non-zero value.
inline constexpr std::uint32_t init_opt_command = 0;
inline constexpr std::uint32_t init_opt_rpc = 1;
inline constexpr std::uint32_t rpc_ready_sreg = 0;
inline constexpr std::uint32_t rpc_ready_value = 1;

inline constexpr std::uint32_t change_receive_address = 0x10;
inline constexpr std::uint32_t change_receive_size = 0x14;
inline constexpr std::uint32_t set_sreg_index = 0x10;
inline constexpr std::uint32_t set_sreg_value = 0x14;
inline constexpr std::uint32_t set_sreg_size = 0x18;

// Reset packet: argument length, mode, then the argument string (an IOP image path, or empty).
inline constexpr std::uint32_t reset_arg_length = 0x10;
inline constexpr std::uint32_t reset_mode = 0x14;
inline constexpr std::uint32_t reset_arg = 0x18;
inline constexpr std::uint32_t reset_arg_max = 0x50;
inline constexpr std::uint32_t reset_size = reset_arg + reset_arg_max;

// Every RPC packet starts with these words after the header; the IOP echoes the first four in its end
// packet so that the EE can find the request and the client it belongs to.
inline constexpr std::uint32_t rpc_rec_id = 0x10;   // the EE's packet-slot flags
inline constexpr std::uint32_t rpc_pkt_addr = 0x14; // the EE address of the request packet
inline constexpr std::uint32_t rpc_id = 0x18;       // the EE's request number
inline constexpr std::uint32_t rpc_client = 0x1C;   // the EE client structure (rdata: its receive data)

inline constexpr std::uint32_t bind_sid = 0x20;
inline constexpr std::uint32_t bind_size = 0x24;

inline constexpr std::uint32_t call_fno = 0x20;
inline constexpr std::uint32_t call_send_size = 0x24;
inline constexpr std::uint32_t call_recv_addr = 0x28;
inline constexpr std::uint32_t call_recv_size = 0x2C;
inline constexpr std::uint32_t call_rmode = 0x30; // 0: no end packet, only the reply data
inline constexpr std::uint32_t call_server = 0x34;
inline constexpr std::uint32_t call_size = 0x38;

inline constexpr std::uint32_t rdata_src = 0x20;  // IOP address
inline constexpr std::uint32_t rdata_dest = 0x24; // EE address
inline constexpr std::uint32_t rdata_size = 0x28;
inline constexpr std::uint32_t rdata_packet_size = 0x2C;

// End packet (IOP -> EE): the echoed words, then the command it ends, the server and its buffers.
inline constexpr std::uint32_t end_cid = 0x20;
inline constexpr std::uint32_t end_server = 0x24;
inline constexpr std::uint32_t end_buffer = 0x28;
inline constexpr std::uint32_t end_cbuffer = 0x2C;
inline constexpr std::uint32_t end_size = 0x30;

// Little-endian word access to packet bytes; out-of-range offsets read 0 and write nothing.
inline std::uint32_t load32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint32_t v = 0;
    if (offset + 4 <= bytes.size()) {
        std::memcpy(&v, bytes.data() + offset, 4);
    }
    return v;
}
inline void store32(std::span<std::uint8_t> bytes, std::size_t offset, std::uint32_t value) {
    if (offset + 4 <= bytes.size()) {
        std::memcpy(bytes.data() + offset, &value, 4);
    }
}

inline std::uint32_t packet_size(std::span<const std::uint8_t> packet) {
    return load32(packet, hdr_sizes) & 0xFFu;
}
inline std::uint32_t extra_size(std::span<const std::uint8_t> packet) {
    return load32(packet, hdr_sizes) >> 8;
}

std::string_view command_name(std::uint32_t cid) noexcept;

} // namespace dq8::rt::sif
