// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 DQ8-Native contributors
// SPDX-FileCopyrightText: PS2Recomp contributors
// Adapted from PS2Recomp (ran-j/PS2Recomp, eb8688b), PS2Recomp contributors:
// ps2xRuntime/include/runtime/ps2_vu1.h.
#pragma once

#include "runtime/vu/vu_host.h"

#include <array>
#include <cstdint>
#include <cstring>

// The VU micro-mode interpreter (architecture §5.4): one instance per VU. It models the pipelines that
// micro programs can observe (FMAC, FDIV, EFU and integer latencies, the flag pipeline, branch delay and the
// VI branch-read rule, XGKICK pacing) and reaches the rest of the machine only through IVuHost: XGKICK
// packets, the micro-memory generation that invalidates its decode cache, and diagnostics.
namespace dq8::rt {

struct VuState {
    float vf[32][4];
    int32_t vi[16];
    float acc[4];
    float q;
    float p;
    float i;
    uint32_t r;
    uint32_t pc;
    uint32_t mac;
    uint32_t clip;
    uint32_t status;
    uint64_t cycles;
    bool ebit;
    bool haltAfterDelaySlot;
    bool dBitEnabled;
    bool tBitEnabled;
    bool stoppedByD;
    bool stoppedByT;
    uint32_t top;  // VIF TOP visible to XTOP
    uint32_t itop; // VIF ITOP visible to XITOP

    bool branchPending;
    uint32_t branchTarget;
    uint32_t branchDelay;
};

class VuInterpreter {
  public:
    enum class Unit : uint8_t { VU0, VU1 };

    explicit VuInterpreter(Unit unit = Unit::VU1);

    void reset();

    // Runs from startPC until the program ends (E bit, or an enabled D/T bit) or maxCycles pass.
    void execute(uint8_t* vuCode, uint32_t codeSize, uint8_t* vuData, uint32_t dataSize, IVuHost& host,
                 uint32_t startPC = 0, uint32_t top = 0, uint32_t itop = 0, uint32_t maxCycles = 65536);

    // Continues a program that ran out of cycles or stopped on D/T (MSCNT).
    void resume(uint8_t* vuCode, uint32_t codeSize, uint8_t* vuData, uint32_t dataSize, IVuHost& host,
                uint32_t top = 0, uint32_t itop = 0, uint32_t maxCycles = 65536);

    // True between an execute() that ran out of cycles and the end of the program.
    bool running() const { return m_running; }

    VuState& state() { return m_state; }
    const VuState& state() const { return m_state; }

  private:
    enum Pipeline : uint8_t {
        PipelineNone = 0,
        PipelineFmac,
        PipelineLsu,
        PipelineFdiv,
        PipelineEfu,
        PipelineIalu,
        PipelineBranch,
        PipelineXgkick
    };

    struct VfAccess {
        uint8_t reg = 0;
        uint8_t lanes = 0;
    };

    struct InstructionUsage {
        std::array<VfAccess, 2> vfRead{};
        VfAccess vfWrite{};
        uint8_t vfReadCount = 0;
        uint16_t viRead = 0;
        uint16_t viWrite = 0;
        uint8_t accRead = 0;
        uint8_t accWrite = 0;
        uint8_t latency = 0;
        uint8_t vfLatency = 0;
        uint8_t viLatency = 0;
        Pipeline pipeline = PipelineNone;
        bool waitQ = false;
        bool waitP = false;
        bool readsClip = false;
        bool writesClip = false;
        bool delaysNextBranchRead = false;
        bool reserved = false;
    };

    struct DecodedInstructionPair {
        uint32_t lower = 0;
        uint32_t upper = 0;
        InstructionUsage lowerUsage{};
        InstructionUsage upperUsage{};
        bool iBit = false;
        bool eBit = false;
        bool mBit = false;
        bool dBit = false;
        bool tBit = false;
        uint8_t upperVfShadowReg = 0;
        uint8_t suppressedLowerVf = 0;

