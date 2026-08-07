// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Core/Text.cpp
#include "Core/Text.h"

namespace mpe::text {
namespace {

constexpr char32_t kReplacement = 0xFFFD;

/// How many bytes a leading byte claims, or zero when it is not a leading byte.
[[nodiscard]] int SequenceLength(unsigned char lead) {
    if (lead < 0x80) {
        return 1;
    }
    if ((lead & 0xE0) == 0xC0) {
        return 2;
    }
    if ((lead & 0xF0) == 0xE0) {
        return 3;
    }
    if ((lead & 0xF8) == 0xF0) {
        return 4;
    }
    return 0;
}

[[nodiscard]] bool IsContinuation(unsigned char byte) {
    return (byte & 0xC0) == 0x80;
}

/// Whitespace as far as a name is concerned. Deliberately not std::isspace, which is
/// locale dependent and only answers for single bytes.
[[nodiscard]] bool IsSpace(char32_t code_point) {
    return code_point == U' ' || code_point == U'\t' || code_point == U'\n' ||
           code_point == U'\r' || code_point == 0x00A0;
}

} // namespace

std::vector<char32_t> DecodeUtf8(std::string_view text) {
    std::vector<char32_t> out;
    out.reserve(text.size());

    std::size_t index = 0;
    while (index < text.size()) {
        const auto lead   = static_cast<unsigned char>(text[index]);
        const int  length = SequenceLength(lead);

        // A byte that cannot start a sequence, or a sequence the string is too short to
        // hold, is one replacement character and one byte of progress. Skipping the whole
        // claimed length instead would swallow valid characters after a single bad byte.
        if (length == 0 || index + static_cast<std::size_t>(length) > text.size()) {
            out.push_back(kReplacement);
            ++index;
            continue;
        }

        char32_t value = 0;
        switch (length) {
            case 1: value = lead; break;
            case 2: value = lead & 0x1FU; break;
            case 3: value = lead & 0x0FU; break;
            default: value = lead & 0x07U; break;
        }

        bool valid = true;
        for (int offset = 1; offset < length; ++offset) {
            const auto next = static_cast<unsigned char>(text[index + static_cast<std::size_t>(offset)]);
            if (!IsContinuation(next)) {
                valid = false;
                break;
            }
            value = (value << 6) | (next & 0x3FU);
        }
        if (!valid) {
            out.push_back(kReplacement);
            ++index;
            continue;
        }

        // Overlong encodings, surrogate halves and anything past the last plane are all
        // invalid however well formed the bytes look. Letting a lone surrogate through is
        // what produces an unpaired half in the UTF-16 the engine is handed.
        const bool overlong = (length == 2 && value < 0x80) || (length == 3 && value < 0x800) ||
                              (length == 4 && value < 0x10000);
        const bool surrogate = value >= 0xD800 && value <= 0xDFFF;
        if (overlong || surrogate || value > 0x10FFFF) {
            out.push_back(kReplacement);
            index += static_cast<std::size_t>(length);
            continue;
        }

        out.push_back(value);
        index += static_cast<std::size_t>(length);
    }
    return out;
}

std::string EncodeUtf8(const std::vector<char32_t>& code_points) {
    std::string out;
    out.reserve(code_points.size());
    for (const char32_t value : code_points) {
        if (value < 0x80) {
            out.push_back(static_cast<char>(value));
        } else if (value < 0x800) {
            out.push_back(static_cast<char>(0xC0U | (value >> 6)));
            out.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
        } else if (value < 0x10000) {
            out.push_back(static_cast<char>(0xE0U | (value >> 12)));
            out.push_back(static_cast<char>(0x80U | ((value >> 6) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
        } else {
            out.push_back(static_cast<char>(0xF0U | (value >> 18)));
            out.push_back(static_cast<char>(0x80U | ((value >> 12) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | ((value >> 6) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
        }
    }
    return out;
}

std::wstring WidenUtf8(std::string_view text) {
    std::wstring out;
    out.reserve(text.size());
    for (const char32_t value : DecodeUtf8(text)) {
        if (value < 0x10000) {
            out.push_back(static_cast<wchar_t>(value));
            continue;
        }
        const char32_t adjusted = value - 0x10000;
        out.push_back(static_cast<wchar_t>(0xD800 + (adjusted >> 10)));
        out.push_back(static_cast<wchar_t>(0xDC00 + (adjusted & 0x3FF)));
    }
    return out;
}

std::size_t CountCodePoints(std::string_view text) {
    return DecodeUtf8(text).size();
}

std::string Truncate(std::string_view text, std::size_t max_code_points) {
    std::vector<char32_t> points = DecodeUtf8(text);
    if (points.size() <= max_code_points) {
        return std::string(text);
    }
    points.resize(max_code_points);
    return EncodeUtf8(points);
}

std::string Ellipsise(std::string_view text, std::size_t max_code_points) {
    const std::vector<char32_t> points = DecodeUtf8(text);
    if (points.size() <= max_code_points) {
        return std::string(text);
    }
    // The ellipsis has to fit inside the limit rather than be added past it, or a name cut
    // to a column's width comes back three characters wider than the column.
    if (max_code_points <= 3) {
        return Truncate(text, max_code_points);
    }
    std::vector<char32_t> kept(points.begin(),
                               points.begin() + static_cast<std::ptrdiff_t>(max_code_points - 3));
    // Trailing spaces before an ellipsis read as a gap rather than as a cut.
    while (!kept.empty() && IsSpace(kept.back())) {
        kept.pop_back();
    }
    return EncodeUtf8(kept) + "...";
}

bool IsDrawable(char32_t code_point) {
    // Basic Latin, minus the control range. The space is kept because a name is allowed to
    // have words in it.
    if (code_point >= 0x0020 && code_point <= 0x007E) {
        return true;
    }
    // Latin-1 Supplement through Latin Extended-B, which covers accented European names.
    // The C1 control block at 0x80..0x9F is excluded.
    if (code_point >= 0x00A1 && code_point <= 0x024F) {
        return true;
    }
    // Greek and Coptic, then Cyrillic. Both are in the frontend's face.
    if (code_point >= 0x0370 && code_point <= 0x03FF) {
        return true;
    }
    if (code_point >= 0x0400 && code_point <= 0x04FF) {
        return true;
    }
    // The handful of general punctuation a name plausibly uses and the font has: dashes and
    // curly quotes. Everything else in that block is spacing and marks that do nothing
    // useful here, including the zero width joiners emoji sequences are built from.
    if (code_point >= 0x2010 && code_point <= 0x2015) {
        return true;
    }
    if (code_point >= 0x2018 && code_point <= 0x201D) {
        return true;
    }
    return false;
}

std::string CleanDisplayName(std::string_view raw, std::size_t max_code_points) {
    const std::vector<char32_t> points = DecodeUtf8(raw);

    std::vector<char32_t> kept;
    kept.reserve(points.size());
    for (const char32_t value : points) {
        if (!IsDrawable(value)) {
            continue;
        }
        // Dropping the decoration out of a name like "[decoration] Nessie [decoration]"
        // leaves the spaces that surrounded it, so runs are collapsed rather than left as a
        // name that appears to start halfway across its slot.
        if (IsSpace(value) && (kept.empty() || IsSpace(kept.back()))) {
            continue;
        }
        kept.push_back(value);
    }
    while (!kept.empty() && IsSpace(kept.back())) {
        kept.pop_back();
    }

    if (kept.empty()) {
        return "PLAYER";
    }
    return Ellipsise(EncodeUtf8(kept), max_code_points);
}

} // namespace mpe::text
