#include "core.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace fg {

#ifndef FITGIRL_VERSION
#error "FITGIRL_VERSION must be defined by the build (see build.zig.zon)"
#endif

const char version[] = FITGIRL_VERSION;

const char usage_text[] =
	"Usage: fitgirl-extractor.exe [options] PACK_DIR\n"
	"\n"
	"Unpacks a FitGirl repack (setup.exe + fg-*.bin) without running its installer.\n"
	"PACK_DIR is the folder with setup.exe, the .bin files and (optionally) MD5\\.\n"
	"\n"
	"Options:\n"
	"  -d, --dest DIR      where to unpack the game (required unless --list)\n"
	"      --list          only list archive contents\n"
	"      --only GLOB     unpack only matching .bin files (repeatable)\n"
	"      --exclude GLOB  skip matching .bin files (repeatable), e.g. 'fg-selective-*'\n"
	"      --skip-bin-check  don't check the .bin files against MD5\\*.md5 first\n"
	"      --skip-verify   don't verify unpacked files against _Redist\\*.md5\n"
	"      --timeout HOURS per-archive timeout (default 6)\n"
	"  -v, --verbose       print every file and the worker's raw output\n"
	"  -h, --help          show this help\n"
	"      --version       show the version\n"
	"\n"
	"Under Wine, paths may be given as Unix paths (e.g. ~/Downloads/Game).\n"
	"Exit code: 0 if every archive unpacked and every listed file matched its MD5,\n"
	"1 if an archive failed or verification found problems, 2 on other errors.\n";

namespace {

bool take_value(const std::vector<std::string> & args, std::size_t & i, const std::string & name,
                std::string & value) {
	const std::string & arg = args[i];
	if(arg == name) {
		if(i + 1 >= args.size()) {
			throw usage_error("option " + name + " needs a value");
		}
		value = args[++i];
		return true;
	}
	if(name.size() > 2 && arg.compare(0, name.size() + 1, name + "=") == 0) {
		value = arg.substr(name.size() + 1);
		return true;
	}
	return false;
}

int lower(char c) {
	return std::tolower(static_cast<unsigned char>(c));
}

bool is_digit(char c) {
	return c >= '0' && c <= '9';
}

} // namespace

options parse_args(const std::vector<std::string> & args) {
	options o;
	bool positional_only = false;
	for(std::size_t i = 0; i < args.size(); i++) {
		const std::string & arg = args[i];
		std::string value;
		if(positional_only || arg.empty() || arg[0] != '-' || arg == "-") {
			if(!o.pack.empty()) {
				throw usage_error("only one pack directory can be given (got '" + o.pack + "' and '" + arg + "')");
			}
			o.pack = arg;
		} else if(arg == "--") {
			positional_only = true;
		} else if(take_value(args, i, "-d", value) || take_value(args, i, "--dest", value)) {
			o.dest = value;
		} else if(take_value(args, i, "--only", value)) {
			o.only.push_back(value);
		} else if(take_value(args, i, "--exclude", value)) {
			o.exclude.push_back(value);
		} else if(take_value(args, i, "--timeout", value)) {
			char * end = nullptr;
			o.timeout_hours = std::strtod(value.c_str(), &end);
			if(value.empty() || *end != '\0' || !(o.timeout_hours > 0)) {
				throw usage_error("--timeout needs a positive number of hours");
			}
		} else if(arg == "--list") {
			o.list = true;
		} else if(arg == "--skip-bin-check") {
			o.skip_bin_check = true;
		} else if(arg == "--skip-verify") {
			o.skip_verify = true;
		} else if(arg == "-v" || arg == "--verbose") {
			o.verbose = true;
		} else if(arg == "-h" || arg == "--help" || arg == "/?") {
			o.help = true;
		} else if(arg == "--version") {
			o.show_version = true;
		} else {
			throw usage_error("unknown option " + arg);
		}
	}
	if(o.help || o.show_version) {
		return o;
	}
	if(o.pack.empty()) {
		throw usage_error("no pack directory given");
	}
	if(!o.list && o.dest.empty()) {
		throw usage_error("--dest is required (or use --list)");
	}
	return o;
}

std::string to_lower(std::string s) {
	for(char & c : s) {
		c = char(lower(c));
	}
	return s;
}

