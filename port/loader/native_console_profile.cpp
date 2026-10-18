#include "native_console_profile.h"
#include <charconv>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {
constexpr uint32_t supported_regions = 0x77;
bool supported_mask(uint32_t value) {
    return value && !(value & ~supported_regions);
}
bool single_region(uint32_t value) {
    return supported_mask(value) && !(value & (value - 1));
}
std::string_view trim(std::string_view text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == text.npos) return {};
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}
uint32_t parse_number(std::string_view text, int base) {
    uint32_t value{};
    const auto result = std::from_chars(text.data(), text.data()+text.size(), value, base);
    if (text.empty() || result.ec != std::errc{} || result.ptr != text.data()+text.size())
        throw std::runtime_error("Invalid native region mask");
    return value;
}
std::string without_comments(std::string_view input) {
    std::string text(input);
    for (size_t at; (at=text.find("<!--"))!=text.npos;) {
        const auto end=text.find("-->",at+4);
        if (end==text.npos || text.find("<!--",at+4)<end)
            throw std::runtime_error("Malformed title metadata comment");
        text.erase(at,end+3-at);
    }
    // A scalar in CDATA is not an XML region element. This narrow metadata
    // reader does not implement CDATA or entity expansion.
    if (text.find("<![CDATA[")!=text.npos)
        throw std::runtime_error("CDATA is unsupported in native region metadata");
    return text;
}
bool space(char value) { return value==' ' || value=='\t' || value=='\r' || value=='\n'; }
bool name_start(char value) {
    return (value>='a' && value<='z') || (value>='A' && value<='Z') || value=='_';
}
void validate_opening(std::string_view tag) {
    size_t at=7; // after '<region'
    while (at<tag.size()-1) {
        if (!space(tag[at])) throw std::runtime_error("Malformed region attributes");
        while (at<tag.size()-1 && space(tag[at])) ++at;
        if (at==tag.size()-1) break;
        if (!name_start(tag[at])) throw std::runtime_error("Malformed region attribute name");
        ++at;
        while (at<tag.size()-1 && (name_start(tag[at]) || (tag[at]>='0' && tag[at]<='9') ||
            tag[at]=='-' || tag[at]==':' || tag[at]=='.')) ++at;
        while (at<tag.size()-1 && space(tag[at])) ++at;
        if (at==tag.size()-1 || tag[at++]!='=') throw std::runtime_error("Malformed region attribute");
        while (at<tag.size()-1 && space(tag[at])) ++at;
        if (at==tag.size()-1 || (tag[at]!='\"' && tag[at]!='\''))
            throw std::runtime_error("Unquoted region attribute");
        const char quote=tag[at++];
        while (at<tag.size()-1 && tag[at]!=quote) {
            if (tag[at++]=='<') throw std::runtime_error("Malformed region attribute value");
        }
        if (at==tag.size()-1) throw std::runtime_error("Unterminated region attribute");
        ++at;
    }
}
}

uint32_t rbrew_parse_title_regions(std::string_view metadata) {
    // The region element in Wii U title metadata is a hexBinary scalar. Keep
    // this deliberately narrow; unknown/ambiguous region data is never guessed.
    const auto cleaned=without_comments(metadata);
    metadata=cleaned;
    size_t begin = metadata.find("<region");
    while (begin != metadata.npos) {
        const auto after = begin + 7;
        if (after < metadata.size() &&
            (metadata[after]=='>' || metadata[after]==' ' || metadata[after]=='\t' ||
             metadata[after]=='\r' || metadata[after]=='\n')) break;
        begin = metadata.find("<region", after);
    }
    if (begin == metadata.npos) throw std::runtime_error("Title metadata has no region scalar");
    const auto body = metadata.find('>', begin);
    const auto end = metadata.find("</region>", body == metadata.npos ? begin : body+1);
    if (body == metadata.npos || end == metadata.npos || metadata.find("<region", end+9)!=metadata.npos)
        throw std::runtime_error("Title metadata has malformed or ambiguous region scalar");
    validate_opening(metadata.substr(begin,body-begin+1));
    const auto value = parse_number(trim(metadata.substr(body+1, end-body-1)), 16);
    if (!supported_mask(value)) throw std::runtime_error("Title metadata has unsupported region bits");
    return value;
}

RbrewNativeRegions rbrew_select_native_regions(uint32_t title_regions,
    std::optional<uint32_t> console_region) {
    if (!supported_mask(title_regions)) throw std::runtime_error("Invalid title region mask");
    const uint32_t selected = console_region.value_or(title_regions);
    if (!single_region(selected))
        throw std::runtime_error("Set RBREW_CONSOLE_REGION to a single Cafe region mask");
    return {selected, title_regions};
}

uint32_t rbrew_parse_console_region(std::string_view text) {
    text = trim(text);
    int base=10;
    if (text.starts_with("0x") || text.starts_with("0X")) { text.remove_prefix(2); base=16; }
    const auto value = parse_number(text, base);
    if (!single_region(value)) throw std::runtime_error("Invalid console region mask");
    return value;
}

RbrewNativeRegions rbrew_read_native_regions(const std::filesystem::path& path,
    std::optional<uint32_t> console_region) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot read title metadata; configure RBREW_TITLE_METADATA");
    // Metadata is small. Limit the read before allocating or parsing external input.
    constexpr size_t limit = 64*1024;
    std::string bytes;
    for (char byte; stream.get(byte);) {
        if (bytes.size()==limit) throw std::runtime_error("Title metadata exceeds 64 KiB profile limit");
        bytes.push_back(byte);
    }
    if (!stream.eof()) throw std::runtime_error("Title metadata read failed");
    return rbrew_select_native_regions(rbrew_parse_title_regions(bytes), console_region);
}

uint32_t rbrew_parse_native_uc_scalar(std::string_view text,uint32_t maximum) {
    text=trim(text);int base=10;
    if(text.starts_with("0x") || text.starts_with("0X")){text.remove_prefix(2);base=16;}
    const auto value=parse_number(text,base);
    if(value>maximum)throw std::runtime_error("Native UC scalar is outside its supported range");
    return value;
}
