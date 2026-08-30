#pragma once

// Minimal JSON value/parse/serialize for the MCP protocol layer.
// Self-contained (no dependencies beyond nall string/maybe), covers the
// JSON subset MCP uses: objects, arrays, strings (with \uXXXX escapes),
// numbers, booleans, null.

#include <nall/string.hpp>
#include <nall/maybe.hpp>

#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

using namespace nall;

namespace mcp {

struct Json {
  enum class Type { Null, Bool, Number, String, Array, Object };

  Type type = Type::Null;
  bool boolean = false;
  double number = 0;
  string str{};
  std::vector<Json> items{};                        // Array
  std::vector<std::pair<string, Json>> members{};   // Object

  static auto makeNull() -> Json { return {}; }
  static auto makeBool(bool v) -> Json { Json j; j.type = Type::Bool; j.boolean = v; return j; }
  static auto makeNumber(double v) -> Json { Json j; j.type = Type::Number; j.number = v; return j; }
  static auto makeString(string v) -> Json { Json j; j.type = Type::String; j.str = std::move(v); return j; }
  static auto makeArray() -> Json { Json j; j.type = Type::Array; return j; }
  static auto makeObject() -> Json { Json j; j.type = Type::Object; return j; }

  // --- object helpers ----------------------------------------------------
  auto set(string key, Json value) -> Json& {
    for(auto& [k, v] : members) if(k == key) { v = std::move(value); return *this; }
    members.emplace_back(std::move(key), std::move(value));
    return *this;
  }

  auto find(string_view key) const -> const Json* {
    for(auto& [k, v] : members) if(k == key) return &v;
    return nullptr;
  }

  auto getString(string_view key, string_view fallback = "") const -> string {
    if(auto j = find(key)) if(j->type == Type::String) return j->str;
    return string{fallback};
  }

  auto getBool(string_view key, bool fallback = false) const -> bool {
    if(auto j = find(key)) if(j->type == Type::Bool) return j->boolean;
    return fallback;
  }

  auto getNumber(string_view key, double fallback = 0) const -> double {
    if(auto j = find(key)) if(j->type == Type::Number) return j->number;
    if(auto j = find(key)) if(j->type == Type::Bool) return j->boolean;
    return fallback;
  }

  // --- printing ----------------------------------------------------------
  auto print() const -> string {
    string s;
    print(s);
    return s;
  }

  auto print(string& out) const -> void {
    switch(type) {
    case Type::Null:   out.append("null"); break;
    case Type::Bool:   out.append(boolean ? "true" : "false"); break;
    case Type::Number: {
      char buffer[64];
      if(number == (u64)number && number >= 0 && number < 9.0e18)
        snprintf(buffer, sizeof(buffer), "%llu", (unsigned long long)(u64)number);
      else
        snprintf(buffer, sizeof(buffer), "%.17g", number);
      out.append(buffer);
      break;
    }
    case Type::String: printString(out, str); break;
    case Type::Array: {
      out.append('[');
      for(u32 i = 0; i < items.size(); i++) { if(i) out.append(','); items[i].print(out); }
      out.append(']');
      break;
    }
    case Type::Object: {
      out.append('{');
      for(u32 i = 0; i < members.size(); i++) {
        if(i) out.append(',');
        printString(out, members[i].first);
        out.append(':');
        members[i].second.print(out);
      }
      out.append('}');
      break;
    }
    }
  }

  // --- parsing -----------------------------------------------------------
  static auto parse(string_view text) -> maybe<Json> {
    Cursor c{text.data(), text.size(), 0};
    Json value;
    if(!parseValue(c, value)) return nothing;
    skipWs(c);
    if(c.pos != c.size) return nothing;
    return value;
  }

private:
  struct Cursor { const char* s; u32 size; u32 pos; };

  static auto printString(string& out, string_view s) -> void {
    out.append('"');
    for(u32 i = 0; i < s.size(); i++) {
      auto c = (u8)s[i];
      switch(c) {
      case '"':  out.append("\\\""); break;
      case '\\': out.append("\\\\"); break;
      case '\b': out.append("\\b"); break;
      case '\f': out.append("\\f"); break;
      case '\n': out.append("\\n"); break;
      case '\r': out.append("\\r"); break;
      case '\t': out.append("\\t"); break;
      default:
        if(c < 0x20) {
          char buffer[8];
          snprintf(buffer, sizeof(buffer), "\\u%04x", c);
          out.append(buffer);
        } else out.append(s[i]);
      }
    }
    out.append('"');
  }

  static auto at(const Cursor& c, u32 offset = 0) -> char {
    return c.pos + offset < c.size ? c.s[c.pos + offset] : '\0';
  }

  static auto skipWs(Cursor& c) -> void {
    while(c.pos < c.size && (c.s[c.pos] == ' ' || c.s[c.pos] == '\t' || c.s[c.pos] == '\n' || c.s[c.pos] == '\r')) c.pos++;
  }

