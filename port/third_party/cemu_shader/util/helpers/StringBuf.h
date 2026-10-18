// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. See LICENSE.txt. Standalone replacement for Cemu StringBuf.
#pragma once
#include "Standalone.h"
class StringBuf {
    std::string value;
public:
    explicit StringBuf(uint32 capacity) { value.reserve(capacity); }
    void add(std::string_view s) { value.append(s); }
    template<class... Args> void addFmt(std::string_view f, const Args&... args) { value += shader_format(f,args...); }
    void reset() { value.clear(); }
    uint32 getLen() const { return uint32(value.size()); }
    const char* c_str() const { return value.c_str(); }
    void shrink_to_fit() { value.shrink_to_fit(); }
};
