#pragma once

#include <string>
#include <vector>

namespace animesama::txt {

std::string trim(const std::string& s);
std::string lower(std::string s);
bool starts_with(const std::string& s, const std::string& prefix);
bool ends_with(const std::string& s, const std::string& suffix);
bool contains_ci(const std::string& hay, const std::string& needle);
void replace_all(std::string& s, const std::string& from, const std::string& to);

bool between(const std::string& s, const std::string& a, const std::string& b, std::string& out, size_t from = 0,
             size_t* end_pos = nullptr);

std::string enclosing_tag(const std::string& html, size_t pos);
bool find_tag(const std::string& html, const std::string& needle, size_t from, std::string& tag,
              size_t* tag_end = nullptr);
std::string attr(const std::string& tag, const std::string& name);
std::string inner_text(const std::string& html, size_t tag_end, const std::string& tag_name);

std::string strip_tags(const std::string& s);
std::string html_decode(const std::string& s);
std::string collapse_ws(const std::string& s);
std::string strip_js_comments(const std::string& s);
std::string clean_query(const std::string& s);

std::string url_encode(const std::string& s);
std::string url_origin(const std::string& url);
std::string url_host(const std::string& url);
std::string url_resolve(const std::string& base, const std::string& ref);
std::string rstrip_char(std::string s, char c);
std::string lstrip_char(const std::string& s, char c);

std::vector<std::string> js_string_literals(const std::string& js);

}  // namespace animesama::txt
