// ============================================================================
//  tests/build_cli.cpp – run the ON-DEVICE dictionary builder on the PC
// ============================================================================
//
//      .pio/build_cli <sd-root> <source> <work-dir>
//
//  <sd-root> plays the SD card (like JISHO_SD), <source> and <work-dir> are
//  SD paths inside it. The builder runs exactly as on the device: step() in a
//  loop, 30 ms at a time. tests/build_compare.sh uses this to compare the
//  output with the Python converter's, byte for byte.
// ============================================================================
#include <cstdio>
#include <cstdlib>

#include "dict/DictBuilder.hpp"

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: %s <sd-root> <source> <work-dir>\n", argv[0]);
        return 2;
    }
    setenv("JISHO_SD", argv[1], 1);
    build::DictBuilder b;
    build::DictBuilder::Options opt;
    opt.source = argv[2];
    opt.workDir = argv[3];
    opt.swap = false;
    if (const char* ram = std::getenv("JISHO_BUILD_RAM_KB")) opt.ramBytes = std::atol(ram) * 1024;
    if (!b.start(opt)) { std::fprintf(stderr, "start failed: %s\n", b.error().c_str()); return 1; }
    int lastPct = -1;
    while (b.running()) {
        b.step(30);
        const int pct = static_cast<int>(b.progress() * 100);
        if (pct / 10 != lastPct / 10) { std::printf("  %3d%%  %s\n", pct, b.phaseName()); lastPct = pct; }
    }
    if (b.phase() != build::DictBuilder::Phase::Done) {
        std::fprintf(stderr, "build failed: %s\n", b.error().c_str());
        return 1;
    }
    const hal::IoStats io = hal::ioStats();
    std::printf("%s\nSD traffic: %u reads %.1f MB, %u writes %.1f MB\n", b.report().c_str(),
                static_cast<unsigned>(io.reads), io.bytes / 1e6, static_cast<unsigned>(io.writes),
                io.bytesWritten / 1e6);
    return 0;
}