  static auto match(Cursor& c, string_view word) -> bool {
    if(c.pos + word.size() > c.size) return false;
    for(u32 i = 0; i < word.size(); i++) if(c.s[c.pos + i] != word[i]) return false;
    c.pos += word.size();
    return true;
  }

  static auto parseValue(Cursor& c, Json& out) -> bool {
    skipWs(c);
    if(c.pos >= c.size) return false;
    auto ch = c.s[c.pos];
    if(ch == '{') return parseObject(c, out);
    if(ch == '[') return parseArray(c, out);
    if(ch == '"') { out.type = Type::String; return parseString(c, out.str); }
    if(ch == 't') { if(match(c, "true"))  { out.type = Type::Bool; out.boolean = true;  return true; } return false; }
    if(ch == 'f') { if(match(c, "false")) { out.type = Type::Bool; out.boolean = false; return true; } return false; }
    if(ch == 'n') { if(match(c, "null"))  { out.type = Type::Null;                       return true; } return false; }
    return parseNumber(c, out);
  }

  static auto parseNumber(Cursor& c, Json& out) -> bool {
    u32 start = c.pos;
    if(at(c) == '-') c.pos++;
    bool any = false;
    while(at(c) >= '0' && at(c) <= '9') { c.pos++; any = true; }
    if(at(c) == '.') { c.pos++; while(at(c) >= '0' && at(c) <= '9') { c.pos++; any = true; } }
    if(at(c) == 'e' || at(c) == 'E') {
      c.pos++;
      if(at(c) == '+' || at(c) == '-') c.pos++;
      while(at(c) >= '0' && at(c) <= '9') c.pos++;
    }
    if(!any || c.pos == start) return false;
    char buffer[64];
    int n = 0;
    for(u32 i = start; i < c.pos && n < 63; i++) buffer[n++] = c.s[i];
    buffer[n] = 0;
    out.type = Type::Number;
    out.number = strtod(buffer, nullptr);
    return true;
  }

  static auto parseString(Cursor& c, string& out) -> bool {
    if(at(c) != '"') return false;
    c.pos++;
    out = {};
    while(c.pos < c.size) {
      auto ch = (u8)c.s[c.pos++];
      if(ch == '"') return true;
      if(ch != '\\') { out.append((char)ch); continue; }
      if(c.pos >= c.size) return false;
      auto e = (u8)c.s[c.pos++];
      switch(e) {
      case '"':  out.append('"');  break;
      case '\\': out.append('\\'); break;
      case '/':  out.append('/');  break;
      case 'b':  out.append('\b'); break;
      case 'f':  out.append('\f'); break;
      case 'n':  out.append('\n'); break;
      case 'r':  out.append('\r'); break;
      case 't':  out.append('\t'); break;
      case 'u': {
        if(c.pos + 4 > c.size) return false;
        u32 cp = 0;
        for(int i = 0; i < 4; i++) {
          auto h = (u8)c.s[c.pos++];
          cp <<= 4;
          if(h >= '0' && h <= '9') cp |= (u32)(h - '0');
          else if(h >= 'a' && h <= 'f') cp |= (u32)(h - 'a' + 10);
          else if(h >= 'A' && h <= 'F') cp |= (u32)(h - 'A' + 10);
          else return false;
        }
        //encode as UTF-8 (BMP; unpaired surrogates pass through as raw units)
        if(cp < 0x80) out.append((char)cp);
        else if(cp < 0x800) { out.append((char)(0xC0 | (cp >> 6))); out.append((char)(0x80 | (cp & 0x3F))); }
        else { out.append((char)(0xE0 | (cp >> 12))); out.append((char)(0x80 | ((cp >> 6) & 0x3F))); out.append((char)(0x80 | (cp & 0x3F))); }
        break;
      }
      default: return false;
      }
    }
    return false; //unterminated
  }

  static auto parseArray(Cursor& c, Json& out) -> bool {
    c.pos++; // '['
    out.type = Type::Array;
    out.items.clear();
    skipWs(c);
    if(at(c) == ']') { c.pos++; return true; }
    for(;;) {
      out.items.push_back(Json::makeNull());
      if(!parseValue(c, out.items.back())) return false;
      skipWs(c);
      if(at(c) == ',') { c.pos++; continue; }
      if(at(c) == ']') { c.pos++; return true; }
      return false;
    }
  }

  static auto parseObject(Cursor& c, Json& out) -> bool {
    c.pos++; // '{'
    out.type = Type::Object;
    out.members.clear();
    skipWs(c);
    if(at(c) == '}') { c.pos++; return true; }
    for(;;) {
      skipWs(c);
      if(at(c) != '"') return false;
      string key;
      if(!parseString(c, key)) return false;
      skipWs(c);
      if(at(c) != ':') return false;
      c.pos++;
      Json value;
      if(!parseValue(c, value)) return false;
      out.members.emplace_back(std::move(key), std::move(value));
      skipWs(c);
      if(at(c) == ',') { c.pos++; continue; }
      if(at(c) == '}') { c.pos++; return true; }
      return false;
    }
  }
};

}
