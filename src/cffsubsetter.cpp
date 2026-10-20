// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Jussi Pakkanen

#include <cffsubsetter.hpp>
#include <bitfiddling.hpp>
#include <utils.hpp>

#include <algorithm>
#include <bit>
#include <iterator>

// clang-format off

/*
 * A subset CFF font created from Noto Serif CJK with one glyph contains the following:
 *
 * Subset top dict entries

 * 3102  1e ROS
 * 1     notice
 * 2     fullname
 * 3     familyname
 * 4     weight
 * 5     fontbbox
 * 3103  1f CIDFontVersion
 * 3106  20 CIDFontRevision
 * 3108  22 CIDCount
 * 3109  24 FDArray
 * 3109  25 FDSelect
 * 15    charset
 * 17    charstrings
 * 3075  03 UnderlinePosition
 *
 * Strings:
 *
 * Adobe
 * Identity
 * Copyright 2014-2021 Adobe (http://www.adobe.com/). Noto is a trademark of Google Inc.
 * Noto Sans CJK JP Regular
 * Noto Sans CJK JP
 * NotoSansCJKjp-Regular-Generic
 * NotoSansCJKjp-Regular-Ideographs
 *
 */

// clang-format on

namespace capypdf::internal {

namespace {

// String ids below this are not stored in the font.
const int32_t NUM_STANDARD_STRINGS = 391;

const char registry_str[] = "Adobe";
const char ordering_str[] = "Identity";

rvoe<std::span<std::byte>>
safe_subspan(const std::span<std::byte> &span, size_t offset, size_t size) {
    if(offset + size < offset) {
        // Overflow
        RETERR(IndexOutOfBounds);
    }
    if(offset > span.size_bytes()) {
        RETERR(IndexOutOfBounds);
    }
    if(offset + size > span.size_bytes()) {
        RETERR(IndexOutOfBounds);
    }
    return span.subspan(offset, size);
}

rvoe<size_t> extract_index_offset(std::span<std::byte> dataspan, size_t offset, uint8_t offSize) {
    if(dataspan.size_bytes() <= offset + offSize) {
        RETERR(IndexOutOfBounds);
    }
    if(offSize == 1) {
        const uint8_t b0 = (uint8_t)dataspan[offset];
        return b0;
    } else if(offSize == 2) {
        ERC(v, extract<uint16_t>(dataspan, offset))
        return std::byteswap(v);
    } else if(offSize == 3) {
        const uint8_t b2 = (uint8_t)dataspan[offset];
        const uint8_t b1 = (uint8_t)dataspan[offset + 1];
        const uint8_t b0 = (uint8_t)dataspan[offset + 2];
        return b2 << 16 | b1 << 8 | b0;
    } else if(offSize == 4) {
        ERC(v, extract<uint32_t>(dataspan, offset))
        return std::byteswap(v);
    } else {
        RETERR(MalformedFontFile);
    }
}

rvoe<CFFIndex> load_index(std::span<std::byte> dataspan, size_t &offset) {
    CFFIndex index;
    ERC(cnt, extract<uint16_t>(dataspan, offset));
    const uint32_t count = std::byteswap(cnt);
    offset += sizeof(uint16_t);
    if(count == 0) {
        // An empty index consists only of the count.
        return index;
    }
    ERC(offSize, extract<uint8_t>(dataspan, offset));
    ++offset;
    std::vector<size_t> offsets;
    offsets.reserve(count + 1);
    if(offSize > 5) {
        RETERR(MalformedFontFile);
    }
    for(uint32_t i = 0; i <= count; ++i) {
        ERC(c, extract_index_offset(dataspan, offset, offSize));
        if(c <= 0) {
            RETERR(MalformedFontFile);
        }
        if(!offsets.empty()) {
            if(offsets.back() > c) {
                /*
                fprintf(stderr,
                        "CFF font has index with negative size in entry %d (previous offset %d, "
                        "current offset %d).\n ",
                        i,
                        (int)offsets.back(),
                        (int)c);
*/
                RETERR(MalformedFontFile);
            }
        }
        offsets.push_back(c);
        offset += offSize;
    }
    --offset;
    index.entries.reserve(count);

    for(uint16_t i = 0; i < count; ++i) {
        const auto entrysize = offsets[i + 1] - offsets[i];
        if(offset + offsets[i] > dataspan.size() ||
           offset + offsets[i] + entrysize > dataspan.size()) {
            RETERR(MalformedFontFile);
        }
        ERC(entry, safe_subspan(dataspan, offset + offsets[i], entrysize));
        index.entries.emplace_back(entry);
    }
    offset += offsets.back();
    return index;
}

rvoe<CFFPrivateDict>
build_pdict_from_dict(std::span<std::byte> dataspan, size_t priv_dict_offset, CFFDict raw_pdict) {
    CFFPrivateDict priv(std::move(raw_pdict), {});
    if(priv.entries.entries.size() > 0) {
        // This seems to always be last.
        const auto &subrs = priv.entries.entries.back();
        if(subrs.opr == DictOperator::Subrs) {
            size_t local_subrs_offset = priv_dict_offset + subrs.operand.front();
            ERC(rsdata, load_index(dataspan, local_subrs_offset));
            priv.subr = std::move(rsdata);
            priv.entries.entries.pop_back();
        }
    }
    return priv;
}

rvoe<CFFDict> unpack_dictionary(std::span<std::byte> dataspan_orig) {
    CFFDict dict;
    size_t offset = 0;
    std::vector<int32_t> operands;
    size_t operands_start = 0;
    // span.at() is not available yet. Change back to span once it is.
    auto dataspan = span2sv(dataspan_orig);
    while(offset < dataspan.size()) {
        // Read operand
        const size_t item_start = offset;
        const int32_t b0 = (uint8_t)dataspan.at(offset++);
        if(b0 <= 21) {
            // Is an operator.
            int32_t unpacked_operator = b0;
            if(b0 == 0xc) {
                const int32_t b1 = (uint8_t)dataspan.at(offset++);
                unpacked_operator = b0 << 8 | b1;
            }
            std::vector<std::byte> raw(dataspan_orig.begin() + operands_start,
                                       dataspan_orig.begin() + item_start);
            dict.entries.emplace_back(
                std::move(operands), (DictOperator)unpacked_operator, std::move(raw));
            operands.clear();
            operands_start = offset;
        } else if((b0 >= 28 && b0 <= 30) || (b0 >= 32 && b0 <= 254)) {
            // Is an operand.
            int32_t unpacked_operand;
            if(b0 >= 32 && b0 <= 246) {
                unpacked_operand = b0 - 139;
            } else if(b0 >= 247 && b0 <= 250) {
                const int32_t b1 = (uint8_t)dataspan.at(offset++);
                unpacked_operand = (b0 - 247) * 256 + b1 + 108;
            } else if(b0 >= 251 && b0 <= 254) {
                const int32_t b1 = (uint8_t)dataspan.at(offset++);
                unpacked_operand = -(b0 - 251) * 256 - b1 - 108;
            } else if(b0 == 28) {
                const int32_t b1 = (uint8_t)dataspan.at(offset++);
                const int32_t b2 = (uint8_t)dataspan.at(offset++);
                unpacked_operand = b1 << 8 | b2;
            } else if(b0 == 29) {
                const int32_t b1 = (uint8_t)dataspan.at(offset++);
                const int32_t b2 = (uint8_t)dataspan.at(offset++);
                const int32_t b3 = (uint8_t)dataspan.at(offset++);
                const int32_t b4 = (uint8_t)dataspan.at(offset++);
                unpacked_operand = b1 << 24 | b2 << 16 | b3 << 8 | b4;
            } else if(b0 == 30) {
                // Floating point. Two digits per byte, terminated by a nibble with all bits set.
                uint8_t b1;
                do {
                    b1 = (uint8_t)dataspan.at(offset++);
                } while((b1 & 0xf) != 0xf && (b1 >> 4) != 0xf);
                unpacked_operand = -1; // FIXME
            } else {
                std::abort();
            }
            operands.push_back(unpacked_operand);
        }
    }
    if(!operands.empty()) {
        RETERR(MalformedFontFile);
    }
    return dict;
}

const CFFDictItem *find_command(const CFFDict &dict, DictOperator op) {
    for(const auto &entry : dict.entries) {
        if(entry.opr == op) {
            return &entry;
        }
    }
    return nullptr;
}

const CFFDictItem *find_command(const CFFont &f, DictOperator op) {
    return find_command(f.top_dict, op);
}

rvoe<std::vector<CFFCharsetRange2>> unpack_charsets(const CFFont &f,
                                                    std::span<std::byte> dataspan) {
    if(dataspan.empty()) {
        RETERR(MalformedFontFile);
    }
    std::vector<CFFCharsetRange2> charset;
    size_t offset = 0;
    const auto format = (uint8_t)dataspan[offset++];
    if(format == 0) {
        const auto num_glyphs = (int32_t)f.char_strings.size() - 1;
        assert(num_glyphs >= 0);
        std::vector<uint16_t> glyphlist;
        for(int32_t i = 0; i < num_glyphs; ++i) {
            ERC(value, extract<uint16_t>(dataspan, offset + i * sizeof(uint16_t)));
            value = std::byteswap(value);
            glyphlist.push_back(value);
        }
        // FIXME do something with this.
        charset.push_back(CFFCharsetRange2{1, (uint16_t)(glyphlist.size() - 1)});
    } else if(format == 1) {
        ERC(rng, extract<CFFCharsetRange1>(dataspan, offset));
        rng.swap_endian();
        charset.emplace_back(CFFCharsetRange2{rng.first, rng.nLeft});
    } else {
        ERC(rng, extract<CFFCharsetRange2>(dataspan, offset));
        rng.swap_endian();
        charset.push_back(rng);
    }
    return charset;
}

rvoe<std::vector<CFFSelectRange3>> unpack_fdselect(std::span<std::byte> dataspan,
                                                   uint32_t num_glyphs) {
    std::vector<CFFSelectRange3> ranges;
    size_t offset = 0;
    if(dataspan.empty()) {
        RETERR(MalformedFontFile);
    }
    const uint8_t format = (uint8_t)dataspan[offset++];
    if(format == 0) {
        // One font dictionary number for each glyph. Stored as ranges, like format 3.
        ERC(selector_array, safe_subspan(dataspan, offset, num_glyphs));
        for(uint32_t i = 0; i < num_glyphs; ++i) {
            const auto fd = (uint8_t)selector_array[i];
            if(ranges.empty() || ranges.back().fd != fd) {
                ranges.push_back(CFFSelectRange3{(uint16_t)i, fd});
            }
        }
    } else if(format == 3) {
        ERC(nRg, extract_and_swap<uint16_t>(dataspan, offset));
        const uint32_t nRanges = nRg;
        offset += sizeof(uint16_t);
        ranges.reserve(nRanges);
        for(uint32_t i = 0; i < nRanges; ++i) {
            ERC(range, extract<CFFSelectRange3>(dataspan, offset));
            offset += sizeof(range);
            range.swap_endian();
            ranges.push_back(range);
        }
        ERC(sentinel, extract_and_swap<uint16_t>(dataspan, offset));
        if(sentinel != num_glyphs) {
            RETERR(MalformedFontFile);
        }
    } else {
        RETERR(UnsupportedFormat);
    }
    // Looking up the font dictionary of a glyph relies on these.
    if(num_glyphs > 0 && (ranges.empty() || ranges.front().first != 0)) {
        RETERR(MalformedFontFile);
    }
    for(size_t i = 1; i < ranges.size(); ++i) {
        if(ranges[i].first <= ranges[i - 1].first) {
            RETERR(MalformedFontFile);
        }
    }
    return ranges;
}

void print_string(const CFFont &cff, int32_t string_number) {
    if(string_number < NUM_STANDARD_STRINGS) {
        printf("<standard string %d>\n", string_number);
    } else {
        const uint32_t strindex = string_number - NUM_STANDARD_STRINGS;
        auto sv = span2sv(cff.string.entries.at(strindex));
        std::string tmp{sv};
        printf("%s\n", tmp.c_str());
    }
}

void print_string_for_operator(const CFFont &cff, DictOperator op) {
    auto *tmp = find_command(cff, op);
    assert(tmp->operand.size() == 1);
    print_string(cff, tmp->operand[0]);
}

void print_info(const CFFont &cff) {
    printf("Font name: ");
    print_string_for_operator(cff, DictOperator::FullName);
    printf("Family name: ");
    print_string_for_operator(cff, DictOperator::FamilyName);
    printf("Weight: ");
    print_string_for_operator(cff, DictOperator::Weight);
}

void append_offset(std::vector<std::byte> &output, uint32_t value, int offsize) {
    for(int i = offsize - 1; i >= 0; --i) {
        output.push_back(std::byte((value >> (8 * i)) & 0xFF));
    }
}

// Returns the positions in the output where the entries start.
std::vector<uint32_t> append_index_to(std::vector<std::byte> &output, const CFFIndex &index) {
    swap_and_append_bytes<uint16_t>(output, index.size());
    std::vector<uint32_t> offsets;
    if(index.entries.empty()) {
        // An empty index is only the count.
        return offsets;
    }
    uint64_t total_size = 1; // Offsets start from one.
    for(const auto &e : index.entries) {
        total_size += e.size_bytes();
    }
    // The offset array is as compact as the sizes permit.
    int offsize = 4;
    if(total_size <= 0xFF) {
        offsize = 1;
    } else if(total_size <= 0xFFFF) {
        offsize = 2;
    } else if(total_size <= 0xFFFFFF) {
        offsize = 3;
    }
    output.push_back(std::byte(offsize));
    uint32_t offset = 1;
    for(const auto &e : index.entries) {
        append_offset(output, offset, offsize);
        offset += e.size_bytes();
    }
    append_offset(output, offset, offsize);
    for(const auto &e : index.entries) {
        offsets.push_back(output.size());
        append_bytes(output, e);
    }
    return offsets;
}

rvoe<CFFPrivateDict>
load_private_dict(std::span<std::byte> dataspan, size_t dict_offset, size_t dict_size) {
    CFFPrivateDict pdict;
    if(dict_offset > dataspan.size_bytes() || dict_offset + dict_size > dataspan.size_bytes()) {
        RETERR(MalformedFontFile);
    }
    ERC(dictspan, safe_subspan(dataspan, dict_offset, dict_size));
    ERC(raw_pdict, unpack_dictionary(dictspan));
    for(const auto &e : raw_pdict.entries) {
        if(e.opr == DictOperator::Subrs) {
            if(e.operand.empty()) {
                RETERR(MalformedFontFile);
            }
            const auto &subr_offset = e.operand.front();
            size_t from_the_top = dict_offset + subr_offset;
            ERC(subr_index, load_index(dataspan, from_the_top));
            pdict.subr = std::move(subr_index);
        } else {
            pdict.entries.entries.emplace_back(std::move(e));
        }
    }
    return pdict;
}

rvoe<CFFFontDict> load_fdarray_entry(std::span<std::byte> dataspan, std::span<std::byte> dstr) {
    ERC(raw_entries, unpack_dictionary(dstr));
    CFFFontDict fdict;
    for(const auto &e : raw_entries.entries) {
        if(e.opr == DictOperator::Private) {
            if(e.operand.size() != 2) {
                RETERR(MalformedFontFile);
            }
            const auto &local_dsize = e.operand.front();
            const auto &local_offset = e.operand.back();
            ERC(priv, load_private_dict(dataspan, local_offset, local_dsize));
            fdict.priv = std::move(priv);
        } else {
            fdict.entries.entries.emplace_back(std::move(e));
        }
    }
    return fdict;
}

// Writes the private dict followed by its local subroutines, if any are wanted.
// Returns the size of the dictionary.
size_t
write_private_dict(std::vector<std::byte> &output, const CFFPrivateDict &pd, bool include_subrs) {
    CFFDictWriter w;
    for(const auto &e : pd.entries.entries) {
        const auto opr = e.opr;
        if(opr == DictOperator::Subrs || opr == DictOperator::Encoding) {
            // We output only CID fonts, which are not allowed to have these operators in them.
            continue;
        }
        w.append_command(e);
    }
    const bool write_subrs = include_subrs && pd.subr;
    if(write_subrs) {
        // This is always last, so we know the layout.
        CFFDictItem e;
        e.operand.push_back(w.current_size() + 1 + 4 + 1);
        e.opr = DictOperator::Subrs;
        w.append_command(e);
    }
    auto bytes = w.steal();
    output.insert(output.end(), bytes.output.cbegin(), bytes.output.cend());
    if(write_subrs) {
        append_index_to(output, pd.subr.value());
    }
    return bytes.output.size();
}

} // namespace

void CFFSelectRange3::swap_endian() { first = std::byteswap(first); }

void CFFCharsetRange1::swap_endian() { first = std::byteswap(first); }

void CFFCharsetRange2::swap_endian() {
    first = std::byteswap(first);
    nLeft = std::byteswap(nLeft);
}

rvoe<CFFont> parse_cff_data(DataSource source) {
    CFFont f;
    f.original_data = std::move(source);
    ERC(dataspan, span_of_source(f.original_data));
    ERC(h, extract<CFFHeader>(dataspan, 0));
    if(h.major != 1) {
        RETERR(UnsupportedFormat);
    }
    if(h.minor != 0) {
        RETERR(UnsupportedFormat);
    }
    f.header = h;
    if(f.header.hdrsize != 4) {
        RETERR(MalformedFontFile);
    }
    if(f.header.offsize == 0 || f.header.offsize >= 5) {
        RETERR(MalformedFontFile);
    }
    size_t offset = f.header.hdrsize;
    ERC(name_index, load_index(dataspan, offset));
    f.name = std::move(name_index);
    ERC(topdict_index, load_index(dataspan, offset));
    f.top_dict_data = std::move(topdict_index);
    if(f.top_dict_data.entries.empty()) {
        RETERR(MalformedFontFile);
    }
    ERC(tc, unpack_dictionary(f.top_dict_data.entries.front()));
    if(tc.entries.empty()) {
        RETERR(MalformedFontFile);
    }
    f.top_dict = std::move(tc);
    ERC(string_index, load_index(dataspan, offset));
    f.string = std::move(string_index);
    ERC(global_subr_index, load_index(dataspan, offset));
    f.global_subr = std::move(global_subr_index);

    if(false) {
        print_info(f);
    }
    if(auto *cst = find_command(f, DictOperator::CharstringType)) {
        // Only Type 2 charstrings (the default) are supported.
        if(cst->operand.size() != 1 || cst->operand.front() != 2) {
            RETERR(UnsupportedFormat);
        }
    }
    auto *cse = find_command(f, DictOperator::CharStrings);
    if(!cse) {
        RETERR(UnsupportedFormat);
    }
    if(cse->operand.size() != 1) {
        RETERR(UnsupportedFormat);
    }
    offset = cse->operand.front();
    ERC(cstring, load_index(dataspan, offset));
    f.char_strings = std::move(cstring);

    auto *ence = find_command(f, DictOperator::Encoding);
    if(ence) {
        f.predefined_encoding = ence->operand.front();
    }
    auto *cste = find_command(f, DictOperator::Charset);
    if(!cste) {
        RETERR(UnsupportedFormat);
    }
    if(cste->operand.size() != 1) {
        RETERR(MalformedFontFile);
    }
    if((size_t)cste->operand[0] > dataspan.size_bytes()) {
        RETERR(MalformedFontFile);
    }
    ERC(charsets, unpack_charsets(f, dataspan.subspan(cste->operand[0])));
    f.charsets = std::move(charsets);

    auto *priv = find_command(f, DictOperator::Private);
    if(priv) {
        if(priv->operand.size() != 2 || priv->operand[0] < 0 || priv->operand[1] < 0) {
            RETERR(MalformedFontFile);
        }
        auto dict_offset = priv->operand[1];
        auto dict_size = priv->operand[0];
        ERC(pdict_span, safe_subspan(dataspan, dict_offset, dict_size));
        ERC(raw_pdict, unpack_dictionary(pdict_span));
        ERC(processed_pdict, build_pdict_from_dict(dataspan, dict_offset, raw_pdict));
        f.pdict = std::move(processed_pdict);
    }

    auto *fda = find_command(f, DictOperator::FDArray);
    auto *fds = find_command(f, DictOperator::FDSelect);
    if(fda) {
        f.is_cid = true;
    } else {
        f.is_cid = false;
    }
    if(f.is_cid) {
        if(!fda || fda->operand.empty()) {
            RETERR(UnsupportedFormat);
        }
        offset = fda->operand[0];
        ERC(fdastr, load_index(dataspan, offset))
        for(const auto dstr : fdastr.entries) {
            ERC(fdentry, load_fdarray_entry(dataspan, dstr));
            f.fdarray.emplace_back(std::move(fdentry));
        }
        if(!fds || fds->operand.empty()) {
            RETERR(UnsupportedFormat);
        }
        offset = fds->operand[0];
        if(offset > dataspan.size_bytes()) {
            RETERR(MalformedFontFile);
        }
        ERC(fdsstr, unpack_fdselect(dataspan.subspan(offset), f.char_strings.size()));
        f.fdselect = std::move(fdsstr);
    }
    return f;
}

uint8_t CFFont::get_fontdict_id(uint16_t glyph_id) const {
    if(!is_cid) {
        return 0;
    }
    assert(!fdselect.empty());
    // The ranges are in ascending order and the first one starts from zero.
    // Find the last one that starts at or before the glyph.
    auto it = std::upper_bound(
        fdselect.begin(), fdselect.end(), glyph_id, [](uint16_t gid, const CFFSelectRange3 &r) {
            return gid < r.first;
        });
    assert(it != fdselect.begin());
    return std::prev(it)->fd;
}

const CFFIndex *CFFont::local_subrs(uint8_t fontdict_id) const {
    if(!is_cid) {
        return pdict.subr ? &pdict.subr.value() : nullptr;
    }
    if(fontdict_id >= fdarray.size()) {
        return nullptr;
    }
    const auto &fd = fdarray[fontdict_id];
    return fd.priv && fd.priv->subr ? &fd.priv->subr.value() : nullptr;
}

rvoe<CFFont> parse_cff_file(const char *fname) {
    ERC(source, mmap_file(fname));
    return parse_cff_data(std::move(source));
}

void CFFDictWriter::append_command(const CFFDictItem &e) {
    if(e.raw_operands.empty()) {
        append_command(e.operand, e.opr);
        return;
    }
    o.offsets.push_back((uint16_t)o.output.size());
    o.output.insert(o.output.end(), e.raw_operands.begin(), e.raw_operands.end());
    if((uint16_t)e.opr > 0xFF) {
        o.output.push_back(std::byte{0xc});
    }
    o.output.push_back(std::byte((uint16_t)e.opr & 0xFF));
}

void CFFDictWriter::append_command(const std::vector<int32_t> &operands, DictOperator op) {
    o.offsets.push_back(o.output.size());
    for(const auto opr : operands) {
        o.output.push_back(std::byte{29});
        swap_and_append_bytes(o.output, opr);
    }
    if((uint16_t)op > 0xFF) {
        o.output.push_back(std::byte{0xc});
    }
    o.output.push_back(std::byte((uint16_t)op & 0xFF));
}

namespace {

// Interprets Type 2 charstrings only far enough to be able to copy them while replacing
// the subroutine calls with the called code. The operators are described in
// Adobe Technical Note #5177.
class CharstringExpander {
public:
    CharstringExpander(const CFFIndex &global_subrs,
                       const CFFIndex *local_subrs,
                       std::vector<std::byte> &result)
        : global_subrs{global_subrs}, local_subrs{local_subrs}, out{result} {}

