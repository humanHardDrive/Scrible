#pragma once

// Reads handwritten numbers from pen strokes. Digits are compared with samples of
// many people's handwriting (UCI "Pen-Based Recognition of Handwritten Digits",
// CC BY 4.0), and with the user's own corrections, which count for more. Decimal
// points, thousands commas and minus signs are told by their size, shape and place.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "Document.h"

namespace scrible {

// A character's strokes, joined in the order they were written and resampled to 8
// points evenly spaced along the way, each axis scaled to 0..100 (y up): the form
// the samples are in.
using DigitFeatures = std::array<uint8_t, 16>;
DigitFeatures digitFeatures(const std::vector<const Stroke*>& strokes);

class DigitReader {
public:
    // `allSamples` false keeps back the samples of the 14 writers used for testing.
    explicit DigitReader(bool allSamples = true);

    int classify(const DigitFeatures& f) const;  // 0..9

    // The user's own handwriting: corrections, kept in a small file of "digit v0 ... v15" lines.
    void addExample(const DigitFeatures& f, int digit);
    size_t exampleCount() const { return examples_.size(); }
    bool loadExamples(const std::string& path);
    bool appendExample(const std::string& path, const DigitFeatures& f, int digit) const;

    // The held-back test samples, for measuring accuracy.
    static size_t testCount();
    static DigitFeatures testSample(size_t i, int& digit);

private:
    struct Sample {
        DigitFeatures f;
        uint8_t digit;
    };
    std::vector<Sample> samples_, examples_;
};

struct ReadChar {
    char c = '?';                 // '0'..'9', '.', ',', '-', or '?' for a mark it can't place
    std::vector<size_t> strokes;  // indices into the strokes that were read
    RectF bounds;                 // page coordinates
    DigitFeatures features{};     // digits only
};

struct ReadNumber {
    std::vector<ReadChar> chars;
    std::string text() const;
    // The number, if the characters make one: an optional minus sign, digits (with
    // commas only between groups of three), and an optional decimal part.
    bool parse(double& value) const;
};

// The number a typed line starts with, spelled as ReadNumber spells them ("-1,250.5";
// ".5" becomes "0.5"). Read like strtod: spaces before it are skipped, and whatever
// follows it is ignored, so "12 apples" is 12. Empty if the line doesn't start with one.
std::string typedNumber(const std::string& line);

// Numbers written in `strokes`: one or more per line, lines top to bottom, each left
// to right. `strokes` is in the order they were written.
std::vector<ReadNumber> readNumbers(const std::vector<Stroke>& strokes, const DigitReader& reader);

}  // namespace scrible
