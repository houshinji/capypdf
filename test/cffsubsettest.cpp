// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jussi Pakkanen

// Tests for creating subsets of CFF fonts, including CID keyed ones
// such as the Japanese Noto fonts.

#include <cffsubsetter.hpp>
#include <ft_subsetter.hpp>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

using namespace capypdf::internal;

namespace {

int failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if(!(cond)) {                                                                              \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);               \
            ++failures;                                                                            \
        }                                                                                          \
    } while(0)

typedef std::vector<std::byte> Bytes;

Bytes bytes_of(std::initializer_list<int> values) {
    Bytes b;
    for(const auto v : values) {
        b.push_back(std::byte(v));
    }
    return b;
}

// Encodes a small integer as a charstring operand.
int num(int v) { return v + 139; }

// Owns the entries of an index.
struct TestIndex {
    explicit TestIndex(std::vector<Bytes> entries_) : entries{std::move(entries_)} {
        for(auto &e : entries) {
            index.entries.emplace_back(e.data(), e.size());
        }
    }
    std::vector<Bytes> entries;
    CFFIndex index;
};

bool expand(const Bytes &charstring,
            const CFFIndex &gsubrs,
            const CFFIndex *lsubrs,
            Bytes &result) {
    return expand_charstring_subroutines(
        std::span<const std::byte>(charstring.data(), charstring.size()), gsubrs, lsubrs, result);
}

