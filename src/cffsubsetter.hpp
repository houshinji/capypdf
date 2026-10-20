// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Jussi Pakkanen

#pragma once

#include <errorhandling.hpp>
#include <mmapper.hpp>
#include <optional>
#include <vector>

#include <span>
#include <cstdint>

namespace capypdf::internal {

enum class DictOperator : uint16_t {
    Version,
    Notice,
    FullName,
    FamilyName,
    Weight,
    FontBBox,
    BlueValues,
    OtherBlues,
    FamilyBlues,
    FamilyOtherBlues,
    StdHW,
    StdVW,
    Escape,
    UniqueID,
    XUID,
    Charset,
    Encoding,
    CharStrings,
    Private,
    Subrs,
    DefaultWidthX,
    NominalWidthX,

    Copyright = 0x0c00,
    IsFixedPitch,
    ItalicAngle,
    UnderlinePosition,
    UnderlineThickness,
    PaintType,
    CharstringType,
    FontMatrix,
    StrokeWidth,
    BlueScale,
    BlueShift,
    BlueFuzz,
    StemSnapH,
    StemSnapV,
    ForceBold,

    LanguageGroup = 0xc11,
    ExpansionFactor,
    InitialRandomSeed,
    SyntheticBase,
    PostScript,
    BaseFontName,
    BaseFontBlend,

    ROS = 0x0c1e,
    CIDFontVersion,
    CIDFontRevision,
    CIDFontType,
    CIDCount,
    UIDBase,
    FDArray,
    FDSelect,
    FontName,
};

static_assert((uint16_t)DictOperator::NominalWidthX == 0x15);
static_assert((uint16_t)DictOperator::ForceBold == 0xc0e);
static_assert((uint16_t)DictOperator::FontName == 0xc26);

struct CFFHeader {
    uint8_t major;
    uint8_t minor;
    uint8_t hdrsize;
    uint8_t offsize;
};

struct CFFIndex {
    std::vector<std::span<std::byte>> entries;

    size_t size() const { return entries.size(); }
};

struct CFFDictItem {
    std::vector<int32_t> operand;
    DictOperator opr; // "operator" is a reserved word
    // The operands exactly as they were encoded in the source font. Real numbers
    // can not be represented in `operand`, so items read from a font are written
    // back from these bytes. Empty for items created from scratch.
    std::vector<std::byte> raw_operands;
};

struct CFFDict {
    std::vector<CFFDictItem> entries;
};

#pragma pack(push, r1, 1)

struct CFFSelectRange3 {
    uint16_t first;
    uint8_t fd;

    void swap_endian();
};

struct CFFCharsetRange1 {
    uint16_t first;
    uint8_t nLeft;

    void swap_endian();
};

struct CFFCharsetRange2 {
    uint16_t first;
    uint16_t nLeft;

    void swap_endian();
};

#pragma pack(pop, r1)

struct DictOutput {
    std::vector<std::byte> output;
    std::vector<uint16_t> offsets;
};

struct LocalSubrs {
    std::vector<std::byte> data;
    std::vector<uint32_t> data_offsets;
};

struct CFFPrivateDict {
    CFFDict entries; // Without substr entry.
    std::optional<CFFIndex> subr;
};

struct CFFFontDict {
    CFFDict entries; // without private entry
    std::optional<CFFPrivateDict> priv;
};

struct CFFont {
    DataSource original_data;
    CFFHeader header;
    CFFIndex name;
    CFFIndex top_dict_data;
    CFFDict top_dict;
    CFFIndex string;
    CFFIndex global_subr;
    CFFIndex char_strings;
    std::vector<CFFCharsetRange2> charsets;
    CFFPrivateDict pdict;
    std::vector<CFFFontDict> fdarray;
    std::vector<CFFSelectRange3> fdselect;
    bool is_cid;
    std::optional<int32_t> predefined_encoding;
    std::optional<uint32_t> predefined_charset;

    uint8_t get_fontdict_id(uint16_t glyph_id) const;
    // The local subroutines that the glyphs of the given font dictionary use, if any.
    // A font that is not in the CID format has only one set of them.
    const CFFIndex *local_subrs(uint8_t fontdict_id) const;
};

rvoe<CFFont> parse_cff_file(const char *fname);
rvoe<CFFont> parse_cff_data(DataSource original_data);

// Replaces every subroutine call in a Type 2 charstring with the contents of the subroutine,
// so that the result can be used without any subroutines. This is what makes it possible to
// leave the subroutines out of a subset font: they are shared between all glyphs of the
// font and are mostly needed by glyphs that were not selected.
//
// Returns false if the charstring uses something that is not supported (for example
// arithmetic operators or an accented character defined with the old "seac" form of endchar),
// in which case the result must not be used.
bool expand_charstring_subroutines(std::span<const std::byte> charstring,
                                   const CFFIndex &global_subrs,
                                   const CFFIndex *local_subrs,
                                   std::vector<std::byte> &result);

struct SubsetGlyphs {
    uint32_t codepoint; // unicode
    uint16_t gid;
};

struct OffsetPatch {
    uint32_t offset = -1;
    uint32_t value = -1;
};

struct Fixups {
    OffsetPatch charsets;
    OffsetPatch fdselect;
    OffsetPatch charstrings;
    OffsetPatch fdarray;
};

class CFFDictWriter {
public:
    void append_command(const std::vector<int32_t> &operands, DictOperator op);
    void append_command(const CFFDictItem &e);

    DictOutput steal() { return std::move(o); }

    size_t current_size() const { return o.output.size(); }

private:
    DictOutput o;
};

// Writes a CID keyed CFF font that has only the given glyphs. Glyph number N of the
// output is the glyph `sub[N].gid` of the source, and its CID is also N. The first one
// must be the .notdef glyph.
//
// The source can be either a CID keyed or a name keyed font.
class CFFWriter {
public:
    CFFWriter(const CFFont &source, const std::vector<SubsetGlyphs> &sub);

    rvoe<NoReturnValue> create();

    std::vector<std::byte> steal() { return std::move(output); }

    // Whether the subroutines were removed from the font by inlining them to the glyphs.
    // This is not possible for all fonts. Only valid after create().
    bool subroutines_removed() const { return !keep_subrs; }

private:
    std::vector<uint32_t> append_index(const CFFIndex &entries);
    std::vector<uint32_t> append_index(const std::vector<std::vector<std::byte>> &entries);
    void append_charset();
    void append_charstrings();
    void append_fdthings();

    rvoe<NoReturnValue> plan_fontdicts();
    rvoe<NoReturnValue> prepare_charstrings();
    void collect_strings();
    int32_t string_id(std::string_view s);
    rvoe<NoReturnValue> create_topdict();
    void copy_dict_item_if_exists(CFFDictWriter &w, DictOperator op);

    void patch_offsets();
    void write_fix(const OffsetPatch &p);

    const CFFont &source;
    const std::vector<SubsetGlyphs> &sub;
    std::vector<std::byte> output;
    Fixups fixups;

    // Strings of the output font. The ones of the source plus whatever we need.
    std::vector<std::span<std::byte>> strings;
    // Font dictionaries of the source that are used by the glyphs, in output order.
    std::vector<uint8_t> used_fontdicts;
    // For each output glyph, the number of its font dictionary in the output.
    std::vector<uint8_t> glyph_fontdict;
    // Charstrings of the output glyphs. Only used if the subroutines are removed.
    std::vector<std::vector<std::byte>> expanded_charstrings;
    bool keep_subrs = false;
};

} // namespace capypdf::internal