        // Derived from the two usages at decode, for the scheduler: the pair's VF, VI and ACC reads (upper
        // and lower together), the VI register the lower instruction writes, whether its VF write survives
        // the upper's, and the latency each write becomes visible after.
        std::array<VfAccess, 4> vfReads{};
        uint8_t vfReadCount = 0;
        uint16_t viReads = 0;
        uint8_t accReads = 0;
        uint8_t writtenVi = 0;
        bool lowerVfWriteKept = false;
        uint8_t upperVfLatency = 0;
        uint8_t lowerVfLatency = 0;
        uint8_t viWriteLatency = 0;
    };

    struct FlagPipelineEntry {
        uint64_t readyCycle = 0;
        uint64_t issueCycle = 0;
        uint32_t mac = 0;
        uint32_t status = 0;
        uint32_t extraSticky = 0;
        uint32_t clip = 0;
        bool valid = false;
        bool writesMac = false;
        bool writesStatus = false;
        bool writesSticky = false;
        bool writesClip = false;
    };

    struct ScalarPipelineEntry {
        uint64_t readyCycle = 0;
        float value = 0.0f;
        uint32_t statusDi = 0;
        bool valid = false;
    };

    struct PendingStore {
        uint64_t readyCycle = 0;
        uint32_t address = 0;
        std::array<uint32_t, 4> words{};
        uint8_t laneMask = 0;
        bool valid = false;
    };

    struct PendingVfWrite {
        uint64_t readyCycle = 0;
        uint64_t sequence = 0;
        std::array<float, 4> value{};
        uint8_t reg = 0;
        uint8_t laneMask = 0;
        bool valid = false;
    };

    struct PendingViWrite {
        uint64_t readyCycle = 0;
        uint64_t sequence = 0;
        int32_t value = 0;
        uint8_t reg = 0;
        bool valid = false;
    };

    struct PendingAccWrite {
        uint64_t readyCycle = 0;
        uint64_t sequence = 0;
        std::array<float, 4> value{};
        uint8_t laneMask = 0;
        bool valid = false;
    };

    struct XgkickPipeline {
        static constexpr uint32_t kBufferSize = 0x10000u;
        std::array<uint8_t, kBufferSize> packet{};
        uint32_t sourceAddress = 0;
        uint32_t totalBytes = 0;
        uint32_t copiedBytes = 0;
        uint32_t currentTagEnd = 0;
        uint32_t cycleCredit = 0;
        uint64_t issueCycle = 0;
        bool active = false;
        bool currentTagEop = false;

        // Everything but the packet buffer: a transfer writes each byte of it before reading or sending it.
        void clear() {
            sourceAddress = 0;
            totalBytes = 0;
            copiedBytes = 0;
            currentTagEnd = 0;
            cycleCredit = 0;
            issueCycle = 0;
            active = false;
            currentTagEop = false;
        }
    };

    static constexpr uint32_t kFmacLatency = 4u;
    static constexpr uint32_t kAccForwardLatency = 1u;
    static constexpr uint32_t kMaxFlagEntries = 8u;
    static constexpr uint32_t kMaxPendingStores = 8u;
    static constexpr uint32_t kMaxPendingVfWrites = 16u;
    static constexpr uint32_t kMaxPendingViWrites = 8u;
    static constexpr uint32_t kMaxPendingAccWrites = 8u;
    static constexpr uint32_t kEfuEntries = 2u;
    static constexpr uint32_t kMaxDecodedPairs = 0x4000u / 8u;
    static constexpr uint64_t kNoPendingCommit = ~uint64_t{0};
    // More cycles than the longest latency (EFU, 54).
    static constexpr uint32_t kCommitSlots = 64u;
    static constexpr uint32_t kFlagCommitBit = 0u;
    static constexpr uint32_t kFdivCommitBit = kFlagCommitBit + kMaxFlagEntries;
    static constexpr uint32_t kEfuCommitBit = kFdivCommitBit + 1u;
    static constexpr uint32_t kStoreCommitBit = kEfuCommitBit + kEfuEntries;
    static constexpr uint32_t kVfWriteCommitBit = kStoreCommitBit + kMaxPendingStores;
    static constexpr uint32_t kViWriteCommitBit = kVfWriteCommitBit + kMaxPendingVfWrites;
    static constexpr uint32_t kAccWriteCommitBit = kViWriteCommitBit + kMaxPendingViWrites;
    static_assert(kAccWriteCommitBit + kMaxPendingAccWrites <= 64u, "a commit slot is one 64-bit mask");

