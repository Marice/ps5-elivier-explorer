// EliVier Explorer - Text as lines and columns: splitting, tabs, odd bytes, encodings.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "viewer/text_document.hpp"

#include <gtest/gtest.h>

#include <string>

namespace
{

using hui::viewer::looks_like_text;
using hui::viewer::TextDocument;
using hui::viewer::TextEncoding;

TextDocument doc(const std::string &bytes, bool truncated = false)
{
    TextDocument document;
    document.load(bytes, truncated);
    return document;
}

TEST(TextDocument, SplitsLinesOnLfCrLfAndLoneCr)
{
    const TextDocument d = doc("one\ntwo\r\nthree\rfour");
    ASSERT_EQ(d.line_count(), 4);
    EXPECT_EQ(d.display_line(0), "one");
    EXPECT_EQ(d.display_line(1), "two");
    EXPECT_EQ(d.display_line(2), "three");
    EXPECT_EQ(d.display_line(3), "four");
}

TEST(TextDocument, ATrailingNewlineIsNotAnExtraLineButABlankLineIs)
{
    EXPECT_EQ(doc("a\nb\n").line_count(), 2);
    EXPECT_EQ(doc("a\n\n").line_count(), 2); // the second is blank
    EXPECT_EQ(doc("a\n\n").display_line(1), "");
    EXPECT_EQ(doc("\n").line_count(), 1);
    EXPECT_EQ(doc("").line_count(), 0);
    EXPECT_EQ(doc("no newline at all").line_count(), 1);
}

TEST(TextDocument, ALineThatDoesNotExistIsEmpty)
{
    const TextDocument d = doc("a\nb");
    EXPECT_EQ(d.display_line(-1), "");
    EXPECT_EQ(d.display_line(2), "");
    EXPECT_EQ(d.columns_of(5), 0);
}

TEST(TextDocument, TabsGoToTheNextStop)
{
    const TextDocument d = doc("a\tb\n\t\tc\nabcd\te");
    EXPECT_EQ(d.display_line(0), "a   b");     // a, then to column 4
    EXPECT_EQ(d.display_line(1), "        c"); // two stops
    EXPECT_EQ(d.display_line(2), "abcd    e"); // a full stop when already on one
}

TEST(TextDocument, ControlCharactersAreDotsAndBadBytesAreReplacements)
{
    const std::string text =
        std::string("a") + '\x01' + "b" + '\x7f' + "c" + '\xff' + "d" + '\xC3' + "e";
    const TextDocument d = doc(text);
    EXPECT_EQ(d.display_line(0), "a\xC2\xB7"
                                 "b\xC2\xB7"
                                 "c\xEF\xBF\xBD"
                                 "d\xEF\xBF\xBD"
                                 "e");
    EXPECT_EQ(d.columns_of(0), 9);
}

TEST(TextDocument, ANulByteIsADotToo)
{
    const TextDocument d = doc(std::string("a\0b", 3));
    EXPECT_EQ(d.display_line(0), "a\xC2\xB7"
                                 "b");
}

TEST(TextDocument, OverlongSurrogateAndTooLargeSequencesAreRejected)
{
    EXPECT_EQ(doc("\xC0\x80").display_line(0), "\xEF\xBF\xBD\xEF\xBF\xBD"); // overlong NUL
    EXPECT_EQ(doc("\xED\xA0\x80").display_line(0),
              "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");              // a surrogate
    EXPECT_EQ(doc("\xF4\x90\x80\x80").display_line(0).size(), 12u); // past U+10FFFF
}

TEST(TextDocument, ValidMultibyteTextPassesThroughAsOneColumnEach)
{
    const TextDocument d = doc("caf\xC3\xA9 \xE2\x94\x80\xE2\x94\x80 \xF0\x9F\x98\x80");
    EXPECT_EQ(d.columns_of(0), 9); // c a f é space ─ ─ space emoji
    EXPECT_EQ(d.display_line(0), "caf\xC3\xA9 \xE2\x94\x80\xE2\x94\x80 \xF0\x9F\x98\x80");
}

TEST(TextDocument, SlicesByColumnNotByByte)
{
    const TextDocument d = doc("h\xC3\xA9llo w\xC3\xB6rld");
    EXPECT_EQ(d.display_slice(0, 0, 3), "h\xC3\xA9l");
    EXPECT_EQ(d.display_slice(0, 1, 3), "\xC3\xA9ll");
    EXPECT_EQ(d.display_slice(0, 6, 5), "w\xC3\xB6rld");
    EXPECT_EQ(d.display_slice(0, 50, 5), ""); // past the end of the line
    EXPECT_EQ(d.display_slice(0, 0, 0), "");
}

TEST(TextDocument, ASliceInsideAnExpandedTabIsSpaces)
{
    const TextDocument d = doc("\tx");
    EXPECT_EQ(d.display_slice(0, 2, 3), "  x"); // columns 2 and 3 are the tab's, 4 is x
}

TEST(TextDocument, TheWidestLineSetsMaxColumns)
{
    EXPECT_EQ(doc("ab\nabcdef\nabc").max_columns(), 6);
    EXPECT_EQ(doc("\t\t").max_columns(), 8);
    EXPECT_EQ(doc("").max_columns(), 0);
}

TEST(TextDocument, AUtf8ByteOrderMarkIsNotText)
{
    const TextDocument d = doc("\xEF\xBB\xBFhello");
    EXPECT_EQ(d.encoding(), TextEncoding::utf8_bom);
    EXPECT_EQ(d.display_line(0), "hello");
}

TEST(TextDocument, Utf16WithAByteOrderMarkIsConverted)
{
    const std::string le = std::string("\xFF\xFE", 2) + std::string("h\0i\0\r\0\n\0!\0", 10);
    const TextDocument a = doc(le);
    EXPECT_EQ(a.encoding(), TextEncoding::utf16_le);
    ASSERT_EQ(a.line_count(), 2);
    EXPECT_EQ(a.display_line(0), "hi");
    EXPECT_EQ(a.display_line(1), "!");
    const std::string be = std::string("\xFE\xFF", 2) + std::string("\0h\0i", 4);
    const TextDocument b = doc(be);
    EXPECT_EQ(b.encoding(), TextEncoding::utf16_be);
    EXPECT_EQ(b.display_line(0), "hi");
}

TEST(TextDocument, Utf16SurrogatePairsMakeOneCharacterAndLoneOnesAreReplaced)
{
    // U+1F600 is D83D DE00; then a lone high surrogate.
    const std::string le = std::string("\xFF\xFE", 2) + std::string("\x3D\xD8\x00\xDE\x3D\xD8", 6);
    const TextDocument d = doc(le);
    EXPECT_EQ(d.display_line(0), "\xF0\x9F\x98\x80\xEF\xBF\xBD");
}

TEST(TextDocument, TruncationIsRemembered)
{
    EXPECT_TRUE(doc("abc", true).truncated());
    EXPECT_FALSE(doc("abc", false).truncated());
}

TEST(TextDocument, AHundredThousandLinesAreSplitOnceAndTheOneAskedForIsBuilt)
{
    std::string big;
    for (int i = 0; i < 100000; ++i)
        big += "line " + std::to_string(i) + "\n";
    const TextDocument d = doc(big);
    ASSERT_EQ(d.line_count(), 100000);
    EXPECT_EQ(d.display_line(99999), "line 99999");
    EXPECT_EQ(d.display_line(0), "line 0");
}

TEST(LooksLikeText, RecognisesPlainAndAccentedText)
{
    EXPECT_TRUE(looks_like_text("Hello, world!\nSecond line\twith a tab\r\n"));
    EXPECT_TRUE(looks_like_text("caf\xC3\xA9 cr\xC3\xA8me"));
    EXPECT_TRUE(looks_like_text("{\"a\": [1, 2, 3]}"));
}

TEST(LooksLikeText, ByteOrderMarksCount)
{
    EXPECT_TRUE(looks_like_text("\xEF\xBB\xBF"
                                "x"));
    EXPECT_TRUE(looks_like_text(std::string("\xFF\xFE"
                                            "h\0",
                                            4)));
    EXPECT_TRUE(looks_like_text(std::string("\xFE\xFF\0h", 4)));
}

TEST(LooksLikeText, BinaryIsNot)
{
    EXPECT_FALSE(looks_like_text(""));
    EXPECT_FALSE(looks_like_text(std::string("MZ\x90\0\x03\0\0\0", 8))); // a program
    EXPECT_FALSE(looks_like_text("\x89PNG\r\n\x1a\n"));                  // 0x89 starts no sequence
    EXPECT_FALSE(looks_like_text(std::string("text with a \0 inside", 20)));
    EXPECT_FALSE(looks_like_text("\x01\x02\x03\x04\x05\x06\x07\x08 and a few letters"));
}

TEST(LooksLikeText, ACharacterCutOffByTheEndOfTheHeadIsAllowed)
{
    EXPECT_TRUE(looks_like_text("abc\xE2\x94"));  // the third byte of the box character is missing
    EXPECT_FALSE(looks_like_text("abc\xE2\x41")); // but a wrong continuation byte is not
}

} // namespace
