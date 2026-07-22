#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace otoworm::util
{
    void debug_break();
    bool is_numeric(const char* s);	

    time_t get_last_modified_time(std::filesystem::path Path);
    std::string get_sha256_for_data(std::string_view data);
    std::string get_sha256_for_file(std::filesystem::path Filename);
    
    void normalize_filename(std::string &S, bool removeSlash, bool noAbsolute = true);

    std::string format(std::string str, ...);

    std::vector<std::string> token_split(const std::string &str, const std::string &token = ",", bool compress = false);

    std::string trim(std::string& str);
    std::string replace_all(std::string& str, const std::string& seq, const std::string what);
    std::string to_lower(std::string& str); // Caveat: only for ascii purposes.

    template <class T>
    std::string join(const T& iterable, const std::string& seq)
    {
        std::string ret;
        for (auto s = iterable.begin(); s != iterable.end(); ++s)
        {
            auto next = s; ++next;
            if (next != iterable.end())
                ret += *s + seq;
            else
                ret += *s;
        }

        return ret;
    }
}

namespace otoworm::locale {
    // Convert utf8 string into std::wstring.
    std::wstring widen(std::string Line);

	// Convert system locale string into std::wstring.
	std::wstring from_locale_str(std::string Line);

	// Convert system locale string into U8.
	std::string locale_to_u8(std::string line);

	// Convert std::wstring into utf8 std::string.
    std::string wstring_to_utf8(std::wstring Line);

	// Convert std::wstring into system locale std::string
	std::string to_locale_str(std::wstring Line);

	// Convert SHIFT-JIS std::string into UTF-8 std::string.
    std::string sjis_to_u8(std::string Line);
}

std::string int_to_str(int num);
std::string char_to_string(char c);


namespace otoworm::util
{
    int b36toi(const char* txt);
    int b16toi(const char* txt);
}

using otoworm::util::b36toi;
using otoworm::util::b16toi;

template <class F, class T>
T filter(F pred, const T &ctr)
{
	T rt_val;
	for (auto&& v: ctr)
	{
		if (pred(v))
			rt_val.insert(rt_val.cend(), v);
	}

	return rt_val;
}