void test_expansion() {
    const CFFIndex no_subrs;
    Bytes out;

    // No subroutines: the charstring is copied as is. 21 is rmoveto, 14 endchar.
    const auto plain = bytes_of({num(10), num(20), 21, num(30), num(-30), 5, 14});
    CHECK(expand(plain, no_subrs, nullptr, out));
    CHECK(out == plain);

    // A call to a global subroutine. With fewer than 1240 subroutines the bias is 107.
    // Operands to the subroutine are before the call and the operator after it.
    {
        TestIndex gs({bytes_of({num(7), num(8), 11}), bytes_of({num(1), num(2), 5, 11})});
        const auto cs = bytes_of({num(100), num(-107), 29, 21, num(-106), 29, 14});
        CHECK(expand(cs, gs.index, nullptr, out));
        CHECK(out == bytes_of({num(100), num(7), num(8), 21, num(1), num(2), 5, 14}));
    }

    // Nested calls and local subroutines.
    {
        TestIndex gs({bytes_of({num(5), 5, 11})});
        TestIndex ls({bytes_of({num(-107), 29, num(6), 5, 11}), bytes_of({num(-107), 10, 11})});
        const auto cs = bytes_of({num(1), 21, num(-106), 10, 14});
        CHECK(expand(cs, gs.index, &ls.index, out));
        CHECK(out == bytes_of({num(1), 21, num(5), 5, num(6), 5, 14}));
    }

    // Number of the subroutine written with other operand types.
    {
        TestIndex gs({bytes_of({num(9), 5, 11})});
        // 28 is a 16 bit integer. -107 is 0xFF95.
        const auto cs = bytes_of({28, 0xFF, 0x95, 29, 14});
        CHECK(expand(cs, gs.index, nullptr, out));
        CHECK(out == bytes_of({num(9), 5, 14}));
    }

    // The bias grows with the number of subroutines.
    {
        std::vector<Bytes> subrs;
        for(int i = 0; i < 1240; ++i) {
            subrs.push_back(bytes_of({num(i % 100), 6, 11}));
        }
        TestIndex gs(subrs);
        // Subroutine number zero is the one at index 1131.
        const auto cs = bytes_of({num(0), 29, 14});
        CHECK(expand(cs, gs.index, nullptr, out));
        CHECK(out == bytes_of({num(1131 % 100), 6, 14}));
        // And the first one can be reached with the lowest possible operand.
        const auto cs2 = bytes_of({28, 0xFB, 0x95, 29, 14}); // -1131
        CHECK(expand(cs2, gs.index, nullptr, out));
        CHECK(out == bytes_of({num(0), 6, 14}));
    }

    // Hint masks have one bit for each stem, even if the stems and the mask are in
    // different subroutines. The mask bytes below look like the callsubr operator, and must
    // be skipped and not treated as one. If the number of stems is miscounted the
    // call has no operand and the expansion fails.
    {
        // Each of these declares one stem: hstemhm.
        auto stems = [](int count) {
            Bytes b;
            for(int i = 0; i < count; ++i) {
                for(const auto v : {num(1), num(2), 18}) {
                    b.push_back(std::byte(v));
                }
            }
            return b;
        };
        auto concat = [](Bytes a, const Bytes &b) {
            a.insert(a.end(), b.begin(), b.end());
            return a;
        };
        TestIndex gs({bytes_of({19, 10, 11}),                               // hintmask, 1 byte
                      bytes_of({num(5), num(6), 1, num(7), num(8), 3, 11}), // hstem, vstem
                      bytes_of({19, 10, 10, 11})});                         // hintmask, 2 bytes
        // The stem is in the glyph and the mask is in the subroutine.
        CHECK(expand(
            concat(stems(1), bytes_of({num(-107), 29, num(3), 21, 14})), gs.index, nullptr, out));
        CHECK(out == concat(stems(1), bytes_of({19, 10, num(3), 21, 14})));
        // The stems are in the subroutine and the mask is in the glyph.
        CHECK(expand(bytes_of({num(-106), 29, 19, 10, 14}), gs.index, nullptr, out));
        CHECK(out == bytes_of({num(5), num(6), 1, num(7), num(8), 3, 19, 10, 14}));
        // Eight stems still fit in one byte, nine need two.
        auto with_mask = [&](int count, const Bytes &mask) {
            return concat(concat(stems(count), bytes_of({19})), concat(mask, bytes_of({14})));
        };
        CHECK(expand(with_mask(8, bytes_of({10})), gs.index, nullptr, out));
        CHECK(out == with_mask(8, bytes_of({10})));
        CHECK(expand(with_mask(9, bytes_of({10, 10})), gs.index, nullptr, out));
        CHECK(out == with_mask(9, bytes_of({10, 10})));
        // The same with a mask in a subroutine, after the stems in the glyph.
        CHECK(expand(concat(stems(9), bytes_of({num(-105), 29, 14})), gs.index, nullptr, out));
        CHECK(out == concat(stems(9), bytes_of({19, 10, 10, 14})));
        // Operands before a hintmask are an implicit vstem. This makes the ninth stem.
        const auto implicit =
            concat(concat(stems(8), bytes_of({num(4), num(5), 19, 10, 10})), bytes_of({14}));
        CHECK(expand(implicit, gs.index, nullptr, out));
        CHECK(out == implicit);
    }

    // Things we can't handle.
    {
        TestIndex gs({bytes_of({num(1), num(2), 5, 11})});
        // Arithmetic.
        CHECK(!expand(bytes_of({num(1), num(2), 12, 10, 21, 14}), gs.index, nullptr, out));
        // Subroutine that does not exist.
        CHECK(!expand(bytes_of({num(-100), 29, 14}), gs.index, nullptr, out));
        CHECK(!expand(bytes_of({num(0), 29, 14}), gs.index, nullptr, out));
        // No local subroutines.
        CHECK(!expand(bytes_of({num(-107), 10, 14}), gs.index, nullptr, out));
        // Call without an operand.
        CHECK(!expand(bytes_of({29, 14}), gs.index, nullptr, out));
        // The old way of building accented characters.
        CHECK(!expand(bytes_of({num(0), num(0), num(65), num(194), 14}), gs.index, nullptr, out));
        // A reserved operator.
        CHECK(!expand(bytes_of({num(1), 9, 14}), gs.index, nullptr, out));
        // Cut short in the middle of an operand, or of a mask.
        CHECK(!expand(bytes_of({num(1), 28, 0}), gs.index, nullptr, out));
        CHECK(!expand(bytes_of({num(1), num(2), 18, 19}), gs.index, nullptr, out));
        // A real number is not a subroutine number.
        CHECK(!expand(bytes_of({255, 0, 1, 0, 0, 29, 14}), gs.index, nullptr, out));
    }
    // A subroutine that calls itself.
    {
        TestIndex gs({bytes_of({num(-107), 29, 11})});
        CHECK(!expand(bytes_of({num(-107), 29, 14}), gs.index, nullptr, out));
    }
    // Code after an endchar is not part of the glyph.
    {
        TestIndex gs({bytes_of({num(1), 5, 14, num(2), 5, 11})});
        CHECK(expand(bytes_of({num(-107), 29, num(3), 5}), gs.index, nullptr, out));
        CHECK(out == bytes_of({num(1), 5, 14}));
    }
}

bool same_outline(FT_Face a, uint32_t glyph_a, FT_Face b, uint32_t glyph_b) {
    const FT_Int32 flags = FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP;
    if(FT_Load_Glyph(a, glyph_a, flags) != 0 || FT_Load_Glyph(b, glyph_b, flags) != 0) {
        return false;
    }
    // The width of a glyph comes from the private dict of its font dictionary.
    if(a->glyph->metrics.horiAdvance != b->glyph->metrics.horiAdvance) {
        return false;
    }
    const FT_Outline &oa = a->glyph->outline;
    const FT_Outline &ob = b->glyph->outline;
    if(oa.n_contours != ob.n_contours || oa.n_points != ob.n_points) {
        return false;
    }
    for(int i = 0; i < oa.n_contours; ++i) {
        if(oa.contours[i] != ob.contours[i]) {
            return false;
        }
    }
    for(int i = 0; i < oa.n_points; ++i) {
        if(oa.points[i].x != ob.points[i].x || oa.points[i].y != ob.points[i].y ||
           (oa.tags[i] & 3) != (ob.tags[i] & 3)) {
            return false;
        }
    }
    return true;
}