bool natural_less(const std::string & a, const std::string & b) {
	std::size_t i = 0, j = 0;
	while(i < a.size() && j < b.size()) {
		if(is_digit(a[i]) && is_digit(b[j])) {
			std::size_t i_end = i, j_end = j;
			while(i_end < a.size() && is_digit(a[i_end])) i_end++;
			while(j_end < b.size() && is_digit(b[j_end])) j_end++;
			// compare numerically without overflow: skip leading zeros, then by length, then digits
			std::size_t i_nz = i, j_nz = j;
			while(i_nz + 1 < i_end && a[i_nz] == '0') i_nz++;
			while(j_nz + 1 < j_end && b[j_nz] == '0') j_nz++;
			std::size_t la = i_end - i_nz, lb = j_end - j_nz;
			if(la != lb) {
				return la < lb;
			}
			int c = a.compare(i_nz, la, b, j_nz, lb);
			if(c != 0) {
				return c < 0;
			}
			i = i_end, j = j_end;
		} else {
			int ca = lower(a[i]), cb = lower(b[j]);
			if(ca != cb) {
				return ca < cb;
			}
			i++, j++;
		}
	}
	if((a.size() - i) != (b.size() - j)) {
		return (a.size() - i) < (b.size() - j);
	}
	return a < b;
}

namespace {

// Matches one pattern element at pattern[p] ('?', a [class] or a literal) against c.
// Sets `next` to the index after the element.
bool match_element(const std::string & pattern, std::size_t p, char c, std::size_t & next) {
	if(pattern[p] == '?') {
		next = p + 1;
		return true;
	}
	if(pattern[p] == '[') {
		std::size_t i = p + 1;
		bool negate = i < pattern.size() && (pattern[i] == '!' || pattern[i] == '^');
		if(negate) {
			i++;
		}
		bool matched = false;
		bool first = true;
		for(; i < pattern.size() && (first || pattern[i] != ']'); i++, first = false) {
			if(i + 2 < pattern.size() && pattern[i + 1] == '-' && pattern[i + 2] != ']') {
				matched = matched || (lower(c) >= lower(pattern[i]) && lower(c) <= lower(pattern[i + 2]));
				i += 2;
			} else {
				matched = matched || lower(c) == lower(pattern[i]);
			}
		}
		if(i < pattern.size()) { // found the closing ']'
			next = i + 1;
			return matched != negate;
		}
		// no closing ']': treat '[' literally
	}
	next = p + 1;
	return lower(pattern[p]) == lower(c);
}

} // namespace

bool glob_match(const std::string & pattern, const std::string & name) {
	std::size_t p = 0, n = 0, star = std::string::npos, mark = 0;
	while(n < name.size()) {
		std::size_t next;
		if(p < pattern.size() && pattern[p] == '*') {
			star = p++, mark = n;
		} else if(p < pattern.size() && match_element(pattern, p, name[n], next)) {
			p = next, n++;
		} else if(star != std::string::npos) {
			p = star + 1, n = ++mark;
		} else {
			return false;
		}
	}
	while(p < pattern.size() && pattern[p] == '*') {
		p++;
	}
	return p == pattern.size();
}

std::vector<std::string> select_names(std::vector<std::string> names, const std::vector<std::string> & only,
                                      const std::vector<std::string> & exclude) {
	auto matches_any = [](const std::vector<std::string> & patterns, const std::string & name) {
		return std::any_of(patterns.begin(), patterns.end(),
		                   [&](const std::string & pattern) { return glob_match(pattern, name); });
	};
	std::vector<std::string> selected;
	for(const std::string & name : names) {
		if((only.empty() || matches_any(only, name)) && !matches_any(exclude, name)) {
			selected.push_back(name);
		}
	}
	std::sort(selected.begin(), selected.end(), natural_less);
	return selected;
}

std::vector<md5_entry> parse_md5_list(const std::string & contents) {
	std::vector<md5_entry> entries;
	std::size_t pos = 0;
	while(pos < contents.size()) {
		std::size_t end = contents.find('\n', pos);
		if(end == std::string::npos) {
			end = contents.size();
		}
		std::string line = contents.substr(pos, end - pos);
		pos = end + 1;
		std::size_t first = line.find_first_not_of(" \t\r");
		if(first == std::string::npos || line[first] == ';') {
			continue;
		}
		line = line.substr(first, line.find_last_not_of(" \t\r") - first + 1);
		std::size_t space = line.find_first_of(" \t");
		if(space != 32 || line.find_first_not_of("0123456789abcdefABCDEF") < 32) {
			continue;
		}
		std::size_t name_start = line.find_first_not_of(" \t", space);
		if(name_start == std::string::npos) {
			continue;
		}
		std::string path = line.substr(name_start);
		if(!path.empty() && path[0] == '*') {
			path.erase(0, 1);
		}
		std::replace(path.begin(), path.end(), '\\', '/');
		if(path.empty()) {
			continue;
		}
		entries.push_back(md5_entry{ to_lower(line.substr(0, 32)), path });
	}
	return entries;
}

