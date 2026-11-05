// Original synthetic instruction tests; no game payloads, GPU or game execution.
// Build: clang++ -std=c++20 -O2 -static -I runtime/third_party/eglantine tests/hwaoi_ampere_lowering.cpp -o work/build/hwaoi196608/xlate-ampere-test.exe
#include <cstdlib>
#include <cstdio>
#include <stdexcept>
#include <cmath>
#include <limits>
#include "shader_recompiler/frontend/ampere/program_capture.h"
using namespace Shader::Ampere;

static void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
static Instruction op(u32 opcode, u32 dst = 0, u32 a = 0, u32 b = 0, u32 flags = 0, u32 pred = 7) {
    return {{opcode | (pred << 12) | (dst << 16) | (a << 24), b, flags, 0}};
}
static const Instruction exit_op = op(0x94d, 0, 0, 0, 7U << 23);
static LoweredProgram lower(Instruction instruction) {
    const std::array code{instruction, exit_op};
    return LowerToMaxwell(code);
}

// Independent small Maxwell evaluator for just the ALU/predicate subset tested
// below. Unknown opcodes throw instead of treating an unimplemented op as a NOP.
struct Machine {
    std::array<u32, 768> r{};
    std::array<bool, 7> p{};
    u32 constant = 0;
    u32 active_lanes = ~0U;
    std::array<u32, 32> lane_values{};
    u32 shared_value = 0;
    bool carry_flag = false;
    std::array<float, 4> texture_values{};
    std::array<u32, 3> texture_coordinates{};
    u32 texture_calls = 0, texture_mask = 0, texture_component = 0;
    static float half_float(u32 h) {
        const u32 exponent = (h >> 10) & 31, fraction = h & 1023;
        check(exponent < 31, "finite half test input");
        const float f = std::ldexp(exponent ? 1.0f + float(fraction) / 1024 : float(fraction) / 1024,
                                   exponent ? int(exponent) - 15 : -14);
        return h & 0x8000 ? -f : f;
    }
    // Independent numerical oracle: search the representable finite half values,
    // compare exact double distances, and choose the even significand on a tie.
    // This deliberately does not implement the production F2F bit manipulation.
    static u32 nearest_half(float f) {
        const u32 sign = std::signbit(f) ? 0x8000 : 0;
        if (std::isnan(f)) return sign | 0x7e00;
        const double magnitude = std::fabs(static_cast<double>(f));
        if (magnitude >= 65520.0) return sign | 0x7c00;
        static const auto positive = [] {
            std::array<float, 0x7c00> values{};
            for (u32 h = 0; h < values.size(); ++h) values[h] = half_float(h);
            return values;
        }();
        const auto next = std::lower_bound(positive.begin(), positive.end(), magnitude);
        if (next == positive.end()) return sign | 0x7bff;
        const u32 high = static_cast<u32>(next - positive.begin());
        if (high == 0) return sign;
        const double below = magnitude - positive[high - 1], above = positive[high] - magnitude;
        return sign | (below < above || (below == above && (high & 1)) ? high - 1 : high);
    }
    u32 read(u32 reg) const { return reg == 255 ? 0 : r.at(reg); }
    void run(const LoweredProgram& program) {
        check(bool(program), "unexpected lowering rejection");
        for (u64 word : program.instructions) {
            if (((word >> 44) & 15) == 10 && (word >> 48) == 0x5c98) {
                const u64 fields = 0xffULL | (0xfULL << 8) | (0xffULL << 20);
                check((word & ~fields) == 0x5c98a00000070000ULL, "strict IR-bank encoding");
                const u32 db = (word >> 8) & 3, sb = (word >> 10) & 3;
                check(db < 3 && sb < 3, "IR-bank bounds");
                const u32 dst = db * 256 + (word & 255), src = sb * 256 + ((word >> 20) & 255);
                if (dst != 255) r.at(dst) = read(src);
                continue;
            }
            const u32 pred = (word >> 16) & 15, pi = pred & 7;
            if ((pi == 7 ? true : p[pi]) == bool(pred & 8)) continue;
            const u32 dst = word & 255, a = (word >> 8) & 255, b = (word >> 20) & 255;
            const u32 imm = static_cast<u32>(word >> 20), top = word >> 48;
            if ((word >> 52) == 0xe30) break;
            u32 value;
            if ((word >> 56) == 0x01) value = imm;
            else if (top == 0x38e8) {
                const u32 mask = imm & 127;
                value = read(a) & ~mask;
                for (u32 k = 0; k < 7; ++k) if ((mask >> k) & 1) value |= u32(p[k]) << k;
            } else if (top == 0x38f0) {
                for (u32 k = 0; k < 7; ++k) if ((imm >> k) & 1) p[k] = (read(a) >> k) & 1;
                continue;
            } else if (top == 0x5c98) value = read(b);
            else if (top == 0x4c98) value = constant;
            else if (top == 0x3848) value = (imm & 0x7ffff) < 32 ? read(a) << (imm & 31) : 0;
            else if ((top & ~1U) == 0x3828) {
                const u32 shift = imm & 0x7ffff;
                if (top & 1) value = static_cast<u32>(static_cast<s32>(read(a)) >> (std::min)(shift, 31U));
                else value = shift < 32 ? read(a) >> shift : 0;
            } else if (top == 0x5c48) {
                const u32 shift = (word & (u64{1} << 39)) ? read(b) & 31 : read(b);
                value = shift < 32 ? read(a) << shift : 0;
            } else if ((top & ~1U) == 0x5c18) {
                value = (read(a) << ((word >> 39) & 31)) + ((top & 1) ? 0U - read(b) : read(b));
            } else if ((top & ~1U) == 0x5c10) {
                const u64 sum = u64{read(a)} + ((top & 1) ? 0U - read(b) : read(b)) +
                                ((word & (u64{1} << 43)) && carry_flag ? 1U : 0U);
                value = static_cast<u32>(sum);
                if (word & (u64{1} << 47)) carry_flag = sum >> 32;
            } else if (top == 0x5ca0) {
                const u32 cond = (word >> 39) & 15;
                const bool enabled = ((cond & 7) == 7 ? true : p[cond & 7]) != bool(cond & 8);
                value = enabled ? read(a) : read(b);
            } else if ((top & 0xffc0) == 0x5b00) {
                const u32 c = (word >> 39) & 255, mode = (word >> 50) & 7;
                check(mode <= 2 && !(word & (u64{7} << 36)), "XMAD fixture mode");
                const u32 aa = (read(a) >> ((word & (u64{1} << 53)) ? 16 : 0)) & 0xffff;
                const u32 bb = (read(b) >> ((word & (u64{1} << 35)) ? 16 : 0)) & 0xffff;
                value = aa * bb + (mode == 0 ? read(c) : mode == 1 ? read(c) & 0xffff : read(c) >> 16);
            }
            else if ((top & ~1U) == 0x50d8) value = active_lanes;
            else if ((word >> 52) == 0xef1) value = lane_values.at((word >> 20) & 31);
            else if ((word >> 56) == 4) value = read(a) & imm;
            else if ((top & ~7U) == 0x5c40) {
                const u32 rhs = (word & (u64{1} << 40)) ? ~read(b) : read(b);
                const u32 mode = (word >> 41) & 3;
                check(mode < 3, "tested LOP mode");
                value = mode == 0 ? read(a) & rhs : mode == 1 ? read(a) | rhs : read(a) ^ rhs;
            } else if (top == 0x5c20) value = (word & (u64{1} << 42)) ? (std::max)(read(a), read(b)) : (std::min)(read(a), read(b));
            else if (top == 0x5ca8) {
                const u32 destination_size = (word >> 8) & 3, source_size = (word >> 10) & 3;
                if (destination_size == 1 && source_size == 2) {
                    check(!(word & ((u64{3} << 39) | (u64{1} << 41) | (u64{1} << 44))),
                          "F2F half conversion uses nearest rounding without saturation");
                    float f = std::bit_cast<float>(read(b));
                    if (word & (u64{1} << 49)) f = std::fabs(f);
                    if (word & (u64{1} << 45)) f = -f;
                    value = nearest_half(f);
                } else {
                    check(destination_size == 2 && source_size == 1, "tested F2F formats");
                    const u32 h = (read(b) >> ((word & (u64{1} << 41)) ? 16 : 0)) & 0xffff;
                    value = std::bit_cast<u32>(half_float(h));
                }
            } else if (top == 0x5cb0) {
                check(((word >> 39) & 3) == 3 && (word & (u64{1} << 12)), "signed truncating F2I test");
                value = static_cast<u32>(static_cast<s32>(std::bit_cast<float>(read(b))));
            } else if (top == 0x5cb8) {
                const float f = (word & (u64{1} << 13)) ? float(static_cast<s32>(read(b))) : float(read(b));
                value = std::bit_cast<u32>(f);
            } else if (top == 0x5c08) value = std::popcount(read(b));
            else if ((word >> 56) == 0x5d) {
                check(((word >> 28) & 3) == 0 && ((word >> 35) & 3) == 0 && ((word >> 47) & 3) == 0,
                      "packed HFMA independent fixture");
                const u32 c = (word >> 39) & 255;
                value = 0;
                for (u32 lane = 0; lane < 2; ++lane) {
                    float aa = half_float((read(a) >> (lane * 16)) & 0xffff);
                    float bb = half_float((read(b) >> (lane * 16)) & 0xffff);
                    float cc = half_float((read(c) >> (lane * 16)) & 0xffff);
                    if (word & (u64{1} << 31)) aa = -aa;
                    if (word & (u64{1} << 30)) cc = -cc;
                    const float sum = std::fma(aa, bb, cc);
                    // These original fixtures use exact representable results;
                    // do not invent an approximate half-rounding oracle.
                    u32 h = 0;
                    while (h < 0x7c00 && half_float(h) != std::fabs(sum)) ++h;
                    check(h < 0x7c00, "HFMA fixture exact half result");
                    if (sum < 0) h |= 0x8000;
                    value |= h << (lane * 16);
                }
            } else if ((word >> 56) == 0xec) {
                const u32 operation = (word >> 52) & 15, argument = read(b);
                const bool sign = word & (u64{1} << 28);
                value = shared_value;
                if (operation == 0) shared_value += argument;
                else if (operation == 1) shared_value = sign ? static_cast<u32>((std::min)(static_cast<s32>(value), static_cast<s32>(argument))) : (std::min)(value, argument);
                else if (operation == 2) shared_value = sign ? static_cast<u32>((std::max)(static_cast<s32>(value), static_cast<s32>(argument))) : (std::max)(value, argument);
                else if (operation == 5) shared_value &= argument;
                else if (operation == 6) shared_value |= argument;
                else if (operation == 7) shared_value ^= argument;
                else if (operation == 8) shared_value = argument;
                else throw std::runtime_error("untested atomic operation");
            }
            else if ((top & ~1U) == 0x3800) {
                const u32 pos = imm & 255, width = (imm >> 8) & 255;
                check(width > 0 && width < 32 && pos + width <= 32, "BFE test operand");
                value = (read(a) >> pos) & ((1U << width) - 1);
                if ((top & 1) && (value & (1U << (width - 1)))) value |= ~((1U << width) - 1);
            } else if ((word >> 48) == 0x36f8) {
                const u32 shift = (word >> 20) & 0x1f, high = (word >> 39) & 255;
                const u32 amount = (word >> 20) & 0x7ffff;
                value = amount == 32 ? read(a) : shift == 0 ? read(high) : (read(high) << shift) | (read(a) >> (32 - shift));
            } else if ((word >> 52) == 0x36f) {
                const u32 pos = imm & 255, width = (imm >> 8) & 255, c = (word >> 39) & 255;
                check(width > 0 && width < 32 && pos + width <= 32, "BFI test operand");
                const u32 mask = ((1U << width) - 1) << pos;
                value = (read(c) & ~mask) | ((read(a) << pos) & mask);
            } else if ((word & 0xf800000000000000ULL) == 0xd800000000000000ULL) {
                // Decode Maxwell scalar F32 TLDS independently: target 2 takes
                // integer x from Ra, y from Rb, and implicit integer LOD zero.
                check(((word >> 53) & 15) == 2 && ((word >> 36) & 0x1fff) == 7,
                      "mock texel fetch is scalar 2D LZ, handle seven");
                const u32 second = (word >> 28) & 255, swizzle = (word >> 50) & 7;
                const u32 two_masks[8]={1,2,4,8,3,9,10,12};
                const u32 split_masks[5]={7,11,13,14,15};
                check(second==255 || swizzle<5, "TLDS selected result swizzle");
                texture_mask = second==255 ? two_masks[swizzle] : split_masks[swizzle];
                texture_coordinates={read(a),read(b),0};texture_component=0;++texture_calls;
                for (u32 channel=0,output=0;channel<4;channel++) if((texture_mask>>channel)&1) {
                    const u32 target=output<2 ? dst+output : second+output-2;
                    r.at(target)=std::bit_cast<u32>(texture_values[channel]);++output;
                }
                continue;
            } else if ((word & 0xfc00000000000000ULL) == 0xc800000000000000ULL ||
                       (word & 0xf800000000000000ULL) == 0xc000000000000000ULL) {
                // Mock just the image lookup: decode Maxwell's selected channel
                // mask and write the supplied texel values in mask order. All
                // following conversion/packing instructions execute normally.
                const bool gather = (word & 0xfc00000000000000ULL) == 0xc800000000000000ULL;
                const u32 dimension = (word >> 28) & 7;
                check(dimension == 2 && ((word >> 54) & 3) == 0 &&
                      !(word & (u64{1} << 50)) && ((word >> 36) & 0x1fff) == 7,
                      "mock texture is non-shadow 2D, no offsets, handle seven");
                texture_coordinates = {read(a), read(a + 1), read(b)};
                texture_mask = (word >> 31) & 15;
                texture_component = gather ? (word >> 56) & 3 : 0;
                ++texture_calls;
                u32 output = dst;
                for (u32 channel = 0; channel < 4; ++channel)
                    if ((texture_mask >> channel) & 1)
                        r.at(output++) = std::bit_cast<u32>(texture_values[channel]);
                continue;
            } else throw std::runtime_error("unexpected opcode in independent evaluator");
            if (dst != 255) r[dst] = value;
        }
    }
};