// The top dict operand that is a string id, as a string.
std::string sid_string(const CFFont &f, int32_t sid) {
    if(sid < 391) {
        return "<standard string>";
    }
    const auto &s = f.string.entries.at(sid - 391);
    return std::string((const char *)s.data(), s.size());
}

const CFFDictItem *find_item(const CFFDict &d, DictOperator op) {
    for(const auto &e : d.entries) {
        if(e.opr == op) {
            return &e;
        }
    }
    return nullptr;
}

void remove_item(CFFDict &d, DictOperator op) {
    std::erase_if(d.entries, [op](const CFFDictItem &e) { return e.opr == op; });
}

// Changes made to the top dict of the font before subsetting it.
struct TopDictTweak {
    std::vector<DictOperator> remove;
    bool add_font_matrix = false;
};

// Subsets the glyphs of the font and checks that the glyph outlines are the same.
void test_font(const std::string &fontfile,
               int subfont,
               const std::vector<uint32_t> &codepoints,
               bool cid_keyed,
               size_t max_size,
               const TopDictTweak &tweak) {
    FT_Library ft;
    CHECK(FT_Init_FreeType(&ft) == 0);
    FT_Face original;
    CHECK(FT_New_Face(ft, fontfile.c_str(), subfont, &original) == 0);

    FontProperties props;
    props.subfont = subfont;
    auto fontdata = load_and_parse_font_file(fontfile.c_str(), props);
    CHECK(fontdata.has_value());
    if(!fontdata) {
        return;
    }
    auto *ttf = std::get_if<TrueTypeFontFile>(&fontdata.value());
    CHECK(ttf != nullptr);
    CHECK(ttf && ttf->in_cff_format());
    if(!ttf || !ttf->in_cff_format()) {
        return;
    }
    CFFont &source = ttf->cff.value();
    CHECK(source.is_cid == cid_keyed);
    const auto source_fontdicts = source.fdarray.size();
    for(const auto op : tweak.remove) {
        remove_item(source.top_dict, op);
    }
    if(tweak.add_font_matrix) {
        // 0.0005 0 0 0.0005 0 0, that is 2000 units per em. Real numbers are
        // two digits per byte: 0.0005 is 0 . 0 0 0 5 and then the end marker.
        CFFDictItem fm;
        fm.opr = DictOperator::FontMatrix;
        const Bytes real = bytes_of({30, 0x0a, 0x00, 0x05, 0xff});
        const Bytes zero = bytes_of({139});
        for(const auto &part : {real, zero, zero, real, zero, zero}) {
            fm.raw_operands.insert(fm.raw_operands.end(), part.begin(), part.end());
        }
        source.top_dict.entries.push_back(fm);
    }

    // The first glyph is always .notdef.
    std::vector<SubsetGlyphs> glyphs{SubsetGlyphs{0, 0}};
    for(const auto cp : codepoints) {
        const auto gid = FT_Get_Char_Index(original, cp);
        CHECK(gid != 0);
        glyphs.push_back(SubsetGlyphs{cp, (uint16_t)gid});
    }
    CFFWriter writer(source, glyphs);
    auto created = writer.create();
    CHECK(created.has_value());
    if(!created) {
        return;
    }
    CHECK(writer.subroutines_removed());
    auto output = writer.steal();
    CHECK(output.size() < max_size);

    auto reread = parse_cff_data(std::span<std::byte>(output.data(), output.size()));
    CHECK(reread.has_value());
    if(!reread) {
        return;
    }
    const auto &subset = reread.value();
    CHECK(subset.char_strings.size() == glyphs.size());
    CHECK(subset.is_cid);
    // Subroutines are not used by the glyphs so they are not needed.
    CHECK(subset.global_subr.size() == 0);
    CHECK(!subset.pdict.subr);
    for(const auto &fd : subset.fdarray) {
        CHECK(!fd.priv || !fd.priv->subr);
    }
    if(cid_keyed) {
        // Only the font dictionaries that some glyph uses are needed.
        CHECK(!subset.fdarray.empty());
        CHECK(subset.fdarray.size() < source_fontdicts);
    } else {
        CHECK(subset.fdarray.size() == 1);
    }
    // The glyphs are numbered in order, so the character collection is the trivial one.
    const auto *ros = find_item(subset.top_dict, DictOperator::ROS);
    CHECK(ros != nullptr);
    if(ros && ros->operand.size() == 3) {
        CHECK(sid_string(subset, ros->operand[0]) == "Adobe");
        CHECK(sid_string(subset, ros->operand[1]) == "Identity");
        CHECK(ros->operand[2] == 0);
    } else {
        CHECK(false);
    }
    CHECK(find_item(subset.top_dict, DictOperator::FDArray) != nullptr);
    CHECK(find_item(subset.top_dict, DictOperator::FDSelect) != nullptr);
    CHECK(find_item(subset.top_dict, DictOperator::Charset) != nullptr);
    CHECK(find_item(subset.top_dict, DictOperator::CharStrings) != nullptr);
    for(const auto op : tweak.remove) {
        // Items that the source does not have are not made up. The exception
        // is the number of CIDs, which is always written.
        if(op != DictOperator::CIDCount) {
            CHECK(find_item(subset.top_dict, op) == nullptr);
        }
    }
    CHECK((find_item(subset.top_dict, DictOperator::FontMatrix) != nullptr) ==
          (tweak.add_font_matrix || find_item(source.top_dict, DictOperator::FontMatrix)));

    // And the glyphs look the same. FreeType accepts bare CFF fonts.
    FT_Face subface;
    CHECK(FT_New_Memory_Face(ft, (const FT_Byte *)output.data(), output.size(), 0, &subface) == 0);
    CHECK((size_t)subface->num_glyphs == glyphs.size());
    if(tweak.add_font_matrix) {
        CHECK(subface->units_per_EM == 2000);
    } else {
        CHECK(subface->units_per_EM == original->units_per_EM);
    }
    for(size_t i = 0; i < glyphs.size(); ++i) {
        CHECK(same_outline(original, glyphs[i].gid, subface, (uint32_t)i));
    }
    FT_Done_Face(subface);
    FT_Done_Face(original);
    FT_Done_FreeType(ft);
}