std::string replace_all(std::string data, const std::vector<std::pair<std::string, std::string>> & replacements) {
	for(const auto & r : replacements) {
		if(r.first.empty()) {
			continue;
		}
		std::size_t pos = 0;
		while((pos = data.find(r.first, pos)) != std::string::npos) {
			data.replace(pos, r.first.size(), r.second);
			pos += r.second.size();
		}
	}
	return data;
}

std::string cls_plugin_name(const std::string & filename) {
	std::string name = to_lower(filename);
	const std::string prefix = "cls-", suffix = ".dll";
	if(name.size() <= prefix.size() + suffix.size() || name.compare(0, prefix.size(), prefix) != 0
	   || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
		return std::string();
	}
	return name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
}

static const char global_ns[] = "Global\\";

bool contains_global_namespace(const std::string & data) {
	return data.find(global_ns) != std::string::npos;
}

std::vector<std::size_t> blank_global_namespace(std::string & data) {
	const std::string needle = std::string(1, '\0') + global_ns + std::string(1, '\0');
	std::vector<std::size_t> patched;
	std::size_t pos = 0;
	while((pos = data.find(needle, pos)) != std::string::npos) {
		data[pos + 1] = '\0';
		patched.push_back(pos + 1);
		pos += needle.size() - 1;
	}
	return patched;
}

bool pe_is_large_address_aware(const std::string & image) {
	auto u32 = [&](std::size_t off) {
		const unsigned char * p = reinterpret_cast<const unsigned char *>(image.data()) + off;
		return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
	};
	if(image.size() < 0x40 || image.compare(0, 2, "MZ") != 0) {
		throw std::runtime_error("not a PE file");
	}
	std::size_t pe = u32(0x3c);
	if(pe > image.size() || image.size() - pe < 24 || image.compare(pe, 4, std::string("PE\0\0", 4)) != 0) {
		throw std::runtime_error("not a PE file");
	}
	unsigned characteristics = unsigned(static_cast<unsigned char>(image[pe + 22]))
	                           | unsigned(static_cast<unsigned char>(image[pe + 23])) << 8;
	return (characteristics & 0x20) != 0;
}

worker_line parse_worker_line(const std::string & raw) {
	std::string line = raw;
	while(!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
		line.pop_back();
	}
	worker_line w;
	w.text = line;
	auto field = [&](std::size_t & pos) {
		std::size_t end = line.find(' ', pos);
		std::string f = line.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
		pos = (end == std::string::npos) ? line.size() : end + 1;
		return f;
	};
	auto number = [](const std::string & s, long long & out) {
		char * end = nullptr;
		out = std::strtoll(s.c_str(), &end, 10);
		return !s.empty() && *end == '\0';
	};
	std::size_t pos = 0;
	std::string tag = field(pos);
	if(tag == "FILE") {
		std::string size = field(pos);
		if(number(size, w.size)) {
			w.kind = worker_line::file;
			w.text = line.substr(pos);
		}
	} else if(tag == "ERROR") {
		w.kind = worker_line::error;
		w.text = line.substr(pos);
	} else if(tag == "INFO") {
		std::string what = field(pos), a = field(pos), b = field(pos);
		if(!what.empty() && number(a, w.size) && number(b, w.value2)) {
			w.kind = worker_line::info;
			w.text = what;
		}
	} else if(tag == "RESULT") {
		long long code;
		if(number(line.substr(pos), code)) {
			w.kind = worker_line::result;
			w.code = int(code);
		}
	}
	return w;
}

std::string clean_error(const std::string & message) {
	std::string m = message;
	while(m.compare(0, 6, "ERROR:") == 0) {
		m.erase(0, m.find_first_not_of(' ', 6) == std::string::npos ? m.size() : m.find_first_not_of(' ', 6));
	}
	return m;
}

std::string quote_arg(const std::string & arg) {
	if(!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) {
		return arg;
	}
	std::string out = "\"";
	std::size_t backslashes = 0;
	for(char c : arg) {
		if(c == '\\') {
			backslashes++;
			continue;
		}
		// backslashes are literal unless they precede a quote
		out.append(c == '"' ? backslashes * 2 + 1 : backslashes, '\\');
		backslashes = 0;
		out += c;
	}
	out.append(backslashes * 2, '\\');
	out += '"';
	return out;
}

} // namespace fg
