// SPDX-License-Identifier: Apache-2.0
// Copyright 2022-2024 Jussi Pakkanen

#pragma once

#include <errorhandling.hpp>
#include <pdfcommon.hpp>
#include <string>
#include <cstdio>
#include <filesystem>
#include <string_view>
#include <vector>

namespace capypdf::internal {

template<class... Ts> struct overloaded : Ts... {
    using Ts::operator()...;
};
#if defined __APPLE__
// This should not be needed, but Xcode 15 still requires it.
template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;
#endif

rvoe<std::vector<std::byte>> flate_compress(std::string_view data);
rvoe<std::vector<std::byte>> flate_compress(std::span<std::byte> data);

rvoe<std::string> load_file_as_string(const char *fname);
rvoe<std::string> load_file_as_string(FILE *f);

rvoe<std::vector<std::byte>> load_file_as_bytes(const std::string &fname);

rvoe<std::vector<std::byte>> load_file_as_bytes(FILE *f);

void write_file(const char *ofname, const char *buf, size_t bufsize);

// File names that capypdf gets from its API are UTF-8. On Windows the narrow file functions
// (fopen, std::filesystem::path(const char *)) read them in the ANSI code page instead, and a
// name with, say, Japanese letters in it cannot be opened. These convert the name to the
// native (UTF-16) form there; elsewhere they are the plain calls.
std::filesystem::path path_from_utf8(const char *utf8_name);
std::string path_to_utf8(const std::filesystem::path &path);
FILE *fopen_utf8(const char *utf8_name, const char *mode);

std::string utf8_to_pdfutf16be(const u8string &input, bool add_adornments = true);

bool is_valid_utf8(std::string_view input);

std::string current_date_string();

std::string pdfstring_quote(std::string_view raw_string);

// UTF-8 strings were added in PDF 2.0.
std::string u8str2u8textstring(std::string_view u8string);
std::string u8str2u8textstring(const u8string &str);
std::string u8str2filespec(const u8string &str);

std::string pdfname_quote(std::string_view raw_string);

bool is_ascii(std::string_view text);

std::string bytes2pdfstringliteral(std::string_view raw, bool add_slash = true);

std::string create_trailer_id();

class ObjectFormatter;
void serialize_trans(ObjectFormatter &fmt, const Transition &t);

void quote_xml_element_data_into(const u8string &content, std::string &result);
void quote_xml_element_data_unchecked_into(const std::string_view &content, std::string &result);

std::span<std::byte> str2span(const std::string &s);
std::string_view span2sv(std::span<std::byte> s);

} // namespace capypdf::internal