int main() try {
    unsigned checks = 0;
    for (u32 pred : {1U, 9U}) for (bool up : {false, true}) for (bool old_p : {false, true}) {
        auto program = lower(op(0x899, 9, 63, 2, 0x08011600 | 14, pred));
        Machine m; m.r[239] = up ? 1U << 5 : 0; m.p[5] = old_p;
        m.r[230] = 0xabcdef01; m.r[225] = 0x13579bdf;
        m.run(program);
        check(m.r[225] == ((up != bool(pred & 8)) ? 0x2af37bc0U : 0x13579bdfU), "USHF guard semantics");
        check(m.p[5] == old_p, "USHF restores ordinary predicate"); ++checks;
    }
    for (u32 shift : {0U, 1U, 31U, 32U, 33U, 63U}) for (bool wrap : {false, true}) {
        Machine m; m.r[3] = shift;
        m.run(lower(op(0x419, 2, 255, 0x12345678, 0x00010603 | (wrap ? 0x800 : 0))));
        const u32 s = wrap ? shift & 31 : shift;
        check(m.r[2] == (s < 32 ? 0x12345678U << s : 0), "SHF wrap semantics"); ++checks;
    }
    for (u32 value : {0U, 0x00008001U, 0x12347f80U, 0xffffffffU}) {
        Machine m; m.r[224] = value;
        m.run(lower(op(0x896, 6, 8, 0x9910, 0x0800003f)));
        const u32 sign_byte = (value & 0x8000) ? 0xff : 0;
        check(m.r[222] == ((value & 0xffff) | (sign_byte << 16) | (sign_byte << 24)), "UPRMT sign replication"); ++checks;
    }
    for (u32 shift : {0U, 1U, 8U, 31U}) {
        Machine m; m.r[4] = 0xfedcba98; m.constant = 0x12345678;
        m.run(lower(op(0xa11, 5, 4, 0, 0x078e00ff | (shift << 11))));
        check(m.r[5] == (0xfedcba98U << shift) + 0x12345678U, "constant LEA no carry");
        Machine n; n.r[4] = 0xfedcba98; n.r[221] = 0x12345678;
        n.run(lower(op(0xc11, 5, 4, 5, 0x0f8e00ff | (shift << 11))));
        check(n.r[5] == m.r[5], "uniform LEA semantics"); ++checks;
    }
    for (u32 width = 4; width <= 6; ++width) {
        const u32 count = 1U << (width - 4);
        for (bool store : {false, true}) {
            auto result = lower(op(store ? 0x387 : 0x983, store ? 0 : 12, 255,
                                   (0x80U << 8) | (store ? 12U : 0U), 0x00100000 | (width << 9)));
            check(bool(result) && result.instructions.size() == count + 1, "vector local spill count");
            for (u32 i = 0; i < count; ++i) {
                const auto word = result.instructions[i];
                check((word & 255) == 12 + i && ((word >> 20) & 0xffffff) == 0x80 + 4 * i,
                      "vector local spill register and byte offset");
                check(((word >> 48) & 7) == 4, "vector spill lowers to scalar32");
            } ++checks;
        }
    }
    for (u32 reg = 28; reg < 63; ++reg) {
        const std::array code{op(0x882, reg, 0, 0x1234), op(0xc02, 1, 0, reg, 0x08000f00), exit_op};
        Machine m; m.run(LowerToMaxwell(code));
        check(m.r[1] == 0x1234, "high uniform mirror round trip"); ++checks;
    }
    check(bool(lower(op(0x91a, 0, 0, 0x8002))), "DEPBAR wait list accepted"); ++checks;
    check(!lower(op(0x91a, 0, 0, 0x18002)), "DEPBAR unknown mode rejected"); ++checks;
    check(!lower(op(0x899, 9, 63, 33, 0x0801160e)), "USHF range rejected"); ++checks;
    check(!lower(op(0x419, 2, 1, 0, 0x00010603)), "SHF nonzero low half rejected"); ++checks;
    check(!lower(op(0x387, 0, 255, (0x80U << 8) | 13, 0x00100c00)), "misaligned vector rejected"); ++checks;
    check(!lower(op(0x988, 0, 255, 12, 0x00000809)), "STS missing uniform marker rejected"); ++checks;
    const std::array conflict{op(0x882, 56, 0, 1), op(0x802, 180, 0, 0), exit_op};
    check(!LowerToMaxwell(conflict), "high uniform contiguous register guard"); ++checks;
    const std::array store_conflict{op(0x882, 56, 0, 1), op(0x988, 0, 255, 180, 0x08000800), exit_op};
    check(!LowerToMaxwell(store_conflict), "special-form data GPR alias guard"); ++checks;
    const std::array old_conflict{op(0x882, 0, 0, 1), op(0x802, 233, 0, 0), exit_op};
    check(!LowerToMaxwell(old_conflict), "ordinary scratch guard remains"); ++checks;
    for (bool pred : {false, true}) {
        // Predicated high native writes preserve old state on false predicates;
        // borrowed low registers and overlapping uniform mirrors remain intact.
        const std::array code{op(0x882, 0, 0, 0x4567), op(0x802, 233, 0, 0x1234, 0xf00, 1),
                             op(0x202, 2, 0, 233, 0x00000f00), exit_op};
        auto lowered = LowerToMaxwell(code, {}, true);
        check(bool(lowered) && lowered.uses_ir_banks, "IR-bank opt-in emits marker");
        Machine m; m.p[1] = pred; m.r[489] = 0x9988;
        for (u32 i = 0; i < 216; ++i) m.r[i] = 1000 + i;
        m.run(lowered);
        check(m.r[2] == (pred ? 0x1234U : 0x9988U), "shadow preserves predication");
        check(m.r[216] == 0x4567, "native shadow does not overwrite uniform mirror");
        for (u32 i = 3; i < 216; ++i) check(m.r[i] == 1000 + i, "borrow restore");
        ++checks;
    }
    // A vector straddling a quartet is remapped contiguously, not to separate
    // independently allocated low quartets.
    const auto crossed = PrepareRegisterPressure(op(0xb66, 232, 230, 255, 0x078e0f00), 216);
    check(crossed.valid && crossed.instruction.Bits(24, 8) + 3 < 216, "cross-quartet span allocation"); ++checks;
    check(!PrepareRegisterPressure(op(0xfff, 233), 216).valid, "unknown pressure trait rejected"); ++checks;
    for (u32 active : {1U, 0x80000000U, 0x55555555U, ~0U}) {
        Machine m; m.active_lanes = active;
        u32 expected = ~0U;
        for (u32 i = 0; i < 32; ++i) {
            m.lane_values[i] = (active & (1U << i)) ? 1000 - i * 7 : 0;
            if (active & (1U << i)) expected = (std::min)(expected, m.lane_values[i]);
        }
        m.run(lower(op(0x3c4, 7, 3, 0, 0x10000)));
        check(m.r[223] == expected, "REDUX MIN excludes inactive zero-valued lanes"); ++checks;
    }
    for (u32 half : {0x3c00U, 0x4300U, 0xc300U}) for (bool high : {false, true}) {
        Machine m; m.r[3] = high ? (half << 16) | 0x4900 : 0x49000000 | half;
        m.run(lower(op(0x305, 4, 0, 3 | (high ? 1U << 28 : 0), 0x0010f100)));
        const s32 expected = half == 0x3c00 ? 1 : half == 0x4300 ? 3 : -3;
        check(static_cast<s32>(m.r[4]) == expected, "F2I signed H0/H1 selection and truncation"); ++checks;
    }
    Machine immediate_float;
    immediate_float.run(lower(op(0x845, 4, 0, 32, 0x0020d000)));
    check(immediate_float.r[4] == std::bit_cast<u32>(32.0f), "I2FP immediate"); ++checks;
    Machine constant_shift; constant_shift.constant = 0x1234; constant_shift.r[6] = 4;
    constant_shift.run(lower(op(0x619, 7, 255, 0, 0x00010606)));
    check(constant_shift.r[7] == 0x12340, "constant value SHF"); ++checks;
    for (bool sign : {false, true}) for (u32 operation : {1U, 2U}) {
        Machine m; m.r[3] = 0xfffffff0; m.shared_value = 10;
        m.run(lower(op(0x38c, 4, 255, 3, (operation << 23) | (sign ? 0x200 : 0))));
        const bool take_negative = (operation == 1) == sign;
        check(m.r[4] == 10 && m.shared_value == (take_negative ? 0xfffffff0U : 10U), "shared atomic signedness/return"); ++checks;
    }
    auto multisample = lower(op(0xb66, 4, 6, 0x30000003, 0x008e41ff));
    check(bool(multisample) && ((multisample.instructions[0] >> 53) & 15) == 6 &&
          ((multisample.instructions[0] >> 8) & 255) == 6 && ((multisample.instructions[0] >> 20) & 255) == 3,
          "TLDS multisample coordinate pair and sample operand"); ++checks;
    check(!lower(op(0xb66, 4, 7, 0x30000003, 0x008e41ff)), "unaligned MS coordinate pair rejected"); ++checks;
    std::array callee{op(0x802, 0, 0, 32, 0xf00), op(0x944, 0, 0, 16, 0x03c00000), exit_op,
                      op(0x20b, 0, 4, 5, 0x03f1d000), exit_op};
    check(bool(LowerToMaxwell(callee)), "FSETP has no GPR destination, preserves link R0"); ++checks;
    callee[3] = op(0x802, 0, 0, 7, 0xf00);
    check(!LowerToMaxwell(callee), "real callee link write still rejected"); ++checks;
    Machine abs_float; abs_float.r[3] = std::bit_cast<u32>(-3.5f);
    abs_float.run(lower(op(0x305, 4, 0, 0x40000003, 0x0021f100)));
    check(abs_float.r[4] == 3, "F2I source absolute before integer conversion"); ++checks;
    Machine abs_half; abs_half.r[4] = 0x3c003c00; abs_half.r[5] = 0xc000c000; abs_half.r[6] = 0xc200c200;
    abs_half.run(lower(op(0x231, 2, 4, 0x40000005, 0x00080006)));
    check(abs_half.r[2] == 0x45004500, "HFMA ABS B/C sign clearing"); ++checks;
    for (u32 bits : {0U, 1U, 0x12345678U, ~0U}) {
        Machine m; m.r[222] = bits;
        m.run(lower(op(0x2bf, 4, 0, 6, 0x08000000)));
        check(m.r[220] == std::popcount(bits), "UPOPC"); ++checks;
    }
    for (u32 shift : {0U, 1U, 16U, 31U, 32U}) {
        Machine m; m.r[220] = 0x12345678;
        m.run(lower(op(0x899, 5, 4, shift, 0x08010604)));
        check(m.r[221] == std::rotl(0x12345678U, int(shift)), "uniform funnel rotate"); ++checks;
    }
    auto indexed = lower(op(0xb82, 8, 0, 0, 0x00008a00));
    check(bool(indexed) && ((indexed.instructions[0] >> 44) & 3) == 2, "LDC IS addressing mode"); ++checks;
    check(!lower(op(0xb82, 8, 0, 0, 0x00004a00)), "unknown indexed mode remains rejected"); ++checks;
    for (u32 mask : {5U, 6U}) {
        auto fetched = lower(op(0xb66, 4, 6, 0x30000003, 0x008e00ff | (mask << 8)));
        check(bool(fetched) && ((fetched.instructions[0] >> 28) & 255) == 246,
              "TLD missing two-channel swizzle discards extra alpha in scratch"); ++checks;
    }
    auto gradient = lower(op(0xb6c, 4, 8, 0x2000000c, 0x001e070e));
    check(bool(gradient) && ((gradient.instructions[0] >> 31) & 15) == 7 && gradient.instructions.size() == 5,
          "TXD three channels, nonconsecutive third destination"); ++checks;
    auto gather_offset = lower(op(0xb63, 4, 8, 0x3000000c, 0x000e130a));
    check(bool(gather_offset) && ((gather_offset.instructions[3] >> 54) & 3) == 1,
          "gather single common offset mode"); ++checks;
    auto match32 = lower(op(0x3a1, 255, 3, 0, 2U << 17));
    auto match64 = lower(op(0x3a1, 255, 2, 0, (2U << 17) | 0x200));
    check(bool(match32) && bool(match64) && match64.instructions.size() == match32.instructions.size() + 2,
          "MATCH U32 compares one word, U64 compares both"); ++checks;
    for (u32 carry0 = 0; carry0 < 16; ++carry0) for (u32 carry1 = 0; carry1 < 16; ++carry1) {
        Machine m; m.p = {false,true,false,true,false,true,false};
        m.r[3] = 0xfffffff0; m.r[4] = 0x20; m.r[5] = 0xffffffff;
        const auto cond = [&](u32 p) { return ((p & 7) == 7 ? true : m.p[p & 7]) != bool(p & 8); };
        const u32 expected = m.r[3] + m.r[4] + m.r[5] + cond(carry0) + cond(carry1);
        m.run(lower(op(0x210, 5, 3, 4, 5 | 0x007e0400 | (carry0 << 23) | (carry1 << 13))));
        check(m.r[5] == expected, "IADD3.X both carry predicates, destination alias and wrap"); ++checks;
    }
    for (u32 a : {0U,1U,0x7fffffffU,0xffffffffU}) for (u32 b : {0U,1U,0x24924925U,0xffffffffU}) {
        Machine m; m.r[220] = a; m.r[224] = 0x76543210; m.r[225] = 0x12345678;
        const u64 expected = u64{a} * b + 0x1234567876543210ULL;
        m.run(lower(op(0x8a5, 6, 4, b, 0x0f8e0008)));
        check(m.r[222] == static_cast<u32>(expected) && m.r[223] == expected >> 32,
              "UIMAD.WIDE immediate exact 64-bit product plus addend"); ++checks;
    }
    for (bool offsets : {false,true}) {
        auto array_gather = lower(op(0xb63, 4, 8, 0xb000000c, 0x000e0f0a | (offsets ? 0x1000 : 0)));
        check(bool(array_gather), "array gather accepted");
        const u32 texture_at = offsets ? 4 : 3;
        check(((array_gather.instructions[texture_at] >> 28) & 7) == 3,
              "array gather retains array dimension");
        check(((array_gather.instructions[0] >> 20) & 255) == 8 &&
              ((array_gather.instructions[1] >> 20) & 255) == 9 &&
              ((array_gather.instructions[2] >> 20) & 255) == 12,
              "array gather packs layer/x/y independently");
        if (offsets) check(((array_gather.instructions[3] >> 20) & 255) == 13,
                          "array gather offset follows y"); ++checks;
    }
    // Hand-written IEEE binary16 expectations include even/odd rounding ties,
    // subnormals and their normal boundary, signed zero, overflow and infinities.
    const std::array<std::pair<float, u32>, 16> half_cases{{
        {0.0f, 0x0000}, {-0.0f, 0x8000}, {1.0f, 0x3c00}, {-2.0f, 0xc000},
        {1.00048828125f, 0x3c00}, {1.00146484375f, 0x3c02},
        {0x1p-25f, 0x0000}, {0x1.8p-24f, 0x0002}, {0x1p-24f, 0x0001},
        {0x1.ff8p-15f, 0x03ff}, {0x1.ffcp-15f, 0x0400},
        {65504.0f, 0x7bff}, {65520.0f, 0x7c00},
        {std::numeric_limits<float>::infinity(), 0x7c00},
        {-std::numeric_limits<float>::infinity(), 0xfc00}, {-0x1p-24f, 0x8001},
    }};
    for (const auto [f, expected] : half_cases)
        check(Machine::nearest_half(f) == expected, "independent half reference constants");
    // Gather, direct fetch and the general TEX route must keep the same component order.
    // Binding zero forces TEX through the general path, away from TEXS.
    for (u32 opcode : {0xb63U, 0xb60U, 0xb66U}) for (u32 mask = 1; mask < 16; ++mask)
        for (u32 destination : {5U, 8U, 12U}) for (u32 pred : {1U, 9U})
        for (bool enabled : {false, true}) for (u32 group = 0; group < half_cases.size(); group += 4) {
            const u32 count = std::popcount(mask), second = count > 2 ? 13 : 255;
            Machine m; m.p[1] = enabled;
            for (u32 i = 0; i < 246; ++i) m.r[i] = 0x12340000 + i;
            const auto before = m.r;
            for (u32 i = 0; i < 4; ++i) m.texture_values[i] = half_cases[group + i].first;
            const u32 component = opcode == 0xb63 ? group / 4 : 0;
            m.run(lower(op(opcode, destination, 8, 0x3000070c,
                           0x040e8000 | (opcode == 0xb66 ? 1U<<23 : component << 23) | (mask << 8) | second, pred)));
            const bool executes = enabled != bool(pred & 8);
            check(m.texture_calls == u32(executes), "texture predicate suppresses lookup");
            std::array<u32, 4> selected{};
            for (u32 c = 0, out = 0; c < 4; ++c)
                if ((mask >> c) & 1) selected[out++] = half_cases[group + c].second;
            if (executes) {
                check(m.texture_coordinates[0] == before[8] && m.texture_coordinates[1] == before[12],
                      "texture snapshots coordinates before overlapping destination writes");
                const u32 fetch_mask = opcode==0xb66 && (mask==5 || mask==6) ? mask|8 : mask;
                check(m.texture_mask == fetch_mask && m.texture_component == component,
                      "texture channel mask and gather component retained");
                if(opcode==0xb66)check(m.texture_coordinates[2]==0,"texel fetch has implicit LOD zero");
                check(m.r[destination] == (selected[0] | (selected[1] << 16)),
                      "texture first pair has c0 low and c1 high");
                if (count > 2) check(m.r[second] == (selected[2] | (selected[3] << 16)),
                                     "texture split pair has c2 low and c3 high");
            }
            for (u32 i = 0; i < 246; ++i)
                if (!executes || (i != destination && (count <= 2 || i != second)))
                    check(m.r[i] == before[i], "texture preserves every other native register");
            ++checks;
        }
    for (bool negative : {false, true}) {
        Machine m; m.texture_values = {std::bit_cast<float>(negative ? 0xffc12345U : 0x7fc12345U), -0.0f, 1, 2};
        m.run(lower(op(0xb63, 5, 8, 0x3000070c, 0x000e83ff)));
        const u32 nan = m.r[5] & 0xffff;
        check((nan & 0x7c00) == 0x7c00 && (nan & 0x3ff) != 0 &&
              (m.r[5] >> 16) == 0x8000, "gather preserves NaN category and negative-zero high half");
        ++checks;
    }
    // Odd coordinates, nonconsecutive odd outputs, and output/input aliases from
    // the captured half fetch shape; all sampled inputs must be consumed first.
    for(u32 a : {5U,12U,43U})for(u32 b : {3U,18U,44U})for(u32 dst : {3U,5U,45U}) {
        Machine m;m.texture_values={1,-2,3,4};m.r[a]=123;m.r[b]=456;const auto before=m.r;
        m.run(lower(op(0xb66,dst,a,0x30000700|b,0x049e8701)));
        check(m.texture_coordinates[0]==before[a] && m.texture_coordinates[1]==before[b],"half fetch odd coordinates and aliases");
        check(m.r[dst]==0xc0003c00 && m.r[1]==0x00004200,"half fetch RGB packs third component with zero high half");++checks;
    }
    for(u32 flags : {0x049e8703U|(1U<<12),0x049e8703U|(1U<<13),0x049e8703U|(1U<<14),
                     0x049e8703U|(1U<<16),0x049e8703U|(1U<<27),0x049e8703U&~(7U<<17),
                     0x041e8703U,0x051e8703U,0x049e87ffU,0x049e872dU,0x049e8003U,0x049e8303U}) {
        check(!lower(op(0xb66,45,12,0x3000072c,flags)),"half fetch unsupported mode or result pair rejected");++checks;
    }
    check(!lower(op(0xb66,45,12,0x2000072c,0x049e8703)),"half vector fetch remains rejected");++checks;
    const std::array fetch_binding_conflict{op(0xb66,45,12,0x3000072c,0x049e8703),
        op(0xb66,45,12,0x3040072c,0x049e8703),exit_op};
    check(!LowerToMaxwell(fetch_binding_conflict),"half fetch descriptor binding conflict rejected");++checks;
    // Scalar .F16 supports independent output registers, including RZ discard,
    // but does not silently accept formats whose ABI has not been established.
    Machine discard; discard.texture_values = {1, 2, 3, 4};
    discard.run(lower(op(0xb63, 255, 8, 0x3000070c, 0x000e8f0d)));
    check(discard.r[13] == 0x44004200, "gather discarded first pair still writes split second pair"); ++checks;
    check(!lower(op(0xb63, 13, 8, 0x3000070c, 0x000e8f0d)),
          "ambiguous overlapping gather output pairs rejected"); ++checks;
    for (u32 mask : {3U, 12U, 15U}) {
        Machine m; m.texture_values = {1, -2, 3, -4};
        m.run(lower(op(0xb63, 4, 8, 0x3000070c, 0x000e0000 | (mask << 8) | (mask == 15 ? 12 : 255))));
        for (u32 channel = 0, out = 0; channel < 4; ++channel) if ((mask >> channel) & 1) {
            const u32 target = out < 2 ? 4 + out : 12 + out - 2;
            check(m.r[target] == std::bit_cast<u32>(m.texture_values[channel]), "F32 gather results stay unpacked");
            ++out;
        }
        ++checks;
    }
    check(!lower(op(0xb63, 5, 8, 0x3000070c, 0x000e8fff)), "half gather missing split destination rejected"); ++checks;
    check(!lower(op(0xb63, 5, 8, 0x3000070c, 0x000e80ff)), "empty half gather channel mask rejected"); ++checks;
    const std::array gather_binding_conflict{
        op(0xb63, 5, 8, 0x3000070c, 0x000e83ff),
        op(0xb63, 5, 8, 0x3040070c, 0x000e83ff), exit_op};
    check(!LowerToMaxwell(gather_binding_conflict), "half gather descriptor binding conflict rejected"); ++checks;
    for (u32 flag : {1U << 12, 1U << 13, 1U << 14, 1U << 16, 1U << 27}) {
        check(!lower(op(0xb63, 5, 8, 0x3000070c, 0x000e83ff | flag)),
              "unsupported half gather mode rejected"); ++checks;
    }
    check(!lower(op(0xb63, 5, 8, 0xb000070c, 0x000e83ff)), "half array gather remains rejected"); ++checks;
    check(!lower(op(0xb63, 5, 8, 0x2000070c, 0x000e83ff)), "half vector gather remains rejected"); ++checks;
    for (u32 reg : {246U, 250U, 254U}) {
        check(!lower(op(0xb63, reg, 8, 0x3000070c, 0x000e83ff)), "half gather scratch destination rejected");
        check(!lower(op(0xb63, 5, reg, 0x3000070c, 0x000e83ff)), "half gather scratch coordinate rejected");
        check(!lower(op(0xb63, 5, 8, 0x3000070c, 0x000e8f00 | reg)), "half gather scratch split rejected");
        ++checks;
    }
    ProgramCaptureState capture;
    check(capture.Observe(op(0x94d, 0, 0, 0, 1U << 23), 0) == ProgramCaptureState::Result::Continue,
          "DKB secondary EXIT predicate capture preserved"); ++checks;
    check(capture.Observe(exit_op, 1) == ProgramCaptureState::Result::Complete, "final EXIT capture"); ++checks;
    std::printf("PASS %u original semantic/encoding/failure cases\n", checks);
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1;
}