void test_errors(const std::string &fontfile) {
    FontProperties props;
    auto fontdata = load_and_parse_font_file(fontfile.c_str(), props);
    CHECK(fontdata.has_value());
    if(!fontdata) {
        return;
    }
    auto *ttf = std::get_if<TrueTypeFontFile>(&fontdata.value());
    CHECK(ttf && ttf->in_cff_format());
    if(!ttf || !ttf->in_cff_format()) {
        return;
    }
    // Nonexisting glyph.
    std::vector<SubsetGlyphs> glyphs{SubsetGlyphs{0, 0}, SubsetGlyphs{65, 60000}};
    CFFWriter w1(ttf->cff.value(), glyphs);
    CHECK(!w1.create().has_value());
    // No glyphs at all.
    std::vector<SubsetGlyphs> empty;
    CFFWriter w2(ttf->cff.value(), empty);
    CHECK(!w2.create().has_value());
}

} // namespace

int main(int argc, char **argv) {
    if(argc != 2) {
        fprintf(stderr, "%s <directory with test fonts>\n", argv[0]);
        return 1;
    }
    const std::string fontdir{argv[1]};
    // A name keyed font. Its glyphs have to be converted to a CID keyed font.
    const auto latin = fontdir + "/P052-Roman.otf";
    // A CID keyed font that has 18 font dictionaries, and local and global subroutines.
    const auto cjk = fontdir + "/NotoSerifCJK-Bold.ttc";
    const std::vector<uint32_t> latin_text{'A', 'V', 'a', 't', 'a', 'r', ' ', '0', 0xe9, 0xdf};
    const std::vector<uint32_t> cjk_text{
        0x5927, 0x5200, 0x65e5, 0x672c, 0x8a9e, 0x300c, 0x30fc, 'A'};

    test_expansion();

    test_font(latin, 0, latin_text, false, 100000, {});
    test_font(cjk, 0, cjk_text, true, 40000, {});
    // The top dict does not necessarily have all of the optional items. The offsets
    // that are filled in later must not depend on that.
    test_font(latin,
              0,
              latin_text,
              false,
              100000,
              {{DictOperator::Notice,
                DictOperator::FullName,
                DictOperator::FamilyName,
                DictOperator::Weight,
                DictOperator::FontBBox},
               false});
    test_font(latin, 0, latin_text, false, 100000, {{DictOperator::Weight}, true});
    test_font(cjk, 0, cjk_text, true, 40000, {{DictOperator::Notice, DictOperator::Weight}, true});
    test_font(cjk,
              0,
              cjk_text,
              true,
              40000,
              {{DictOperator::CIDFontVersion, DictOperator::CIDCount}, false});
    test_errors(latin);

    if(failures) {
        fprintf(stderr, "%d checks failed.\n", failures);
        return 1;
    }
    return 0;
}
