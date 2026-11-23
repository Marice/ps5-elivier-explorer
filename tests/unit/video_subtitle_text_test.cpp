// EliVier Explorer - Text subtitles: encodings, SRT, WebVTT, ASS, markup and the cue timeline.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video/subtitle_text.hpp"

#include <gtest/gtest.h>

#include <string>

namespace
{

using namespace hui::video;

// ---- encodings -------------------------------------------------------------------------------

TEST(SubtitleEncoding, Utf8PassesThroughAndALeadingBomIsDropped)
{
    EXPECT_EQ(to_utf8("Zo\xC3\xAB caf\xC3\xA9"), "Zo\xC3\xAB caf\xC3\xA9");
    EXPECT_EQ(to_utf8("\xEF\xBB\xBFHello"), "Hello");
}

TEST(SubtitleEncoding, Utf16WithABomBecomesUtf8)
{
    // "Zoë €" in UTF-16 little endian, then big endian, both with a byte order mark.
    const std::string little("\xFF\xFE" "Z\0o\0\xEB\0 \0\xAC\x20", 12);
    const std::string big("\xFE\xFF" "\0Z\0o\0\xEB\0 \x20\xAC", 12);
    EXPECT_EQ(to_utf8(little), "Zo\xC3\xAB \xE2\x82\xAC");
    EXPECT_EQ(to_utf8(big), "Zo\xC3\xAB \xE2\x82\xAC");
}

TEST(SubtitleEncoding, Utf16SurrogatePairsBecomeOneCharacter)
{
    // U+1F600 is D83D DE00.
    const std::string little("\xFF\xFE\x3D\xD8\x00\xDE", 6);
    EXPECT_EQ(to_utf8(little), "\xF0\x9F\x98\x80");
}

TEST(SubtitleEncoding, BytesThatAreNotUtf8AreReadAsWindows1252)
{
    // "café" with a lone 0xE9, a euro sign (0x80) and a curly apostrophe (0x92).
    EXPECT_EQ(to_utf8("caf\xE9"), "caf\xC3\xA9");
    EXPECT_EQ(to_utf8("5\x80 it\x92s"), "5\xE2\x82\xAC it\xE2\x80\x99s");
}

TEST(SubtitleEncoding, AHalfWrittenCharacterIsNotValidUtf8)
{
    EXPECT_EQ(to_utf8("abc\xC3"), "abc\xC3\x83"); // read as Windows-1252: A with a tilde
}

// ---- markup ----------------------------------------------------------------------------------

TEST(SubtitleMarkup, TagsAreStrippedButALessThanSignIsKept)
{
    EXPECT_EQ(strip_markup("<i>Hello</i> <b>there</b> <font color=\"#fff\">you</font>"),
              "Hello there you");
    EXPECT_EQ(strip_markup("I <3 you, 2 < 3"), "I <3 you, 2 < 3");
}

TEST(SubtitleMarkup, EntitiesAreDecoded)
{
    EXPECT_EQ(strip_markup("Tom &amp; Jerry &lt;3&gt; &quot;hi&quot;&nbsp;there &#233; &#x20AC;"),
              "Tom & Jerry <3> \"hi\" there \xC3\xA9 \xE2\x82\xAC");
}

TEST(SubtitleMarkup, AssOverridesAreStrippedAndTheAlignmentIsRead)
{
    CueAlign align = CueAlign::bottom;
    EXPECT_EQ(strip_markup("{\\an8}On top", &align), "On top");
    EXPECT_EQ(align, CueAlign::top);
    EXPECT_EQ(strip_markup("{\\an5}Middle", &align), "Middle");
    EXPECT_EQ(align, CueAlign::middle);
    EXPECT_EQ(strip_markup("{\\an2}Bottom", &align), "Bottom");
    EXPECT_EQ(align, CueAlign::bottom);
    EXPECT_EQ(strip_markup("a {\\i1}b{\\i0} c"), "a b c");
}

TEST(SubtitleMarkup, TextInBracesWithoutABackslashStays)
{
    EXPECT_EQ(strip_markup("{music playing}"), "{music playing}");
}

// ---- SRT -------------------------------------------------------------------------------------

TEST(Srt, ReadsCuesTextAndTimes)
{
    const SubtitleTrack track = parse_srt(
        "1\n00:00:01,000 --> 00:00:03,500\nHello there.\n\n"
        "2\n00:01:02,250 --> 00:01:04,000\nTwo lines\nin one cue.\n");
    ASSERT_EQ(track.size(), 2u);
    EXPECT_DOUBLE_EQ(track.cues()[0].start, 1.0);
    EXPECT_DOUBLE_EQ(track.cues()[0].end, 3.5);
    EXPECT_EQ(track.cues()[0].text, "Hello there.");
    EXPECT_DOUBLE_EQ(track.cues()[1].start, 62.25);
    EXPECT_EQ(track.cues()[1].text, "Two lines\nin one cue.");
}

TEST(Srt, ToleratesWindowsLineEndingsMissingNumbersAndDotsInTimes)
{
    const SubtitleTrack track = parse_srt("\xEF\xBB\xBF"
                                          "00:00:01.5 --> 00:00:02.25\r\nNo number, dots.\r\n\r\n"
                                          "7\r\n1:02:03,4 --> 1:02:04,0\r\nShort fields.\r\n");
    ASSERT_EQ(track.size(), 2u);
    EXPECT_DOUBLE_EQ(track.cues()[0].start, 1.5);
    EXPECT_EQ(track.cues()[0].text, "No number, dots.");
    EXPECT_DOUBLE_EQ(track.cues()[1].start, 3723.4);
}

TEST(Srt, SkipsCuesWithoutTextOrWithAnEndBeforeTheStart)
{
    const SubtitleTrack track = parse_srt("1\n00:00:01,000 --> 00:00:02,000\n\n\n"
                                          "2\n00:00:05,000 --> 00:00:04,000\nBackwards.\n\n"
                                          "3\n00:00:06,000 --> 00:00:07,000\nKept.\n");
    ASSERT_EQ(track.size(), 1u);
    EXPECT_EQ(track.cues()[0].text, "Kept.");
}

TEST(Srt, KeepsCuesInTimeOrderEvenWhenTheFileIsNot)
{
    const SubtitleTrack track = parse_srt("1\n00:00:09,000 --> 00:00:10,000\nLate\n\n"
                                          "2\n00:00:01,000 --> 00:00:02,000\nEarly\n");
    ASSERT_EQ(track.size(), 2u);
    EXPECT_EQ(track.cues()[0].text, "Early");
    EXPECT_EQ(track.cues()[1].text, "Late");
}

TEST(Srt, GarbageGivesAnEmptyTrackNotACrash)
{
    EXPECT_EQ(parse_srt("").size(), 0u);
    EXPECT_EQ(parse_srt("--> --> -->\n\n\n12:::::,,,\n").size(), 0u);
    EXPECT_EQ(parse_srt(std::string(5000, '\0')).size(), 0u);
}

TEST(Srt, ALongCueIsCutOnACharacterBoundary)
{
    std::string text;
    for (int i = 0; i < 3000; ++i)
        text += "\xC3\xA9"; // 6000 bytes of "é"
    const SubtitleTrack track = parse_srt("1\n00:00:01,000 --> 00:00:02,000\n" + text + "\n");
    ASSERT_EQ(track.size(), 1u);
    const std::string &kept = track.cues()[0].text;
    EXPECT_LE(kept.size(), 4096u);
    EXPECT_EQ(kept.size() % 2, 0u); // never half an "é"
}

// ---- WebVTT ----------------------------------------------------------------------------------

TEST(WebVtt, ReadsCuesWithIdsSettingsAndShortTimes)
{
    const SubtitleTrack track = parse_webvtt(
        "WEBVTT - a test\n\n"
        "NOTE this block is ignored\nand so is this line\n\n"
        "intro\n00:01.000 --> 00:03.000 align:middle line:10%\nTop line\n\n"
        "01:00:00.500 --> 01:00:01.000\n<v Anna>Hello</v> <c.yellow>there</c>\n");
    ASSERT_EQ(track.size(), 2u);
    EXPECT_DOUBLE_EQ(track.cues()[0].start, 1.0);
    EXPECT_DOUBLE_EQ(track.cues()[0].end, 3.0);
    EXPECT_EQ(track.cues()[0].text, "Top line");
    EXPECT_EQ(track.cues()[0].align, CueAlign::top); // line:10% is near the top
    EXPECT_DOUBLE_EQ(track.cues()[1].start, 3600.5);
    EXPECT_EQ(track.cues()[1].text, "Hello there");
    EXPECT_EQ(track.cues()[1].align, CueAlign::bottom);
}

TEST(WebVtt, KaraokeTimestampsInsideACueAreDropped)
{
    const SubtitleTrack track =
        parse_webvtt("WEBVTT\n\n00:00.000 --> 00:02.000\nOne <00:00:01.000>two\n");
    ASSERT_EQ(track.size(), 1u);
    EXPECT_EQ(track.cues()[0].text, "One two");
}

TEST(WebVtt, AFileWithoutTheWebVttHeaderIsNotRecognisedAsOne)
{
    SubtitleTrack track;
    std::string error;
    // load_subtitle_file decides by content: with no header this is read as SRT, and a time
    // without hours is not an SRT time, so nothing is found.
    EXPECT_FALSE(load_subtitle_file("00:00.000 --> 00:02.000\nNo header\n", track, &error));
    EXPECT_FALSE(error.empty());
}

// ---- ASS -------------------------------------------------------------------------------------

TEST(Ass, ReadsDialogueLinesWhateverTheFieldOrder)
{
    const SubtitleTrack track = parse_ass(
        "[Script Info]\nTitle: x\n\n[V4+ Styles]\nFormat: Name\nStyle: Default\n\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:01.00,0:00:02.50,Default,,0,0,0,,{\\i1}Hello{\\i0}, world\\Nsecond\n"
        "Comment: 0,0:00:03.00,0:00:04.00,Default,,0,0,0,,not shown\n"
        "Dialogue: 0,1:02:03.45,1:02:04.00,Default,,0,0,0,,{\\an8}Up, here\n");
    ASSERT_EQ(track.size(), 2u);
    EXPECT_DOUBLE_EQ(track.cues()[0].start, 1.0);
    EXPECT_DOUBLE_EQ(track.cues()[0].end, 2.5);
    EXPECT_EQ(track.cues()[0].text, "Hello, world\nsecond"); // the commas of the text survive
    EXPECT_NEAR(track.cues()[1].start, 3723.45, 1e-9);
    EXPECT_EQ(track.cues()[1].text, "Up, here");
    EXPECT_EQ(track.cues()[1].align, CueAlign::top);
}

TEST(Ass, ADrawingIsNotText)
{
    const SubtitleTrack track = parse_ass(
        "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, "
        "Text\n"
        "Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,{\\p1}m 0 0 l 100 0 100 100{\\p0}\n");
    EXPECT_EQ(track.size(), 0u);
}

// ---- the timeline ----------------------------------------------------------------------------

Cue cue(double start, double end, const char *text)
{
    Cue result;
    result.start = start;
    result.end = end;
    result.text = text;
    return result;
}

TEST(SubtitleTrack, StartIsInclusiveAndEndIsExclusive)
{
    SubtitleTrack track;
    track.add(cue(1.0, 2.0, "a"));
    EXPECT_TRUE(track.active_at(0.999).empty());
    ASSERT_EQ(track.active_at(1.0).size(), 1u);
    ASSERT_EQ(track.active_at(1.999).size(), 1u);
    EXPECT_TRUE(track.active_at(2.0).empty());
}

TEST(SubtitleTrack, OverlappingCuesAreAllActiveInOrderOfStart)
{
    SubtitleTrack track;
    track.add(cue(0.0, 10.0, "long"));
    track.add(cue(2.0, 3.0, "short"));
    track.add(cue(2.5, 6.0, "middle"));
    const auto active = track.active_at(2.7);
    ASSERT_EQ(active.size(), 3u);
    EXPECT_EQ(active[0]->text, "long");
    EXPECT_EQ(active[1]->text, "short");
    EXPECT_EQ(active[2]->text, "middle");
    EXPECT_EQ(track.active_at(7.0).size(), 1u);
}

TEST(SubtitleTrack, AnIdenticalCueIsKeptOnlyOnce)
{
    // Seeking back makes the demuxer deliver the same subtitle packet again.
    SubtitleTrack track;
    EXPECT_TRUE(track.add(cue(1.0, 2.0, "same")));
    EXPECT_FALSE(track.add(cue(1.0, 2.0, "same")));
    EXPECT_TRUE(track.add(cue(1.0, 2.0, "other")));
    EXPECT_EQ(track.size(), 2u);
}

TEST(SubtitleTrack, ACueLongerThanTenMinutesIsCutToTenMinutes)
{
    // A damaged file must not make every lookup walk the whole track.
    SubtitleTrack track;
    track.add(cue(10.0, 1.0e7, "stuck"));
    track.add(cue(5000.0, 5001.0, "later"));
    ASSERT_EQ(track.size(), 2u);
    EXPECT_DOUBLE_EQ(track.cues()[0].end, 610.0);
    EXPECT_TRUE(track.active_at(700.0).empty());
    EXPECT_EQ(track.active_at(300.0).size(), 1u);
}

TEST(SubtitleTrack, ClearEmptiesIt)
{
    SubtitleTrack track;
    track.add(cue(1.0, 2.0, "a"));
    track.clear();
    EXPECT_EQ(track.size(), 0u);
    EXPECT_TRUE(track.active_at(1.5).empty());
}

TEST(SubtitleTrack, RefusesCuesBeyondTheLimit)
{
    SubtitleTrack track;
    for (int i = 0; i < kMaxCues + 10; ++i)
        track.add(cue(i, i + 0.5, "x"));
    EXPECT_EQ(track.size(), static_cast<std::size_t>(kMaxCues));
}

// ---- loading a file by content ---------------------------------------------------------------

TEST(LoadSubtitleFile, RecognisesEachFormatByItsContent)
{
    SubtitleTrack srt;
    SubtitleTrack vtt;
    SubtitleTrack ass;
    std::string error;
    EXPECT_TRUE(load_subtitle_file("1\n00:00:01,000 --> 00:00:02,000\nSRT\n", srt, &error));
    EXPECT_TRUE(load_subtitle_file("WEBVTT\n\n00:01.000 --> 00:02.000\nVTT\n", vtt, &error));
    EXPECT_TRUE(load_subtitle_file(
        "[Script Info]\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, "
        "MarginV, Effect, Text\nDialogue: 0,0:00:01.00,0:00:02.00,D,,0,0,0,,ASS\n",
        ass, &error));
    EXPECT_EQ(srt.cues()[0].text, "SRT");
    EXPECT_EQ(vtt.cues()[0].text, "VTT");
    EXPECT_EQ(ass.cues()[0].text, "ASS");
}

TEST(LoadSubtitleFile, RefusesAnEmptyOrTooLargeFile)
{
    SubtitleTrack track;
    std::string error;
    EXPECT_FALSE(load_subtitle_file("", track, &error));
    EXPECT_FALSE(error.empty());
    error.clear();
    EXPECT_FALSE(load_subtitle_file(std::string(kMaxSubtitleFileBytes + 1, 'a'), track, &error));
    EXPECT_FALSE(error.empty());
}

TEST(LoadSubtitleFile, ReadsAnSrtInWindows1252)
{
    SubtitleTrack track;
    std::string error;
    ASSERT_TRUE(load_subtitle_file("1\r\n00:00:01,000 --> 00:00:02,000\r\nCaf\xE9 \x80 5\r\n",
                                   track, &error));
    EXPECT_EQ(track.cues()[0].text, "Caf\xC3\xA9 \xE2\x82\xAC 5");
}

} // namespace
