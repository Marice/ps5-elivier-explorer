// EliVier Explorer - Console probes: the plan, the report, timing statistics and memory patterns.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "probe/plan.hpp"
#include "probe/report.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace
{

using namespace hui::probe;

// ---- the plan --------------------------------------------------------------------------------

TEST(ProbePlan, SafeMeansTheFourThatOnlyReadAndMeasure)
{
    ProbePlan plan;
    std::string error;
    ASSERT_TRUE(parse_probe_plan("safe", plan, &error)) << error;
    EXPECT_TRUE(plan.memory);
    EXPECT_TRUE(plan.upload);
    EXPECT_TRUE(plan.egl);
    EXPECT_TRUE(plan.decoder_load);
    EXPECT_EQ(plan.hdr, HdrMode::none);
}

TEST(ProbePlan, SingleProbesAndCombinationsWithAPlus)
{
    ProbePlan plan;
    std::string error;
    ASSERT_TRUE(parse_probe_plan("memory", plan, &error)) << error;
    EXPECT_TRUE(plan.memory);
    EXPECT_FALSE(plan.upload);
    EXPECT_FALSE(plan.egl);

    plan = ProbePlan{};
    ASSERT_TRUE(parse_probe_plan("upload+egl", plan, &error)) << error;
    EXPECT_FALSE(plan.memory);
    EXPECT_TRUE(plan.upload);
    EXPECT_TRUE(plan.egl);
}

TEST(ProbePlan, AnHdrAttemptRunsAloneBecauseItEndsTheLaunch)
{
    ProbePlan plan;
    std::string error;
    ASSERT_TRUE(parse_probe_plan("hdr:pq10", plan, &error)) << error;
    EXPECT_EQ(plan.hdr, HdrMode::pq10);
    EXPECT_FALSE(plan.memory);

    plan = ProbePlan{};
    ASSERT_TRUE(parse_probe_plan("hdr:hlg10", plan, &error));
    EXPECT_EQ(plan.hdr, HdrMode::hlg10);
    plan = ProbePlan{};
    ASSERT_TRUE(parse_probe_plan("hdr:fp16", plan, &error));
    EXPECT_EQ(plan.hdr, HdrMode::fp16);

    plan = ProbePlan{};
    EXPECT_FALSE(parse_probe_plan("memory+hdr:pq10", plan, &error));
    EXPECT_NE(error.find("alone"), std::string::npos) << error;
    EXPECT_FALSE(parse_probe_plan("safe+hdr:pq10", plan, &error));
}

TEST(ProbePlan, TheDecoderBindRunsAloneAndIsNotPartOfSafe)
{
    ProbePlan plan;
    std::string error;
    ASSERT_TRUE(parse_probe_plan("decoder-bind", plan, &error)) << error;
    EXPECT_TRUE(plan.decoder_bind);
    EXPECT_FALSE(plan.memory);
    EXPECT_EQ(describe(plan), "decoder-bind");

    ASSERT_TRUE(parse_probe_plan("safe", plan, &error));
    EXPECT_FALSE(plan.decoder_bind); // `safe` only reads and measures

    EXPECT_FALSE(parse_probe_plan("memory+decoder-bind", plan, &error));
    EXPECT_NE(error.find("alone"), std::string::npos) << error;
    EXPECT_FALSE(parse_probe_plan("decoder-bind+decoder-load", plan, &error));
}

TEST(ProbePlan, RefusesWhatItDoesNotKnow)
{
    ProbePlan plan;
    std::string error;
    EXPECT_FALSE(parse_probe_plan("", plan, &error));
    EXPECT_FALSE(error.empty());
    for (const char *name : {"bogus", "memory+", "+memory", "hdr:", "hdr:nope", "decoder-query",
                             "memory memory ", "../etc", "MEMORY"})
    {
        error.clear();
        EXPECT_FALSE(parse_probe_plan(name, plan, &error)) << name;
        EXPECT_FALSE(error.empty()) << name;
    }
    EXPECT_FALSE(parse_probe_plan(std::string(200, 'a'), plan, &error));
}

TEST(ProbePlan, TheSameProbeTwiceIsFine)
{
    ProbePlan plan;
    std::string error;
    EXPECT_TRUE(parse_probe_plan("memory+memory", plan, &error)) << error;
    EXPECT_TRUE(plan.memory);
}

TEST(ProbePlan, ADescriptionNamesWhatWillRun)
{
    ProbePlan plan;
    std::string error;
    ASSERT_TRUE(parse_probe_plan("memory+upload", plan, &error));
    EXPECT_EQ(describe(plan), "memory+upload");
    ASSERT_TRUE(parse_probe_plan("hdr:fp16", plan = ProbePlan{}, &error));
    EXPECT_EQ(describe(plan), "hdr:fp16");
}

// ---- the report ------------------------------------------------------------------------------

TEST(ProbeReport, OneLinePerValueInAFixedShape)
{
    Report report;
    report.add("memory", "direct_total_mib", std::int64_t{12288});
    report.add("memory", "label", "type 12");
    report.add_real("upload", "avg_ms", 3.14159, 2);
    report.note("egl", "no HDR surface");
    ASSERT_EQ(report.lines().size(), 4u);
    EXPECT_EQ(report.lines()[0], "probe memory direct_total_mib=12288");
    EXPECT_EQ(report.lines()[1], "probe memory label=type 12");
    EXPECT_EQ(report.lines()[2], "probe upload avg_ms=3.14");
    EXPECT_EQ(report.lines()[3], "probe egl note=no HDR surface");
    EXPECT_EQ(report.text(), "probe memory direct_total_mib=12288\nprobe memory label=type 12\n"
                             "probe upload avg_ms=3.14\nprobe egl note=no HDR surface\n");
}

TEST(ProbeReport, KeysAndValuesCannotBreakTheLineFormat)
{
    Report report;
    report.add("a b", "k=ey\n", "line one\nline two\r\ttab");
    ASSERT_EQ(report.lines().size(), 1u);
    EXPECT_EQ(report.lines()[0].find('\n'), std::string::npos);
    EXPECT_EQ(report.lines()[0].find('\r'), std::string::npos);
    EXPECT_EQ(report.lines()[0], "probe a_b k_ey_=line one line two  tab");
}

TEST(ProbeReport, ALongValueIsCut)
{
    Report report;
    report.add("egl", "extensions", std::string(5000, 'x'));
    EXPECT_LE(report.lines()[0].size(), 300u);
}

TEST(ProbeReport, NegativeAndHexValues)
{
    Report report;
    report.add("decoder", "rc", std::int64_t{-5});
    report.add_hex("decoder", "status", 0x811d0200u);
    EXPECT_EQ(report.lines()[0], "probe decoder rc=-5");
    EXPECT_EQ(report.lines()[1], "probe decoder status=0x811d0200");
}

// ---- timing ----------------------------------------------------------------------------------

TEST(TimingStats, AverageMinimumMaximumAndPercentile)
{
    TimingStats stats;
    for (int i = 1; i <= 100; ++i)
        stats.add(i);
    EXPECT_EQ(stats.count(), 100u);
    EXPECT_DOUBLE_EQ(stats.average(), 50.5);
    EXPECT_DOUBLE_EQ(stats.minimum(), 1.0);
    EXPECT_DOUBLE_EQ(stats.maximum(), 100.0);
    EXPECT_DOUBLE_EQ(stats.percentile(0.95), 95.0);
    EXPECT_DOUBLE_EQ(stats.percentile(0.5), 50.0);
    EXPECT_DOUBLE_EQ(stats.percentile(1.0), 100.0);
}

TEST(TimingStats, EmptyGivesZeroNotACrash)
{
    TimingStats stats;
    EXPECT_EQ(stats.count(), 0u);
    EXPECT_DOUBLE_EQ(stats.average(), 0.0);
    EXPECT_DOUBLE_EQ(stats.percentile(0.95), 0.0);
    EXPECT_DOUBLE_EQ(stats.maximum(), 0.0);
}

TEST(TimingStats, ThroughputInMibPerSecond)
{
    EXPECT_DOUBLE_EQ(mib_per_second(64u * 1024u * 1024u, 0.5), 128.0);
    EXPECT_DOUBLE_EQ(mib_per_second(1024, 0.0), 0.0); // no division by zero
}

// ---- memory patterns -------------------------------------------------------------------------

TEST(MemoryPattern, FillThenVerifyFindsNothingWrong)
{
    std::vector<std::uint8_t> block(4096 + 3); // not a multiple of four on purpose
    fill_pattern(block.data(), block.size(), 7);
    EXPECT_EQ(find_mismatch(block.data(), block.size(), 7), block.size());
}

TEST(MemoryPattern, ASingleFlippedByteIsFoundAtItsPlace)
{
    std::vector<std::uint8_t> block(10000);
    fill_pattern(block.data(), block.size(), 99);
    block[7777] ^= 0x10;
    EXPECT_EQ(find_mismatch(block.data(), block.size(), 99), 7777u);
    block[7777] ^= 0x10;
    block[block.size() - 1] ^= 1; // the tail is checked too
    EXPECT_EQ(find_mismatch(block.data(), block.size(), 99), block.size() - 1);
}

TEST(MemoryPattern, DifferentSeedsGiveDifferentContent)
{
    std::vector<std::uint8_t> a(512);
    std::vector<std::uint8_t> b(512);
    fill_pattern(a.data(), a.size(), 1);
    fill_pattern(b.data(), b.size(), 2);
    EXPECT_NE(a, b);
    EXPECT_LT(find_mismatch(a.data(), a.size(), 2), a.size());
}

TEST(MemoryPattern, ALadderDoublesFromSixteenMibUpToTheLimit)
{
    const std::vector<std::size_t> ladder = allocation_ladder(512u * 1024u * 1024u);
    const std::vector<std::size_t> expected = {16u << 20, 32u << 20, 64u << 20,
                                               128u << 20, 256u << 20, 512u << 20};
    EXPECT_EQ(ladder, expected);
    EXPECT_TRUE(allocation_ladder(8u << 20).empty());
    const std::vector<std::size_t> odd = allocation_ladder(100u << 20);
    ASSERT_FALSE(odd.empty());
    EXPECT_EQ(odd.back(), 64u << 20); // only what fits
}

} // namespace
