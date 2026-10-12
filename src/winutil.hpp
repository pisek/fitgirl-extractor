// Windows-specific helpers of fitgirl-extractor.
#ifndef FITGIRL_WINUTIL_HPP
#define FITGIRL_WINUTIL_HPP

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>

namespace fg {

namespace fs = boost::filesystem;

struct fatal_error : std::runtime_error {
	explicit fatal_error(const std::string & what) : std::runtime_error(what) { }
};

std::wstring widen(const std::string & utf8);
std::string narrow(const std::wstring & wide);

// UTF-8 <-> the process ANSI code page (what unarc.dll's char* API uses).
// Throws fatal_error if the text is not representable in the ANSI code page.
std::string utf8_to_ansi(const std::string & utf8);
std::string ansi_to_utf8(const std::string & ansi);

std::string error_message(unsigned long code);

// Wine version if running under Wine, empty string otherwise.
std::string wine_version();

// Under Wine, maps a Unix path ("/Users/me/Game") to its DOS path ("Z:\Users\me\Game");
// anything else is returned unchanged.
fs::path host_path(const std::string & arg);

// Absolute path with backslashes, as a UTF-8 string.
std::string win(const fs::path & p);

std::string read_file(const fs::path & p);
void write_file(const fs::path & p, const std::string & data);
std::string md5_file(const fs::path & p);

fs::path self_exe();

struct job_result {
	bool timed_out = false;
	unsigned long exit_code = 0;
};

// Runs `exe args...` with stdout/stderr connected to a pipe, inside a job object that is
// terminated afterwards (this also stops any helper processes the child started).
// `on_line` is called for every output line (without the line break).
job_result run_in_job(const fs::path & exe, const std::vector<std::string> & args, unsigned long timeout_ms,
                      const std::function<void(const std::string &)> & on_line);

} // namespace fg

#endif // FITGIRL_WINUTIL_HPP
