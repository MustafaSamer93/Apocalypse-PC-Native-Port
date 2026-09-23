// Known-answer tests for the GTE. Build: see CMake target `gte_test`.
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "psx.h"

// Minimal stubs for what gte.cpp pulls in from the runtime.
bool log_enabled(LogCat) { return false; }
void log_msg(LogCat, const char*, ...) {}
uint64_t host_frame_count() { return 0; }
void trace(const char*, ...) {}

static int g_fail = 0;
#define EXPECT_EQ(a, b)                                                                                  \
    do {                                                                                                 \
        long long _a = (long long)(a), _b = (long long)(b);                                              \
        if (_a != _b) {                                                                                  \
            std::printf("FAIL %s:%d: %s = %lld (0x%llX), expected %lld (0x%llX)\n", __FILE__, __LINE__, #a, \
                        _a, (unsigned long long)_a, _b, (unsigned long long)_b);                         \
            g_fail++;                                                                                    \
        }                                                                                                \
    } while (0)

static void identity_setup(R3000& c) {
    std::memset(&c, 0, sizeof c);
    gte_write_ctrl(&c, 0, 0x00001000);  // RT11=1.0 RT12=0
    gte_write_ctrl(&c, 1, 0x00000000);  // RT13=0  RT21=0
    gte_write_ctrl(&c, 2, 0x00001000);  // RT22=1.0 RT23=0
    gte_write_ctrl(&c, 3, 0x00000000);  // RT31=0 RT32=0
    gte_write_ctrl(&c, 4, 0x00001000);  // RT33=1.0
    gte_write_ctrl(&c, 5, 0);           // TRX
    gte_write_ctrl(&c, 6, 0);           // TRY
    gte_write_ctrl(&c, 7, 1000);        // TRZ
    gte_write_ctrl(&c, 24, 160 << 16);  // OFX
    gte_write_ctrl(&c, 25, 120 << 16);  // OFY
    gte_write_ctrl(&c, 26, 1000);       // H
}

static void test_rtps() {
    R3000 c;
    identity_setup(c);
    gte_write_data(&c, 0, (50u << 16) | 100u);  // VX0=100 VY0=50
    gte_write_data(&c, 1, 0);                    // VZ0=0
    gte_command(&c, 0x0080001);                  // RTPS, sf=1
    EXPECT_EQ(gte_read_data(&c, 25), 100);       // MAC1
    EXPECT_EQ(gte_read_data(&c, 26), 50);
    EXPECT_EQ(gte_read_data(&c, 27), 1000);
    EXPECT_EQ(gte_read_data(&c, 19), 1000);      // SZ3
    uint32_t sxy = gte_read_data(&c, 14);
    EXPECT_EQ((int16_t)(sxy & 0xFFFF), 260);
    EXPECT_EQ((int16_t)(sxy >> 16), 170);
    EXPECT_EQ(gte_read_ctrl(&c, 31), 0);         // FLAG
}

static void test_rtpt_nclip() {
    R3000 c;
    identity_setup(c);
    // triangle (0,0,0) (100,0,0) (0,100,0): clockwise on screen (y down) -> positive area
    gte_write_data(&c, 0, 0);
    gte_write_data(&c, 1, 0);
    gte_write_data(&c, 2, 100);
    gte_write_data(&c, 3, 0);
    gte_write_data(&c, 4, 100u << 16);
    gte_write_data(&c, 5, 0);
    gte_command(&c, 0x0080030);  // RTPT sf=1
    EXPECT_EQ(gte_read_ctrl(&c, 31), 0);
    uint32_t s0 = gte_read_data(&c, 12), s1 = gte_read_data(&c, 13), s2 = gte_read_data(&c, 14);
    EXPECT_EQ((int16_t)s0, 160); EXPECT_EQ((int16_t)(s0 >> 16), 120);
    EXPECT_EQ((int16_t)s1, 260); EXPECT_EQ((int16_t)(s1 >> 16), 120);
    EXPECT_EQ((int16_t)s2, 160); EXPECT_EQ((int16_t)(s2 >> 16), 220);
    gte_command(&c, 0x1400006);  // NCLIP
    // SX0*SY1 + SX1*SY2 + SX2*SY0 - SX0*SY2 - SX1*SY0 - SX2*SY1
    long long e = 160LL * 120 + 260LL * 220 + 160LL * 120 - 160LL * 220 - 260LL * 120 - 160LL * 120;
    EXPECT_EQ((int32_t)gte_read_data(&c, 24), e);  // MAC0 = +10000
    EXPECT_EQ(gte_read_ctrl(&c, 31), 0);
}

