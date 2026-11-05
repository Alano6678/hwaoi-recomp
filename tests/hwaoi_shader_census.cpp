// Offline production-wrapper census. No game/GPU execution. Uses the existing
// harness's logging/hash helpers, but validates complete dumps, exports C1 and
// compute root/shared metadata, lists every outcome, and exits nonzero on rejects.
// The generated files belong in ignored work/, never in source control.
#define main legacy_lower_test_main
#include "../tools/dkb_lower_test.cpp"
#undef main
#include <iomanip>
#include <sstream>
#include <stdexcept>

static uint32_t get32(const uint8_t* data, size_t at) {
    uint32_t value; std::memcpy(&value, data + at, 4); return value;
}
static std::string utf8_path(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}
int main(int argc, char** argv) try {
    if (argc != 3) return std::fprintf(stderr, "usage: %s <dump dir> <new output dir>\n", argv[0]), 2;
    const auto out = std::filesystem::absolute(argv[2]);
    if (std::filesystem::exists(out)) throw std::runtime_error("output must be a new directory");
    std::filesystem::create_directories(out);
    std::ofstream jobs(out / "jobs.json"), report(out / "lowering.csv");
    jobs << "[\n";
    report << "key,outcome,detail\n";
    int total = 0, lowered = 0, emitted = 0;
    for (const auto& entry : std::filesystem::directory_iterator(argv[1])) {
        if (entry.path().extension() != ".bin") continue;
        ++total;
        current = entry.path().stem().string();
        auto code = slurp(entry.path());
        auto ctl = slurp(std::filesystem::path(entry.path()).replace_extension(".ctl"));
        if (ctl.size() < 0xd8 || code.size() < 16 || get32(ctl.data(), 0x30) > ctl.size() ||
            get32(ctl.data(), 0x38) > code.size() || get32(ctl.data(), 0x44) > code.size()) {
            report << current << ",invalid,truncated dump\n"; continue;
        }
        const uint8_t *nctl, *ncode;
        const auto before = nvn2_lower_failures();
        const auto previous_reasons = reasons;
        if (!nvn2_lower(ctl.data(), code.data(), &nctl, &ncode)) {
            report << current << ",rejected,";
            for (const auto& [why, count] : reasons) {
                const auto prior = previous_reasons.find(why);
                if (prior == previous_reasons.end() || count != prior->second) report << why;
            }
            report << '\n';
            // A second request must reuse the failure without counting it again.
            const auto rejected = nvn2_lower_failures();
            if (rejected != before + 1 || nvn2_lower(ctl.data(), code.data(), &nctl, &ncode) ||
                nvn2_lower_failures() != rejected) throw std::runtime_error("failure counter/cache mismatch");
            continue;
        }
        ++lowered;
        const auto length = get32(nctl, 0x704), stage = get32(nctl, 0x714), c1 = get32(nctl, 0x700);
        const char* name = stage == 0 ? "vs" : stage == 1 ? "ps" : stage == 5 ? "cs" : nullptr;
        if (!name) { report << current << ",unexported,stage=" << stage << '\n'; continue; }
        if (c1 > length) throw std::runtime_error("bad lowered inline constant range");
        const uint32_t skip = stage == 5 ? 0 : 0x30;
        std::ofstream(out / (current + ".code"), std::ios::binary).write(reinterpret_cast<const char*>(ncode + skip), length - skip);
        std::ostringstream hex;
        for (uint32_t i = c1; i < length; ++i) hex << std::hex << std::setfill('0') << std::setw(2) << unsigned(ncode[i]);
        jobs << (emitted++ ? ",\n" : "") << "{\"key\":\"" << current << "\",\"stage\":\"" << name
             << "\",\"code\":\"" << utf8_path(out / (current + ".code")) << "\",\"c1\":\"" << hex.str() << "\"";
        if (nvn2_ir_bank_version(nctl)) jobs << ",\"hwaoi_ir_banks\":1";
        if (stage == 5) {
            jobs << ",\"local\":[" << get32(nctl, 0x748) << ',' << get32(nctl, 0x74c) << ',' << get32(nctl, 0x750)
                 << "],\"shared\":" << get32(nctl, 0x868)
                 << ",\"storage_candidates_version\":1,\"c0_version\":2,\"c0\":[";
            for (int i = 0; i < 4; ++i) jobs << (i ? "," : "") << unsigned(nctl[0x864 + i]);
            jobs << "],\"c0_roots\":[";
            for (int i = 0; i < 8; ++i) jobs << (i ? "," : "") << unsigned(nctl[0x86c + i]);
            jobs << ']';
        }
        jobs << '}';
        report << current << ",exported," << name << '\n';
    }
    jobs << "\n]\n";
    std::printf("%d/%d lowered; %d translator jobs; %llu unique lower failures\n", lowered, total, emitted,
                static_cast<unsigned long long>(nvn2_lower_failures()));
    return lowered == total && emitted == lowered ? 0 : 1;
} catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what()); return 2;
}
