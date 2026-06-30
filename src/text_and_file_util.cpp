#include <cstdlib>
#include <string>
#include <vector>
#include <regex>
#include <csignal>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <chrono>


#include "text_and_file_util.h"

#include "sha256/sha256.h"

namespace otoworm::util
{
    int b36toi(const char *txt)
    {
        return strtoul(txt, nullptr, 36);
    }

    int b16toi(const char *txt)
    {
        return strtoul(txt, nullptr, 16);
    }

    void debug_break()
    {
#ifndef NDEBUG
#if (defined WIN32) && !(defined MINGW)
        __debugbreak();
#else
        raise(SIGTRAP);
#endif
#endif
    }

#ifndef isdigit
    bool isdigit(const char s)
    {
        return s >= '0' && s <= '9';
    }
#endif

    bool is_numeric(const char* s)
    {
        // check for first character being a minus sign
        std::stringstream k;
        double d;
        k << s;
        k >> d;
        return !k.fail();
    }

    std::vector<std::string> token_split(const std::string& str, const std::string &token, const bool compress)
    {
        std::vector<std::string> ret;
        const size_t len = str.length();
        auto it = &str[0];
        auto next = strpbrk(str.c_str(), token.c_str()); // next token instance
        for (; next != nullptr; next = strpbrk(it, token.c_str()))
        {
            if (!compress || it - next != 0)
            {
                ret.push_back(str.substr(it - str.c_str(), next - it));
            }
            it = next + 1;
        }

        if (it != next && len)
        {
			ret.push_back(str.substr(it - str.c_str(), next - it));
        }
        return ret;
    }

    std::string trim(std::string& str)
    {
        const std::regex trimreg("^\\s*(.*?)\\s*$");
        return str = regex_replace(str, trimreg, "$1");
    }

    std::string replace_all(std::string& str, const std::string& seq, const std::string what)
    {
        return str = regex_replace(str, std::regex(seq), what);
    }

    std::string to_lower(std::string& str)
    {
        std::transform(str.begin(), str.end(), str.begin(), tolower);
        return str;
    }

    time_t get_last_modified_time(std::filesystem::path Path)
    {
		if (std::filesystem::exists(Path)) {
#ifndef WIN32
			auto a = std::filesystem::last_write_time(Path);
            const auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                a - decltype(a)::clock::now() + std::chrono::system_clock::now());
            return std::chrono::system_clock::to_time_t(systemTime);
#else
            // az: bah. fucking windows.
            struct _stat s{};
            _wstat(Path.c_str(), &s);
            return s.st_mtime;
#endif
		}
		else return -1;
    }

    void normalize_filename(std::string &S, const bool removeSlash, bool noAbsolute)
    {
        // size_t len = strlen(fn);
        /*if (!noAbsolute)
            replace_all(S, "[<>\\|?*]", "");
        else*/
            replace_all(S, "[<>\\|?*]", "");

        if (removeSlash)
            replace_all(S, "/", "");
    }

    std::string get_sha256_for_file(std::filesystem::path Filename)
    {
        SHA256 SHA;
		std::ifstream InStream(Filename, std::ios::in | std::ios::binary);
        unsigned char tmpbuf[256];

        if (!InStream.is_open())
            return "";

        while (!InStream.eof())
        {
            InStream.read((char*)tmpbuf, 256);
            const size_t cnt = InStream.gcount();

            SHA.add(tmpbuf, cnt);
        }

        return std::string(SHA.getHash());
    }

	std::vector<std::filesystem::path> get_file_listing(std::filesystem::path path)
	{
		std::vector<std::filesystem::path> out;

		if (!std::filesystem::exists(path)) return out;
		for (auto &p : std::filesystem::directory_iterator(path)) {
			out.push_back(p);
		}

		return out;
	}
    double latof(std::string s)
    {
        const char point = *localeconv()->decimal_point;

        if (s.find_first_of(point) == std::string::npos)
        {
            char toFind = '.';
            if (point == ',') toFind = '.';
            else if (point == '.') toFind = ',';

            const size_t idx = s.find_first_of(toFind);
            if (idx != std::string::npos)
                s[idx] = point;
        }

        return atof(s.c_str());
    }

} // namespace otoworm::util

std::string int_to_str(const int num)
{
    std::stringstream k;
    k << num;
    return k.str();
}

std::string char_to_string(const char c)
{
    std::stringstream k;
    k << c;
    return k.str();
}
