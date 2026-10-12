// Platform-independent helpers of fitgirl-extractor (unit-tested on the build host).
#ifndef FITGIRL_CORE_HPP
#define FITGIRL_CORE_HPP

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fg {

extern const char version[];

struct usage_error : std::runtime_error {
	explicit usage_error(const std::string & what) : std::runtime_error(what) { }
};

struct options {
	std::string pack;
	std::string dest;
	bool list = false;
	std::vector<std::string> only;
	std::vector<std::string> exclude;
	bool skip_bin_check = false;
	bool skip_verify = false;
	double timeout_hours = 6;
	bool verbose = false;
	bool help = false;
	bool show_version = false;
};

// Parses the user-facing command line (without the program name). Throws usage_error.
options parse_args(const std::vector<std::string> & args);

extern const char usage_text[];

std::string to_lower(std::string s);

// Sort order that puts fg-2.bin before fg-10.bin.
bool natural_less(const std::string & a, const std::string & b);

// Case-insensitive shell-style match supporting '*' and '?'.
bool glob_match(const std::string & pattern, const std::string & name);

// Keeps the names matching any `only` pattern (all if empty) and no `exclude` pattern, naturally sorted.
std::vector<std::string> select_names(std::vector<std::string> names, const std::vector<std::string> & only,
                                      const std::vector<std::string> & exclude);

// One line of a QuickSFV-style list: "<md5> *..\dir\file". `path` uses '/' separators and is
// relative to the directory of the list file (it may start with "../").
struct md5_entry {
	std::string hash; // lower-case hex
	std::string path;
};
std::vector<md5_entry> parse_md5_list(const std::string & contents);

// Replaces every occurrence of each key by its value.
std::string replace_all(std::string data, const std::vector<std::pair<std::string, std::string>> & replacements);

// For "cls-<name>.dll" (case-insensitive) returns "<name>" in lower case, otherwise "".
std::string cls_plugin_name(const std::string & filename);

bool contains_global_namespace(const std::string & data);

// Blanks every standalone "Global\" string (NUL-delimited) by zeroing its first byte.
// Returns the patched offsets.
std::vector<std::size_t> blank_global_namespace(std::string & data);

// Throws std::runtime_error if `image` is not a PE file.
bool pe_is_large_address_aware(const std::string & image);

// Line protocol between the orchestrating process and the unarc.dll worker.
struct worker_line {
	enum kind_t { file, error, info, result, other } kind = other;
	long long size = 0;     // file: size, info: first value
	long long value2 = 0;   // info: second value
	int code = 0;           // result
	std::string text;       // file: name, error: message, info: callback name, other: raw line
};
worker_line parse_worker_line(const std::string & line);

// Strips unarc's own "ERROR: " prefix from an error message.
std::string clean_error(const std::string & message);

// Quotes one argument for a Windows command line (CommandLineToArgvW rules).
std::string quote_arg(const std::string & arg);

} // namespace fg

#endif // FITGIRL_CORE_HPP
