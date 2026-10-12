// fitgirl-extractor: unpacks FitGirl repack .bin archives without running the Inno Setup installer.
//
//   1. checks the .bin files against MD5\*.md5 (if present),
//   2. unpacks the installer's files from setup.exe (embedded innoextract),
//   3. prepares FitGirl's tools (unarc.dll, CLS plugins, external compressors) and fixes known
//      incompatibilities,
//   4. unpacks each archive with unarc.dll in a worker process (worker.cpp),
//   5. copies the installer's regular files (e.g. _Redist) and verifies the result against
//      _Redist\*.md5 (if present).
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/exception.hpp>
#include <boost/filesystem/operations.hpp>

#include "cli/extract.hpp"
#include "release.hpp"
#include "setup/version.hpp"
#include "util/console.hpp"
#include "util/log.hpp"
#include "util/windows.hpp" // renames main() to utf8_main(), called with UTF-8 arguments

#include "core.hpp"
#include "winutil.hpp"
#include "worker.hpp"

namespace fg {

namespace {

const char work_dir_name[] = ".fgtmp";

void log(const std::string & message) {
	std::cout << "==> " << message << std::endl;
}

void warn(const std::string & message) {
	std::cout.flush();
	std::cerr << "WARNING: " << message << std::endl;
}

std::vector<fs::path> list_dir(const fs::path & dir, bool files) {
	std::vector<fs::path> entries;
	for(fs::directory_iterator it(dir), end; it != end; ++it) {
		if(files ? fs::is_regular_file(it->status()) : fs::is_directory(it->status())) {
			entries.push_back(it->path());
		}
	}
	return entries;
}

// Case-insensitive lookup of a file in a directory; empty path if missing.
fs::path find_file(const fs::path & dir, const std::string & name) {
	for(const fs::path & p : list_dir(dir, true)) {
		if(to_lower(p.filename().string()) == to_lower(name)) {
			return p;
		}
	}
	return fs::path();
}

std::string extension(const fs::path & p) {
	return to_lower(p.extension().string());
}

fs::path resolve_listed(const fs::path & list_file, const std::string & relative) {
	return (list_file.parent_path() / fs::path(relative)).lexically_normal().make_preferred();
}

// Absolute, without "." and ".." components, with backslashes.
fs::path full_path(const fs::path & p) {
	return fs::absolute(p).lexically_normal().make_preferred();
}

bool same_path(const fs::path & a, const fs::path & b) {
	return to_lower(win(a)) == to_lower(win(b));
}

void check_bins(const fs::path & pack, const std::vector<std::string> & archives) {
	fs::path md5_dir = pack / "MD5";
	std::vector<fs::path> lists;
	if(fs::is_directory(md5_dir)) {
		for(const fs::path & p : list_dir(md5_dir, true)) {
			if(extension(p) == ".md5") {
				lists.push_back(p);
			}
		}
	}
	if(lists.empty()) {
		warn("no MD5\\*.md5 in the pack directory; skipping .bin verification");
		return;
	}
	std::size_t checked = 0;
	for(const fs::path & list : lists) {
		for(const md5_entry & entry : parse_md5_list(read_file(list))) {
			fs::path path = resolve_listed(list, entry.path);
			bool wanted = std::any_of(archives.begin(), archives.end(),
			                          [&](const std::string & name) { return same_path(path, pack / name); });
			if(!wanted) {
				continue;
			}
			log("Checking " + path.filename().string());
			if(md5_file(path) != entry.hash) {
				throw fatal_error(path.filename().string() + " is corrupted (MD5 mismatch); re-download it");
			}
			checked++;
		}
	}
	if(checked < archives.size()) {
		warn(std::to_string(archives.size() - checked) + " selected .bin file(s) are not listed in MD5\\*.md5 and were not checked");
	}
}

void extract_setup(const fs::path & setup, const fs::path & out) {
	log("Unpacking installer files from " + setup.filename().string());
	::extract_options o;
	o.quiet = o.silent = true;
	o.extract = true;
	o.extract_temp = true;
	o.extract_unknown = true;
	o.preserve_file_times = true;
	o.gog_galaxy = false;
	o.codepage = 0;
	o.collisions = OverwriteCollisions;
	o.filenames.set_expand(true);
	o.filenames.set_lowercase(false);
	o.output_dir = out;
	try {
		process_file(setup, o);
	} catch(const setup::version_error &) {
		throw fatal_error(setup.filename().string() + " is not a supported Inno Setup installer (" + innosetup_versions + ")");
	} catch(const std::ios_base::failure & e) {
		throw fatal_error("cannot unpack " + setup.filename().string() + ": stream error: " + e.what());
	} catch(const std::runtime_error & e) {
		throw fatal_error("cannot unpack " + setup.filename().string() + ": " + e.what());
	}
	if(logger::total_errors) {
		throw fatal_error("innoextract reported errors while unpacking " + setup.filename().string());
	}
}

struct prepared_tools {
	fs::path dir;
	fs::path arc_ini;
};

// unarc.dll and FitGirl's tools need some adjustments because the installer script never runs.
prepared_tools prepare_tools(const fs::path & tools, const std::vector<std::pair<std::string, std::string>> & replacements) {
	if(!fs::is_directory(tools) || find_file(tools, "unarc.dll").empty()) {
		throw fatal_error("setup.exe does not contain unarc.dll; not a FreeArc-based FitGirl installer?");
	}
	std::vector<fs::path> files = list_dir(tools, true);

	// Inno Setup constants ({app}, {tmp}, {src}) in the .ini files, e.g. CLS.ini's temp paths.
	std::vector<std::string> changed;
	for(const fs::path & ini : files) {
		if(extension(ini) != ".ini") {
			continue;
		}
		std::string data = read_file(ini);
		std::string updated = replace_all(data, replacements);
		if(updated != data) {
			write_file(ini, updated);
			changed.push_back(ini.filename().string());
		}
	}
	if(!changed.empty()) {
		std::string names;
		for(const std::string & name : changed) {
			names += (names.empty() ? "" : ", ") + name;
		}
		log("Filled installer placeholders in " + names);
	}

	// A CLS-<name>.dll (loaded by unarc.dll) talks to its cls-<name>_x64/_x86.exe helper through named
	// file mappings. Some pairs disagree on the namespace: the helper creates "Global\Read_MapFile_..."
	// while the DLL opens "Read_MapFile_...". Wine keeps those apart, so the DLL fails and the helper
	// hangs. If the DLL has no "Global\" string, blank the helper's one so both use the same name.
	for(const fs::path & dll : files) {
		std::string name = cls_plugin_name(dll.filename().string());
		if(name.empty() || contains_global_namespace(read_file(dll))) {
			continue;
		}
		for(const char * suffix : { "_x64.exe", "_x86.exe" }) {
			fs::path helper = find_file(tools, "cls-" + name + suffix);
			if(helper.empty()) {
				continue;
			}
			std::string data = read_file(helper);
			std::vector<std::size_t> offsets = blank_global_namespace(data);
			if(!offsets.empty()) {
				write_file(helper, data);
			}
			for(std::size_t offset : offsets) {
				char hex[32];
				std::snprintf(hex, sizeof(hex), "0x%zx", offset);
				log("Patched Global\\ namespace in " + helper.filename().string() + " at " + hex);
			}
		}
	}

	prepared_tools result;
	result.dir = tools;
	// External compressor definitions. unarc.dll would look for them next to a hard-coded
	// c:\unarc.dll; its -cfg option points it at the installer's copy instead.
	result.arc_ini = find_file(tools, "arc.ini");
	if(result.arc_ini.empty()) {
		warn("installer has no arc.ini; external compressors will be unavailable");
	}
	return result;
}

struct archive_result {
	bool has_result = false;
	int code = 0;
	std::vector<std::string> errors;
	long long files = 0;
};

archive_result run_archive(const prepared_tools & tools, const std::string & command, const fs::path & archive,
                           const std::vector<std::string> & options, double timeout_hours, bool verbose) {
	std::vector<std::string> args = { "--worker", win(tools.dir), command, win(archive) };
	args.insert(args.end(), options.begin(), options.end());
	if(!tools.arc_ini.empty()) {
		args.push_back("-cfg" + win(tools.arc_ini));
	}

	archive_result r;
	bool tty = util::isatty(1) != 0;
	bool progress_shown = false;
	long long total_files = -1, orig_mb = -1, comp_mb = -1;
	auto end_progress = [&]() {
		if(progress_shown) {
			std::cout << '\n';
			progress_shown = false;
		}
	};
	auto on_line = [&](const std::string & raw) {
		worker_line line = parse_worker_line(raw);
		switch(line.kind) {
			case worker_line::file: {
				r.files++;
				if(verbose || !tty) {
					std::cout << "    " << line.text << std::endl;
				} else {
					std::string name = line.text.size() > 60 ? "..." + line.text.substr(line.text.size() - 57) : line.text;
					name.resize(60, ' ');
					std::cout << "\r    " << r.files << " files  " << name << std::flush;
					progress_shown = true;
				}
				break;
			}
			case worker_line::error: {
				end_progress();
				r.errors.push_back(clean_error(line.text));
				std::cout << "    ERROR: " << r.errors.back() << std::endl;
				break;
			}
			case worker_line::result: {
				r.has_result = true;
				r.code = line.code;
				break;
			}
			case worker_line::info: {
				// callback values: a count for total_files, megabytes for origsize/compsize
				if(line.text == "total_files") total_files = line.size;
				if(line.text == "origsize") orig_mb = line.size;
				if(line.text == "compsize") comp_mb = line.size;
				if(verbose) {
					end_progress();
					std::cout << "    | " << raw << std::endl;
				}
				break;
			}
			case worker_line::other: {
				if(verbose && !line.text.empty()) {
					end_progress();
					std::cout << "    | " << line.text << std::endl;
				}
				break;
			}
		}
	};

	double ms = timeout_hours * 3600.0 * 1000.0;
	unsigned long timeout_ms = ms >= 4294967294.0 ? 4294967294UL : static_cast<unsigned long>(ms);
	job_result job = run_in_job(self_exe(), args, timeout_ms, on_line);
	end_progress();
	if(job.timed_out) {
		r.errors.push_back("timed out after " + std::to_string(timeout_ms / 1000) + " s");
	}
	if(command == "l" && total_files >= 0) {
		std::cout << "    " << total_files << " files, " << orig_mb << " MB unpacked, " << comp_mb << " MB packed"
		          << std::endl;
	}
	return r;
}

std::size_t copy_tree(const fs::path & from, const fs::path & to) {
	if(!fs::is_directory(from)) {
		return 0;
	}
	std::size_t count = 0;
	for(fs::recursive_directory_iterator it(from), end; it != end; ++it) {
		fs::path target = to / fs::relative(it->path(), from);
		if(fs::is_directory(it->status())) {
			fs::create_directories(target);
		} else if(fs::is_regular_file(it->status())) {
			fs::create_directories(target.parent_path());
			fs::copy_file(it->path(), target, fs::copy_options::overwrite_existing);
			count++;
		}
	}
	return count;
}

std::string display_path(const fs::path & p, const fs::path & base) {
	std::string a = win(p), b = win(base) + "\\";
	return to_lower(a).compare(0, b.size(), to_lower(b)) == 0 ? a.substr(b.size()) : a;
}

// 1 if every listed file matches, 0 on problems, -1 if there is no MD5 list.
int verify_dest(const fs::path & dest) {
	fs::path redist = dest / "_Redist";
	std::vector<fs::path> lists;
	if(fs::is_directory(redist)) {
		for(const fs::path & p : list_dir(redist, true)) {
			if(extension(p) == ".md5") {
				lists.push_back(p);
			}
		}
	}
	std::sort(lists.begin(), lists.end());
	if(lists.empty()) {
		warn("no _Redist\\*.md5 found; cannot verify unpacked files");
		return -1;
	}
	bool good = true;
	for(const fs::path & list : lists) {
		log("Verifying files against " + display_path(list, dest));
		std::size_t ok = 0, bad = 0, missing = 0;
		for(const md5_entry & entry : parse_md5_list(read_file(list))) {
			fs::path path = resolve_listed(list, entry.path);
			if(!fs::is_regular_file(path)) {
				missing++;
				std::cout << "    MISSING  " << display_path(path, dest) << std::endl;
			} else if(md5_file(path) != entry.hash) {
				bad++;
				std::cout << "    BAD      " << display_path(path, dest) << std::endl;
			} else {
				ok++;
			}
		}
		std::cout << "    " << ok << " ok, " << bad << " bad, " << missing << " missing" << std::endl;
		good = good && !bad && !missing;
	}
	return good ? 1 : 0;
}

// Removes the work directory however run() ends.
struct scoped_remove {
	fs::path path;
	~scoped_remove() {
		boost::system::error_code ec;
		fs::remove_all(path, ec);
	}
};

int run(const options & o) {
	fs::path pack = full_path(host_path(o.pack));
	if(!fs::is_directory(pack)) {
		throw fatal_error("pack directory " + pack.string() + " does not exist");
	}
	fs::path setup = find_file(pack, "setup.exe");
	if(setup.empty()) {
		throw fatal_error("no setup.exe in " + pack.string());
	}
	std::vector<std::string> bins;
	for(const fs::path & p : list_dir(pack, true)) {
		if(extension(p) == ".bin") {
			bins.push_back(p.filename().string());
		}
	}
	std::vector<std::string> archives = select_names(bins, o.only, o.exclude);
	if(archives.empty()) {
		throw fatal_error("no .bin archives selected in " + pack.string());
	}
	std::string names;
	for(const std::string & name : archives) {
		names += (names.empty() ? "" : ", ") + name;
	}
	log("Archives: " + names);
	std::string wine = wine_version();
	if(!wine.empty()) {
		log("Running under " + wine);
	}

	if(!o.skip_bin_check && !o.list) {
		check_bins(pack, archives);
	}

	fs::path dest;
	fs::path work;
	if(o.list) {
		work = fs::temp_directory_path() / ("fitgirl-extractor-" + std::to_string(GetCurrentProcessId()));
	} else {
		dest = full_path(host_path(o.dest));
		fs::create_directories(dest);
		// Installer files and the temp files of unarc.dll and the CLS/external tools, on the destination disk.
		work = dest / work_dir_name;
	}
	scoped_remove cleanup{ work };
	fs::remove_all(work);
	fs::path temp = work / "tmp";
	fs::create_directories(temp);

	fs::path installer = work / "setup";
	extract_setup(setup, installer);
	// CLS.ini uses {app} only for temp paths (TempPath, ldmfTempPath); pointing it at the game folder
	// itself breaks magic2l's temp handling, so it gets the temp folder.
	std::vector<std::pair<std::string, std::string>> replacements = {
		{ "{app}", win(temp) }, { "{src}", win(pack) }, { "{tmp}", win(installer / "tmp") },
	};
	prepared_tools tools = prepare_tools(installer / "tmp", replacements);

	std::vector<std::string> failed;
	for(const std::string & name : archives) {
		fs::path archive = pack / name;
		archive_result r;
		if(o.list) {
			log("Listing " + name);
			r = run_archive(tools, "l", archive, {}, o.timeout_hours, o.verbose);
		} else {
			log("Unpacking " + name);
			fs::create_directories(temp);
			DWORD started = GetTickCount();
			r = run_archive(tools, "x", archive, { "-o+", "-dp" + win(dest), "-w" + win(temp) }, o.timeout_hours, o.verbose);
			if(r.has_result && r.code == 0) {
				log(name + ": " + std::to_string(r.files) + " files in " + std::to_string((GetTickCount() - started) / 1000) + " s");
			}
		}
		if(!r.has_result || r.code != 0) {
			failed.push_back(name);
			std::string reason;
			for(const std::string & e : r.errors) {
				reason += (reason.empty() ? ": " : "; ") + e;
			}
			warn(name + " failed (result " + (r.has_result ? std::to_string(r.code) : std::string("none")) + ")" + reason);
		}
	}

	if(o.list) {
		return failed.empty() ? 0 : 1;
	}

	std::size_t copied = copy_tree(installer / "app", dest);
	if(copied) {
		log("Copied " + std::to_string(copied) + " installer files (e.g. _Redist) to " + dest.string());
	}
	int verified = o.skip_verify ? -1 : verify_dest(dest);

	if(!failed.empty()) {
		std::string list;
		for(const std::string & name : failed) {
			list += (list.empty() ? "" : ", ") + name;
		}
		std::cout << "\nFAILED: " << list << " (rerun with -v for details)" << std::endl;
		return 1;
	}
	if(verified == 0) {
		std::cout << "\nUnpacking finished, but verification found problems (see above)." << std::endl;
		return 1;
	}
	std::cout << "\nDone: " << dest.string() << (verified == 1 ? "" : " (not verified)") << std::endl;
	return 0;
}

} // namespace

} // namespace fg

int main(int argc, char * argv[]) {
	if(argc >= 2 && std::strcmp(argv[1], "--worker") == 0) {
		return fg::worker_main(argc, argv);
	}

	color::init(color::disable, color::disable);
	fg::options o;
	try {
		o = fg::parse_args(std::vector<std::string>(argv + 1, argv + argc));
	} catch(const fg::usage_error & e) {
		std::cerr << "error: " << e.what() << "\n\n" << fg::usage_text;
		return 2;
	}
	if(o.help) {
		std::cout << fg::usage_text;
		return 0;
	}
	if(o.show_version) {
		std::cout << "fitgirl-extractor " << fg::version << " (with " << innoextract_name << ' ' << innoextract_version
		          << ")\n";
		return 0;
	}

	try {
		return fg::run(o);
	} catch(const fg::fatal_error & e) {
		std::cout.flush();
		std::cerr << "ERROR: " << e.what() << std::endl;
	} catch(const boost::filesystem::filesystem_error & e) {
		std::cout.flush();
		std::cerr << "ERROR: " << e.what() << std::endl;
	} catch(const std::exception & e) {
		std::cout.flush();
		std::cerr << "ERROR: " << e.what() << std::endl;
	}
	return 2;
}