    Unit m_unit;
    VuState m_state;
    std::array<DecodedInstructionPair, kMaxDecodedPairs> m_decodedCodeCache{};
    DecodedInstructionPair m_uncachedPair{};
    const uint8_t* m_cachedVuCode = nullptr;
    const IVuHost* m_cachedHost = nullptr;
    uint32_t m_cachedCodeSize = 0;
    uint64_t m_cachedCodeGeneration = 0;
    bool m_decodedCodeCacheValid = false;
    // Micro memory and its generation change only while the interpreter is not running or inside a host
    // call, so the decode cache is checked against them at the first fetch after either.
    bool m_codeCheckPending = true;

    std::array<FlagPipelineEntry, kMaxFlagEntries> m_flagPipeline{};
    ScalarPipelineEntry m_fdiv{};
    std::array<ScalarPipelineEntry, kEfuEntries> m_efu{};
    std::array<PendingStore, kMaxPendingStores> m_storePipeline{};
    std::array<PendingVfWrite, kMaxPendingVfWrites> m_vfWritePipeline{};
    std::array<PendingViWrite, kMaxPendingViWrites> m_viWritePipeline{};
    std::array<PendingAccWrite, kMaxPendingAccWrites> m_accWritePipeline{};
    XgkickPipeline m_xgkick{};
    std::array<std::array<uint64_t, 4>, 32> m_vfReady{};
    std::array<uint64_t, 16> m_viReady{};
    std::array<uint64_t, 4> m_accReady{};
    std::array<std::array<uint64_t, 4>, 32> m_vfLatestWrite{};
    std::array<uint64_t, 16> m_viLatestWrite{};
    std::array<uint64_t, 4> m_accLatestWrite{};

    uint64_t m_cycle = 0;
    // The valid entries of each pipeline array, one bit per index (alongside the entries' valid flags).
    uint32_t m_flagValid = 0;
    uint32_t m_efuValid = 0;
    uint32_t m_storeValid = 0;
    uint32_t m_vfWriteValid = 0;
    uint32_t m_viWriteValid = 0;
    uint32_t m_accWriteValid = 0;
    // The entries due at each of the next kCommitSlots cycles (slot = cycle % kCommitSlots), as bits in
    // commit order (k*CommitBit + index), the slots in use, and the first cycle with a slot in use (no
    // later).
    std::array<uint64_t, kCommitSlots> m_commitSlots{};
    uint64_t m_commitSlotsInUse = 0;
    uint64_t m_nextCommitCycle = kNoPendingCommit;
    uint64_t m_nextWriteSequence = 0;
    uint64_t m_efuResourceReady = 0;
    uint32_t m_workingClip = 0;
    uint32_t m_currentUpperInstruction = 0;
    // The current upper instruction's operands as execUpper normalized them, for its FMAC flags (they point
    // into execUpper's frame: valid while it runs).
    struct FmacOperands {
        const float* vs = nullptr;
        const float* vt = nullptr;
        const float* acc = nullptr;
        float q = 0.0f;
        float i = 0.0f;
    };
    FmacOperands m_fmacOperands{};
    int32_t m_viBranchBackupValue = 0;
    uint8_t m_viBranchBackupReg = 0;
    bool m_viBranchBackupValid = false;
    uint8_t* m_activeVuData = nullptr;
    uint32_t m_activeVuDataSize = 0;
    IVuHost* m_activeHost = nullptr;
    bool m_running = false;
    bool m_stopRequested = false;
    bool m_pendingHaltD = false;
    bool m_pendingHaltT = false;

    void run(uint8_t* vuCode, uint32_t codeSize, uint8_t* vuData, uint32_t dataSize, IVuHost& host,
             uint32_t maxCycles);