static void test_mvmva() {
    R3000 c;
    identity_setup(c);
    gte_write_data(&c, 9, 300);   // IR1
    gte_write_data(&c, 10, -200); // IR2
    gte_write_data(&c, 11, 50);   // IR3
    // MVMVA sf=1 mx=RT v=IR cv=TR(0,0,1000)
    gte_command(&c, 0x12u | (1u << 19) | (0u << 17) | (3u << 15) | (0u << 13));
    EXPECT_EQ((int32_t)gte_read_data(&c, 25), 300);
    EXPECT_EQ((int32_t)gte_read_data(&c, 26), -200);
    EXPECT_EQ((int32_t)gte_read_data(&c, 27), 1050);
    EXPECT_EQ((int32_t)gte_read_data(&c, 9), 300);
    EXPECT_EQ((int32_t)gte_read_data(&c, 10), -200);
}

static void test_avsz3_regs() {
    R3000 c;
    identity_setup(c);
    gte_write_ctrl(&c, 29, 0x155);  // ZSF3 ~ 4096/12
    gte_write_data(&c, 17, 1000);
    gte_write_data(&c, 18, 2000);
    gte_write_data(&c, 19, 3000);
    gte_command(&c, 0x158002D);
    EXPECT_EQ(gte_read_data(&c, 7), (0x155 * 6000) >> 12);  // OTZ
    // LZCS/LZCR
    gte_write_data(&c, 30, 0x00FFFFFF);
    EXPECT_EQ(gte_read_data(&c, 31), 8);
    gte_write_data(&c, 30, 0xFFF00000);
    EXPECT_EQ(gte_read_data(&c, 31), 12);
    // IRGB / ORGB
    gte_write_data(&c, 28, 0x7FFF);
    EXPECT_EQ(gte_read_data(&c, 9), 0xF80);
    EXPECT_EQ(gte_read_data(&c, 29), 0x7FFF);
}

// Regression: OP with sf=0 saturates IR1 after MAC1; MAC2/MAC3 must still use the old IR1.
// (Values from Apocalypse's camera setup, which broke all 3D rendering.)
static void test_op_sf0() {
    R3000 c;
    std::memset(&c, 0, sizeof c);
    gte_write_ctrl(&c, 0, 0);       // D1 = RT11 = 0
    gte_write_ctrl(&c, 2, 0xFC6);   // D2 = RT22 = 4038
    gte_write_ctrl(&c, 4, 0x2E4);   // D3 = RT33 = 740
    gte_write_data(&c, 9, 0);
    gte_write_data(&c, 10, (uint32_t)-731);
    gte_write_data(&c, 11, 4055);
    gte_command(&c, 0x170000C);     // OP, sf=0
    EXPECT_EQ((int32_t)gte_read_data(&c, 25), 4055 * 4038 + 731 * 740);
    EXPECT_EQ((int32_t)gte_read_data(&c, 26), 0);
    EXPECT_EQ((int32_t)gte_read_data(&c, 27), 0);
    EXPECT_EQ((int32_t)gte_read_data(&c, 9), 0x7FFF);  // IR1 saturated
}

int main() {
    test_op_sf0();
    test_rtps();
    test_rtpt_nclip();
    test_mvmva();
    test_avsz3_regs();
    std::printf(g_fail ? "%d failure(s)\n" : "all GTE tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
