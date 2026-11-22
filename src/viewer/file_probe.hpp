// EliVier Explorer - What is known about a file before a handler is chosen.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A probe is made for every row that is listed (name and size only: nothing is read) and for the
// file that is opened (with a reader for its first bytes). A handler asks for the head only when it
// wants to sniff, and it is read once however many handlers ask.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace hui::viewer
{

// The kind of picture a file gets in the list, and the family a handler belongs to.
enum class IconKind : std::uint8_t
{
    folder,
    archive,
    image,
    audio,
    video,
    text,
    program,
    disc, // a game or disc image
    other,
};

// The icon an extension (lower case, no dot) stands for; other for an unknown one.
IconKind icon_by_extension(const std::string &ext);

class FileProbe
{
  public:
    // `head_reader`, when given, returns the first bytes of the file (up to kHeadBytes).
    explicit FileProbe(std::string name, std::uint64_t size,
                       std::function<std::string()> head_reader = nullptr);

    static constexpr std::size_t kHeadBytes = 4096;

    const std::string &name() const
    {
        return name_;
    }
    // After the last dot, in lower case; "" for no dot, a leading dot only, or a trailing dot.
    const std::string &ext() const
    {
        return ext_;
    }
    std::uint64_t size() const
    {
        return size_;
    }
    // Whether head() can say anything: a probe made for a listing cannot.
    bool can_sniff() const
    {
        return static_cast<bool>(reader_);
    }
    const std::string &head() const;

  private:
    std::string name_;
    std::string ext_;
    std::uint64_t size_;
    std::function<std::string()> reader_;
    mutable bool loaded_ = false;
    mutable std::string head_;
};

} // namespace hui::viewer