    bool run(std::span<const std::byte> code, int depth);

private:
    // An operand that has been pushed to the stack and written to the output.
    struct Operand {
        bool is_integer;
        int32_t value;
        size_t out_start; // Where its bytes are in the output.
        size_t out_end;
    };

    // Subroutine numbers are stored relative to a bias that depends on the number of subroutines.
    static int64_t bias(size_t num_subrs) {
        if(num_subrs < 1240) {
            return 107;
        }
        if(num_subrs < 33900) {
            return 1131;
        }
        return 32768;
    }

    bool call(const CFFIndex *subrs, int depth);

    // Limits that stop broken fonts, such as ones where a subroutine calls itself,
    // from consuming all resources. Real glyphs are nowhere near these.
    static constexpr int max_depth = 32;
    static constexpr size_t max_stack = 513;
    static constexpr size_t max_output = 4 * 1024 * 1024;

    const CFFIndex &global_subrs;
    const CFFIndex *local_subrs;
    std::vector<std::byte> &out;
    std::vector<Operand> stack;
    // The number of stem hints so far, needed to know the size of the mask of
    // a hintmask operator. They can be declared in a different subroutine than the mask.
    size_t num_stems = 0;
    int calls_left = 100000;
    bool done = false; // Endchar has been seen.
};

bool CharstringExpander::call(const CFFIndex *subrs, int depth) {
    if(!subrs || stack.empty()) {
        return false;
    }
    // The number of the subroutine is the previous operand. Unless it is one
    // that can just be dropped (it is the last thing that was written), we don't know it.
    const Operand top = stack.back();
    if(!top.is_integer || top.out_end != out.size()) {
        return false;
    }
    const int64_t index = (int64_t)top.value + bias(subrs->size());
    if(index < 0 || index >= (int64_t)subrs->size()) {
        return false;
    }
    if(depth >= max_depth || calls_left <= 0) {
        return false;
    }
    --calls_left;
    stack.pop_back();
    out.resize(top.out_start);
    return run(subrs->entries[index], depth + 1);
}

bool CharstringExpander::run(std::span<const std::byte> code, int depth) {
    size_t i = 0;
    const size_t n = code.size();
    while(i < n && !done) {
        if(out.size() > max_output) {
            return false;
        }
        const uint8_t b0 = (uint8_t)code[i];
        if(b0 >= 32 || b0 == 28) {
            // An operand.
            size_t len = 1;
            int32_t value = 0;
            bool is_integer = true;
            if(b0 == 28) {
                len = 3;
                if(i + len > n) {
                    return false;
                }
                value = (int16_t)(((uint8_t)code[i + 1] << 8) | (uint8_t)code[i + 2]);
            } else if(b0 <= 246) {
                value = (int32_t)b0 - 139;
            } else if(b0 <= 250) {
                len = 2;
                if(i + len > n) {
                    return false;
                }
                value = ((int32_t)b0 - 247) * 256 + (uint8_t)code[i + 1] + 108;
            } else if(b0 <= 254) {
                len = 2;
                if(i + len > n) {
                    return false;
                }
                value = -((int32_t)b0 - 251) * 256 - (uint8_t)code[i + 1] - 108;
            } else {
                // 16.16 fixed point number.
                len = 5;
                is_integer = false;
                if(i + len > n) {
                    return false;
                }
            }
            if(stack.size() >= max_stack) {
                return false;
            }
            stack.push_back(Operand{is_integer, value, out.size(), out.size() + len});
            out.insert(out.end(), code.begin() + i, code.begin() + i + len);
            i += len;
            continue;
        }
        ++i;
        switch(b0) {
        case 1:  // hstem
        case 3:  // vstem
        case 18: // hstemhm
        case 23: // vstemhm
            num_stems += stack.size() / 2;
            stack.clear();
            out.push_back(std::byte(b0));
            break;
        case 19:   // hintmask
        case 20: { // cntrmask
            // If there are operands, they are the arguments of an implicit vstem.
            num_stems += stack.size() / 2;
            stack.clear();
            out.push_back(std::byte(b0));
            const size_t mask_size = (num_stems + 7) / 8;
            if(i + mask_size > n) {
                return false;
            }
            out.insert(out.end(), code.begin() + i, code.begin() + i + mask_size);
            i += mask_size;
            break;
        }
        case 4:  // vmoveto
        case 5:  // rlineto
        case 6:  // hlineto
        case 7:  // vlineto
        case 8:  // rrcurveto
        case 21: // rmoveto
        case 22: // hmoveto
        case 24: // rcurveline
        case 25: // rlinecurve
        case 26: // vvcurveto
        case 27: // hhcurveto
        case 30: // vhcurveto
        case 31: // hvcurveto
            stack.clear();
            out.push_back(std::byte(b0));
            break;
        case 10: // callsubr
            if(!call(local_subrs, depth)) {
                return false;
            }
            break;
        case 29: // callgsubr
            if(!call(&global_subrs, depth)) {
                return false;
            }
            break;
        case 11: // return
            return true;
        case 14: // endchar
            if(stack.size() >= 4) {
                // The deprecated form that draws an accented character by
                // combining two other glyphs that are specified by number. We don't
                // know what those glyphs are and so can't make sure they exist.
                return false;
            }
            stack.clear();
            out.push_back(std::byte(b0));
            done = true;
            break;
        case 12: { // Two byte operators.
            if(i >= n) {
                return false;
            }
            const uint8_t b1 = (uint8_t)code[i++];
            switch(b1) {
            case 0:  // dotsection (deprecated)
            case 34: // hflex
            case 35: // flex
            case 36: // hflex1
            case 37: // flex1
                stack.clear();
                out.push_back(std::byte(b0));
                out.push_back(std::byte(b1));
                break;
            default:
                // Arithmetic and storage operators. They can make the numbers of the
                // subroutines unknown. No real font seems to use them.
                return false;
            }
            break;
        }
        default:
            // Reserved operators.
            return false;
        }
    }
    return true;
}

} // namespace

bool expand_charstring_subroutines(std::span<const std::byte> charstring,
                                   const CFFIndex &global_subrs,
                                   const CFFIndex *local_subrs,
                                   std::vector<std::byte> &result) {
    result.clear();
    CharstringExpander expander(global_subrs, local_subrs, result);
    return expander.run(charstring, 0);
}

CFFWriter::CFFWriter(const CFFont &source_, const std::vector<SubsetGlyphs> &sub_)
    : source{source_}, sub{sub_} {
    output.reserve(100 * 1024);
}

rvoe<NoReturnValue> CFFWriter::create() {
    if(sub.empty() || sub.size() > 65535) {
        RETERR(IndexOutOfBounds);
    }
    for(const auto &g : sub) {
        if(g.gid >= source.char_strings.size()) {
            RETERR(IndexOutOfBounds);
        }
    }
    ERCV(plan_fontdicts());
    ERCV(prepare_charstrings());
    collect_strings();

    uint8_t header[4] = {1, 0, 4, 4};
    output.assign((std::byte *)header, (std::byte *)header + 4);
    append_index(source.name);

    ERCV(create_topdict());
    CFFIndex string_index;
    string_index.entries = strings;
    append_index(string_index);
    if(keep_subrs) {
        append_index(source.global_subr);
    } else {
        append_index(CFFIndex{});
    }
    fixups.charsets.value = output.size();
    append_charset();
    fixups.charstrings.value = output.size();
    append_charstrings();
    append_fdthings();
    patch_offsets();
    RETOK;
}

// Decides which font dictionaries of the source are needed and what their numbers are in the
// output. The ones that no glyph uses are not written. The order stays the same.
rvoe<NoReturnValue> CFFWriter::plan_fontdicts() {
    used_fontdicts.clear();
    glyph_fontdict.clear();
    glyph_fontdict.reserve(sub.size());
    std::vector<bool> used(256, false);
    for(const auto &g : sub) {
        // A font that is not in the CID format has only one font dictionary.
        const uint8_t fd = source.get_fontdict_id(g.gid);
        if(source.is_cid && fd >= source.fdarray.size()) {
            RETERR(MalformedFontFile);
        }
        used[fd] = true;
    }
    std::vector<uint8_t> new_number(256, 0);
    for(size_t fd = 0; fd < used.size(); ++fd) {
        if(used[fd]) {
            new_number[fd] = (uint8_t)used_fontdicts.size();
            used_fontdicts.push_back((uint8_t)fd);
        }
    }
    for(const auto &g : sub) {
        glyph_fontdict.push_back(new_number[source.get_fontdict_id(g.gid)]);
    }
    RETOK;
}

// Subroutines are shared between glyphs and typically most of them are needed by glyphs that
// are not in the subset, so the subset glyphs are rewritten not to use them.
// If that is not possible, we have to copy all subroutines as they are.
rvoe<NoReturnValue> CFFWriter::prepare_charstrings() {
    keep_subrs = false;
    expanded_charstrings.clear();
    expanded_charstrings.reserve(sub.size());
    for(size_t i = 0; i < sub.size(); ++i) {
        const auto &gid = sub[i].gid;
        const auto *local_subrs = source.local_subrs(source.get_fontdict_id(gid));
        std::vector<std::byte> expanded;
        if(!expand_charstring_subroutines(
               source.char_strings.entries.at(gid), source.global_subr, local_subrs, expanded)) {
            keep_subrs = true;
            expanded_charstrings.clear();
            RETOK;
        }
        expanded_charstrings.push_back(std::move(expanded));
    }
    RETOK;
}

// The strings are indexed from the first nonstandard one.
int32_t CFFWriter::string_id(std::string_view s) {
    for(size_t i = 0; i < strings.size(); ++i) {
        if(span2sv(strings[i]) == s) {
            return NUM_STANDARD_STRINGS + (int32_t)i;
        }
    }
    // The span points to the data of a string literal.
    strings.emplace_back((std::byte *)const_cast<char *>(s.data()), s.size());
    return NUM_STANDARD_STRINGS + (int32_t)strings.size() - 1;
}

void CFFWriter::collect_strings() {
    strings = source.string.entries;
    // Needed for the character collection (ROS) of the output. See create_topdict.
    string_id(registry_str);
    string_id(ordering_str);
}

rvoe<NoReturnValue> CFFWriter::create_topdict() {
    CFFDictWriter topdict;

    // The glyphs of the output are numbered from zero and also have their position as the CID.
    // This is the same as the identity mapping between CIDs and glyph ids
    // that is used in the PDF, whatever character collection the source belongs to.
    CFFDictItem ros;
    ros.opr = DictOperator::ROS;
    ros.operand.push_back(string_id(registry_str));
    ros.operand.push_back(string_id(ordering_str));
    ros.operand.push_back(0);
    topdict.append_command(ros);

    copy_dict_item_if_exists(topdict, DictOperator::Notice);
    copy_dict_item_if_exists(topdict, DictOperator::FullName);
    copy_dict_item_if_exists(topdict, DictOperator::FamilyName);
    copy_dict_item_if_exists(topdict, DictOperator::Weight);
    copy_dict_item_if_exists(topdict, DictOperator::FontBBox);
    // Without this a font whose glyphs are not defined in units of 1/1000 em
    // (such as one with 2048 units per em) is rendered at the wrong size.
    copy_dict_item_if_exists(topdict, DictOperator::FontMatrix);
    if(source.is_cid) {
        copy_dict_item_if_exists(topdict, DictOperator::CIDFontVersion);
        copy_dict_item_if_exists(topdict, DictOperator::CIDFontRevision);
    }
    int32_t cid_count = 0;
    if(auto *cc = find_command(source, DictOperator::CIDCount);
       source.is_cid && cc && cc->operand.size() == 1) {
        cid_count = cc->operand.front();
    }
    topdict.append_command({std::max<int32_t>(cid_count, (int32_t)sub.size())},
                           DictOperator::CIDCount);

    // The offsets of the following are not known yet. They are written with a fixed number of
    // bytes so they can be replaced when the data has been laid out.
    // The first byte of an operand is its type.
    const std::vector<int32_t> placeholder{-1};
    const size_t fdarray_pos = topdict.current_size() + 1;
    topdict.append_command(placeholder, DictOperator::FDArray);
    const size_t fdselect_pos = topdict.current_size() + 1;
    topdict.append_command(placeholder, DictOperator::FDSelect);
    const size_t charset_pos = topdict.current_size() + 1;
    topdict.append_command(placeholder, DictOperator::Charset);
    const size_t charstrings_pos = topdict.current_size() + 1;
    topdict.append_command(placeholder, DictOperator::CharStrings);

    auto serialization = topdict.steal();
    std::vector<std::vector<std::byte>> wrapper{std::move(serialization.output)};
    auto offsets = append_index(wrapper);
    assert(offsets.size() == 1);
    const auto dict_start = offsets.front();
    fixups.fdarray.offset = fdarray_pos + dict_start;
    fixups.fdselect.offset = fdselect_pos + dict_start;
    fixups.charsets.offset = charset_pos + dict_start;
    fixups.charstrings.offset = charstrings_pos + dict_start;
    RETOK;
}

void CFFWriter::copy_dict_item_if_exists(CFFDictWriter &w, DictOperator op) {
    auto *e = find_command(source, op);
    if(e) {
        // Copied as is, as it may contain real numbers.
        w.append_command(*e);
    }
}

void CFFWriter::append_fdthings() {
    std::vector<std::vector<std::byte>> fontdicts;
    std::vector<std::byte> privatedict_buffer; // Stores concatenated private dict/localsubr pairs.
    std::vector<size_t> privatedict_offsets;   // within the above buffer
    std::vector<size_t> privatereference_offsets; // where the correct value shall be written
    if(source.is_cid) {
        for(const auto font_dict_number : used_fontdicts) {
            const auto &source_dict = source.fdarray.at(font_dict_number);
            // Why is this so complicated you ask?
            // Because the data model is completely wacko.
            //
            // Each glyph has a "font" dictionary.
            // There can be only 256 font dictionaries total. Even with 65k glyphs.
            // Each of those point to a "private" dictionary.
            // Each of those point to "local subrs" index
            // That one is needed for rendering.
            //
            // The latter two are not stored in any global
            // index, they jost float around in the file.
            // If you don't read every single letter of
            // the (not particularly great) CFF spec with a
            // magnifying glass, you can't decipher that
            // and your subset fonts won't work.
            //
            // To make things simple:
            //
            // Any metadata that is somewhat shared
            // is copied as is so all indexes and offsets
            // work directly. Only character data is subset.
            // The same goes for the subroutines if they are not removed.

            privatedict_offsets.push_back(privatedict_buffer.size());
            CFFDictWriter fdarray_dict_writer;
            for(const auto &entry : source_dict.entries.entries) {
                fdarray_dict_writer.append_command(entry);
            }
            if(source_dict.priv) {
                const auto last_dict_size =
                    write_private_dict(privatedict_buffer, source_dict.priv.value(), keep_subrs);
                privatereference_offsets.push_back(fdarray_dict_writer.current_size() + 6);
                CFFDictItem e;
                e.operand.push_back(last_dict_size);
                e.operand.push_back(-1); // Offset from the beginning of the file, fixed below.
                e.opr = DictOperator::Private;
                fdarray_dict_writer.append_command(e);
            } else {
                privatereference_offsets.push_back(-1);
            }
            auto serialization = fdarray_dict_writer.steal();
            fontdicts.emplace_back(std::move(serialization.output));
        }
    } else {
        // Create a FDArray that has only one element that all glyphs use.
        CFFDictWriter fdarray_dict_writer;

        privatedict_offsets.push_back(privatedict_buffer.size());
        auto dict_size = write_private_dict(privatedict_buffer, source.pdict, keep_subrs);
        CFFDictItem e;
        privatereference_offsets.push_back(fdarray_dict_writer.current_size() + 6);
        // FIXME, add font name here.
        e.operand.push_back(dict_size);
        e.operand.push_back(-1);
        e.opr = DictOperator::Private;
        fdarray_dict_writer.append_command(e);
        auto serialization = fdarray_dict_writer.steal();
        fontdicts.emplace_back(std::move(serialization.output));
    }
    fixups.fdarray.value = output.size();
    auto fdarray_index_offsets = append_index(fontdicts);
    const auto privatedict_area_start = output.size();
    output.insert(output.end(), privatedict_buffer.begin(), privatedict_buffer.end());

    assert(fdarray_index_offsets.size() == privatereference_offsets.size());
    assert(fdarray_index_offsets.size() == privatedict_offsets.size());
    for(size_t i = 0; i < fdarray_index_offsets.size(); ++i) {
        const auto fdarray_index_offset = fdarray_index_offsets[i];
        const auto privatereference_offset = privatereference_offsets[i];
        const auto privatedict_offset = privatedict_offsets[i];
        const auto write_location = fdarray_index_offset + privatereference_offset;
        const uint32_t offset_value = privatedict_area_start + privatedict_offset;

        if(privatereference_offset == (size_t)-1) {
            continue;
        }
        uint32_t sanity_check;
        memcpy(&sanity_check, output.data() + write_location, sizeof(int32_t));
        if(sanity_check != (uint32_t)-1) {
            std::abort();
        }
        const auto offset_be = std::byteswap(offset_value);
        memcpy(output.data() + write_location, &offset_be, sizeof(int32_t));
    }

    // Now fdselect using the 16 bit format 3. Consecutive glyphs
    // that use the same font dictionary form one range.
    fixups.fdselect.value = output.size();
    std::vector<CFFSelectRange3> fdrange;
    for(size_t i = 0; i < sub.size(); ++i) {
        if(fdrange.empty() || fdrange.back().fd != glyph_fontdict[i]) {
            fdrange.push_back(CFFSelectRange3{(uint16_t)i, glyph_fontdict[i]});
        }
    }
    assert(fdrange.size() <= 65535);
    output.push_back(std::byte(3));
    swap_and_append_bytes(output, (uint16_t)fdrange.size());
    for(auto &fd : fdrange) {
        fd.swap_endian();
        append_bytes(output, fd);
    }
    swap_and_append_bytes(output, (uint16_t)sub.size());
}

void CFFWriter::patch_offsets() {
    write_fix(fixups.charsets);
    write_fix(fixups.charstrings);
    write_fix(fixups.fdselect);
    write_fix(fixups.fdarray);
}

void CFFWriter::write_fix(const OffsetPatch &p) {
    assert(p.offset != (uint32_t)-1);
    assert(p.value != (uint32_t)-1);
    const uint32_t value = std::byteswap(p.value);
    assert(p.offset + sizeof(uint32_t) < output.size());
    memcpy(output.data() + p.offset, &value, sizeof(uint32_t));
}

std::vector<uint32_t> CFFWriter::append_index(const CFFIndex &index) {
    return append_index_to(output, index);
}

std::vector<uint32_t> CFFWriter::append_index(const std::vector<std::vector<std::byte>> &entries) {
    CFFIndex converter;
    converter.entries.reserve(entries.size());
    for(const auto &e : entries) {
        converter.entries.emplace_back((std::byte *)e.data(), e.size());
    }
    return append_index(converter);
}

// The glyph with the number zero is not listed. Its CID is always zero.
// The other glyphs have their position as the CID, so the whole thing is one range.
void CFFWriter::append_charset() {
    if(sub.size() < 2) {
        output.emplace_back(std::byte(0)); // Format 0 without any glyphs.
        return;
    }
    output.emplace_back(std::byte(2));
    swap_and_append_bytes<uint16_t>(output, 1);
    swap_and_append_bytes<uint16_t>(output, (uint16_t)(sub.size() - 2));
}

void CFFWriter::append_charstrings() {
    CFFIndex subdata;
    subdata.entries.reserve(sub.size());
    for(size_t i = 0; i < sub.size(); ++i) {
        if(keep_subrs) {
            subdata.entries.push_back(source.char_strings.entries.at(sub[i].gid));
        } else {
            subdata.entries.emplace_back(expanded_charstrings[i].data(),
                                         expanded_charstrings[i].size());
        }
    }
    append_index(subdata);
}

} // namespace capypdf::internal