    InstructionUsage decodeUpperUsage(uint32_t upper) const;
    InstructionUsage decodeLowerUsage(uint32_t lower) const;
    static void addVfRead(InstructionUsage& usage, uint8_t reg, uint8_t lanes);
    static void addVfWrite(InstructionUsage& usage, uint8_t reg, uint8_t lanes);
    static uint8_t vfReadLanes(const InstructionUsage& usage, uint8_t reg);
    DecodedInstructionPair decodeInstructionPair(const uint8_t* vuCode, uint32_t pc) const;
    const DecodedInstructionPair& getDecodedInstructionPairForPc(const uint8_t* vuCode, uint32_t codeSize,
                                                                 IVuHost& host, uint32_t pc);
    void rebuildDecodedCodeCache(const uint8_t* vuCode, uint32_t codeSize, const IVuHost& host,
                                 uint64_t generation);

    void execUpper(uint32_t instr);
    void execLower(uint32_t instr, uint8_t* vuData, uint32_t dataSize, uint32_t upperInstr);

    void applyDest(float* dst, const float* result, uint8_t dest);
    void applyDestAcc(const float* result, uint8_t dest);
    void applyFmacDest(float* dst, float* result, uint8_t dest);
    void applyFmacDestAcc(float* result, uint8_t dest);
    void normalizeFmacResult(float* result, uint8_t dest, uint8_t laneFlags[4]);
    uint32_t calculateFmacProductSticky(uint8_t dest) const;
    void updateFmacFlags(const uint8_t laneFlags[4], uint8_t dest, uint32_t extraSticky);
    void queueFsset(uint16_t immediate);
    void queueClip(uint32_t clip);
    void queueFcset(uint32_t clip);
    void queueQ(float value, uint32_t latency, uint32_t statusDi);
    void queueP(float value, uint32_t latency);
    void queueStore(uint32_t address, const uint32_t words[4], uint8_t laneMask);
    void queueVfWrite(uint8_t reg, uint8_t laneMask, const float value[4], uint32_t latency);
    void queueViWrite(uint8_t reg, int32_t value, uint32_t latency);
    void queueAccWrite(uint8_t laneMask, const float value[4], uint32_t latency);
    void startXgkick(uint32_t qwordAddress);

    void resetScheduler();
    // Puts entry `bit` (k*CommitBit + index) in the commit slot of its ready cycle.
    void scheduleCommit(uint32_t bit, uint64_t readyCycle);
    void commitReadyPipelines() {
        if (m_cycle >= m_nextCommitCycle)
            commitDueEntries();
    }
    void commitDueEntries();
    void advanceOneCycle();
    void advanceTo(uint64_t targetCycle);
    void flushPipelines();
    void progressXgkick();
    void finishXgkick();
    uint64_t calculatePairReadyCycle(const DecodedInstructionPair& decoded) const;
    void markPairWrites(const DecodedInstructionPair& decoded);
    bool pipelinesPending() const;

    // Inline and without branches: every FMAC operand goes through normalizeOperand. Zero and denormals
    // become signed zero, infinities and NaNs the signed largest float.
    float normalizeOperand(float value) const {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        const uint32_t sign = bits & 0x80000000u;
        const uint32_t exponent = bits & 0x7F800000u;
        bits = exponent == 0u ? sign : exponent == 0x7F800000u ? (sign | 0x7F7FFFFFu) : bits;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    float normalizeResult(float value, uint32_t& laneFlags) const {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        const uint32_t sign = bits & 0x80000000u;
        const uint32_t magnitude = bits & 0x7FFFFFFFu;
        const uint32_t exponent = (bits >> 23) & 0xFFu;

        laneFlags = sign != 0u ? 0x2u : 0u;
        if (magnitude == 0u) {
            laneFlags |= 0x1u;
        } else if (exponent == 0u) {
            laneFlags |= 0x5u;
            bits = sign;
        } else if (exponent == 0xFFu) {
            laneFlags |= 0x8u;
            bits = sign | 0x7F7FFFFFu;
        }

        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    uint32_t microAddressMask() const;
    int32_t readBranchVi(uint8_t reg) const;
    void recordViWriteForBranch(uint8_t reg, int32_t oldValue);
    void reportReservedInstruction(bool upper, uint32_t instruction);
    float broadcast(const float* vf, uint8_t bc);
};

} // namespace dq8::rt
