// Worker mode: loads FitGirl's unarc.dll and calls FreeArcExtract() for one archive.
//
// Runs in a separate process (started by the orchestrator inside a job object) because unarc.dll
// and its CLS helpers can hang or deadlock at exit, and because a timeout must be able to kill
// everything. Output is a line protocol on stdout (see core.hpp: worker_line):
//   FILE <size> <name> / ERROR <message> / INFO <what> <a> <b> <str> / RESULT <code>
#include "worker.hpp"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "winutil.hpp"

namespace fg {

namespace {

typedef int(__stdcall * callback_t)(char * what, int int1, int int2, char * str);
typedef int(__cdecl * FreeArcExtract_t)(callback_t callback, ...);

const int max_unarc_args = 12;

// Private duplicate of the stdout handle. unarc.dll closes the original standard handle while
// cleaning up after external tools; Wine then reuses the handle value for the next file it
// creates, so output written to the standard handle would end up inside extracted game files.
HANDLE out = INVALID_HANDLE_VALUE;

void emit(const std::string & line) {
	std::string data = line + "\n";
	DWORD written;
	WriteFile(out, data.data(), DWORD(data.size()), &written, nullptr);
}

std::string format(const char * fmt, ...) {
	char buffer[2048];
	va_list ap;
	va_start(ap, fmt);
	int n = std::vsnprintf(buffer, sizeof(buffer), fmt, ap);
	va_end(ap);
	return std::string(buffer, n < 0 ? 0 : (n >= int(sizeof(buffer)) ? sizeof(buffer) - 1 : std::size_t(n)));
}

std::string text(const char * ansi) {
	if(!ansi) {
		return std::string();
	}
	std::string s = ansi_to_utf8(ansi);
	for(char & c : s) {
		if(c == '\n' || c == '\r') {
			c = ' ';
		}
	}
	return s;
}

int __stdcall callback(char * what, int int1, int int2, char * str) {
	if(std::strcmp(what, "read") == 0 || std::strcmp(what, "write") == 0) {
		return 0; // very frequent and not informative
	}
	if(std::strcmp(what, "filename") == 0) {
		emit(format("FILE %d ", int2) + text(str));
	} else if(std::strcmp(what, "error") == 0) {
		emit("ERROR " + text(str));
	} else {
		emit(format("INFO %s %d %d ", what, int1, int2) + text(str));
	}
	return 0;
}

// unarc.dll's largest-free-block probe computes (lo+hi)/2 in 32 bits and loops forever once a
// block of 2 GB or more is reservable, i.e. in a large-address-aware process.
bool self_is_large_address_aware() {
	const unsigned char * base = reinterpret_cast<const unsigned char *>(GetModuleHandleW(nullptr));
	const IMAGE_DOS_HEADER * dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
	const IMAGE_NT_HEADERS * nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
	return (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
}

[[noreturn]] void finish(int code) {
	// Skip DLL detach: unarc.dll and the CLS plugins can deadlock on their critical sections at exit.
	TerminateProcess(GetCurrentProcess(), UINT(code));
	ExitProcess(UINT(code));
}

} // namespace

int worker_main(int argc, char * argv[]) {
	// argv: <exe> --worker <tooldir> <x|e|t|l> <archive> [options...]
	if(!DuplicateHandle(GetCurrentProcess(), GetStdHandle(STD_OUTPUT_HANDLE), GetCurrentProcess(), &out, 0, FALSE,
	                    DUPLICATE_SAME_ACCESS)) {
		return 2;
	}
	// Helpers started by unarc.dll must not keep the orchestrator's pipe open.
	SetHandleInformation(GetStdHandle(STD_OUTPUT_HANDLE), HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation(GetStdHandle(STD_ERROR_HANDLE), HANDLE_FLAG_INHERIT, 0);

	const int fixed = 5;
	if(argc < fixed || argc - fixed > max_unarc_args - 3) {
		emit("ERROR usage: --worker <tooldir> <x|e|t|l> <archive> [options...]");
		finish(2);
	}
	if(self_is_large_address_aware()) {
		emit("ERROR fitgirl-extractor.exe is large-address-aware; unarc.dll hangs in that mode");
		finish(2);
	}

	std::vector<std::string> ansi;
	try {
		for(int i = 2; i < argc; i++) {
			ansi.push_back(utf8_to_ansi(argv[i]));
		}
	} catch(const std::exception & e) {
		emit(std::string("ERROR ") + e.what());
		finish(2);
	}
	const std::string & tooldir = ansi[0];

	// unarc.dll looks up the CLS plugins and external tools next to itself / in the current directory.
	if(!SetCurrentDirectoryA(tooldir.c_str()) || !SetDllDirectoryA(tooldir.c_str())) {
		emit("ERROR cannot use tool directory " + std::string(argv[2]) + ": " + error_message(GetLastError()));
		finish(2);
	}
	HMODULE dll = LoadLibraryA("unarc.dll");
	if(!dll) {
		emit("ERROR cannot load unarc.dll: " + error_message(GetLastError()));
		finish(2);
	}
	FreeArcExtract_t extract = reinterpret_cast<FreeArcExtract_t>(
		reinterpret_cast<void *>(GetProcAddress(dll, "FreeArcExtract")));
	if(!extract) {
		emit("ERROR unarc.dll does not export FreeArcExtract");
		finish(2);
	}

	// command, options..., "--", archive, then NULL padding (FreeArcExtract stops at the first NULL)
	char * a[max_unarc_args] = { nullptr };
	int n = 0;
	a[n++] = &ansi[1][0];
	for(std::size_t i = 3; i < ansi.size(); i++) {
		a[n++] = &ansi[i][0];
	}
	char dashdash[] = "--";
	a[n++] = dashdash;
	a[n++] = &ansi[2][0];

	int result = extract(callback, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11],
	                     static_cast<char *>(nullptr));
	emit(format("RESULT %d", result));
	finish(result == 0 ? 0 : 1);
}

} // namespace fg
