#include "winutil.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <memory>

#include <boost/filesystem/operations.hpp>

#include "core.hpp"
#include "crypto/md5.hpp"
#include "util/fstream.hpp"

namespace fg {

std::wstring widen(const std::string & utf8) {
	if(utf8.empty()) {
		return std::wstring();
	}
	int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
	std::wstring out(std::size_t(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), &out[0], n);
	return out;
}

std::string narrow(const std::wstring & wide) {
	if(wide.empty()) {
		return std::string();
	}
	int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), nullptr, 0, nullptr, nullptr);
	std::string out(std::size_t(n), '\0');
	WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), &out[0], n, nullptr, nullptr);
	return out;
}

std::string utf8_to_ansi(const std::string & utf8) {
	if(GetACP() == CP_UTF8) {
		return utf8; // "Use Unicode UTF-8 for worldwide language support" is enabled
	}
	std::wstring wide = widen(utf8);
	if(wide.empty()) {
		return std::string();
	}
	BOOL lossy = FALSE;
	int n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wide.data(), int(wide.size()), nullptr, 0, nullptr, &lossy);
	if(n <= 0) {
		throw fatal_error("cannot convert '" + utf8 + "' to the ANSI code page: " + error_message(GetLastError()));
	}
	std::string out(std::size_t(n), '\0');
	WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wide.data(), int(wide.size()), &out[0], n, nullptr, &lossy);
	if(lossy) {
		throw fatal_error("'" + utf8 + "' contains characters that unarc.dll cannot handle (not in the ANSI code page "
		                  + std::to_string(GetACP()) + "); use a path without them");
	}
	return out;
}

std::string ansi_to_utf8(const std::string & ansi) {
	if(ansi.empty()) {
		return std::string();
	}
	int n = MultiByteToWideChar(CP_ACP, 0, ansi.data(), int(ansi.size()), nullptr, 0);
	std::wstring wide(std::size_t(n), L'\0');
	MultiByteToWideChar(CP_ACP, 0, ansi.data(), int(ansi.size()), &wide[0], n);
	return narrow(wide);
}

std::string error_message(unsigned long code) {
	wchar_t * buffer = nullptr;
	DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
	                         nullptr, code, 0, reinterpret_cast<wchar_t *>(&buffer), 0, nullptr);
	std::string message = n ? narrow(std::wstring(buffer, n)) : std::string();
	if(buffer) {
		LocalFree(buffer);
	}
	while(!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == ' ' || message.back() == '.')) {
		message.pop_back();
	}
	return message + " (error " + std::to_string(code) + ")";
}

std::string wine_version() {
	typedef const char * (CDECL * wine_get_version_t)(void);
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	auto get_version = ntdll ? reinterpret_cast<wine_get_version_t>(
		reinterpret_cast<void *>(GetProcAddress(ntdll, "wine_get_version"))) : nullptr;
	return get_version ? std::string("Wine ") + get_version() : std::string();
}

fs::path host_path(const std::string & arg) {
	if(arg.empty() || arg[0] != '/' || (arg.size() > 1 && arg[1] == '/')) {
		return fs::path(arg);
	}
	typedef WCHAR * (CDECL * wine_get_dos_file_name_t)(LPCSTR);
	HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
	auto to_dos = kernel32 ? reinterpret_cast<wine_get_dos_file_name_t>(
		reinterpret_cast<void *>(GetProcAddress(kernel32, "wine_get_dos_file_name"))) : nullptr;
	if(!to_dos) {
		return fs::path(arg);
	}
	WCHAR * dos = to_dos(arg.c_str()); // the Unix path is in the Unix locale's encoding, normally UTF-8
	if(!dos) {
		throw fatal_error("cannot map '" + arg + "' to a Windows path (does it exist?)");
	}
	std::wstring result(dos);
	HeapFree(GetProcessHeap(), 0, dos);
	return fs::path(narrow(result));
}

std::string win(const fs::path & p) {
	fs::path copy = fs::absolute(p).lexically_normal();
	copy.make_preferred();
	return copy.string();
}

std::string read_file(const fs::path & p) {
	util::ifstream in(p, std::ios_base::in | std::ios_base::binary);
	if(!in.is_open()) {
		throw fatal_error("cannot read " + p.string());
	}
	std::string data;
	char buffer[1 << 16];
	while(in.read(buffer, sizeof(buffer)) || in.gcount() > 0) {
		data.append(buffer, std::size_t(in.gcount()));
	}
	return data;
}

void write_file(const fs::path & p, const std::string & data) {
	util::ofstream out(p, std::ios_base::out | std::ios_base::binary | std::ios_base::trunc);
	if(!out.is_open() || !out.write(data.data(), std::streamsize(data.size())) || !out.flush()) {
		throw fatal_error("cannot write " + p.string());
	}
}

std::string md5_file(const fs::path & p) {
	util::ifstream in(p, std::ios_base::in | std::ios_base::binary);
	if(!in.is_open()) {
		throw fatal_error("cannot read " + p.string());
	}
	crypto::md5 md5;
	md5.init();
	std::unique_ptr<char[]> buffer(new char[1 << 22]);
	while(in.read(buffer.get(), 1 << 22) || in.gcount() > 0) {
		md5.update(buffer.get(), std::size_t(in.gcount()));
	}
	char digest[16];
	md5.finalize(digest);
	static const char hex[] = "0123456789abcdef";
	std::string out;
	for(char c : digest) {
		out += hex[(static_cast<unsigned char>(c) >> 4) & 0xf];
		out += hex[static_cast<unsigned char>(c) & 0xf];
	}
	return out;
}

fs::path self_exe() {
	std::wstring buffer(MAX_PATH, L'\0');
	for(;;) {
		DWORD n = GetModuleFileNameW(nullptr, &buffer[0], DWORD(buffer.size()));
		if(n == 0) {
			throw fatal_error("GetModuleFileName failed: " + error_message(GetLastError()));
		}
		if(n < buffer.size()) {
			buffer.resize(n);
			return fs::path(narrow(buffer));
		}
		buffer.resize(buffer.size() * 2);
	}
}

namespace {

struct handle {
	HANDLE h = nullptr;
	handle() = default;
	explicit handle(HANDLE value) : h(value == INVALID_HANDLE_VALUE ? nullptr : value) { }
	handle(const handle &) = delete;
	handle & operator=(const handle &) = delete;
	~handle() { reset(); }
	void reset(HANDLE value = nullptr) {
		if(h) {
			CloseHandle(h);
		}
		h = value;
	}
};

// Shared between run_in_job() and its reader thread. Heap-allocated and reference-counted, so a reader
// that cannot be stopped never touches run_in_job()'s stack after it has returned.
struct reader_state {
	HANDLE pipe = nullptr;
	HANDLE ready = nullptr; // auto-reset event: lines were queued or the reader finished
	CRITICAL_SECTION lock;
	std::deque<std::string> lines;
	bool done = false;
	reader_state() { InitializeCriticalSection(&lock); }
	~reader_state() {
		if(pipe) CloseHandle(pipe);
		if(ready) CloseHandle(ready);
		DeleteCriticalSection(&lock);
	}
	void push(std::string line, bool finished) {
		EnterCriticalSection(&lock);
		if(finished) {
			done = true;
		} else {
			lines.push_back(std::move(line));
		}
		LeaveCriticalSection(&lock);
		SetEvent(ready);
	}
	// Returns the queued lines; `finished` tells whether the reader has stopped.
	std::deque<std::string> take(bool & finished) {
		EnterCriticalSection(&lock);
		std::deque<std::string> out;
		out.swap(lines);
		finished = done;
		LeaveCriticalSection(&lock);
		return out;
	}
};

DWORD WINAPI read_lines(LPVOID param) {
	std::unique_ptr<std::shared_ptr<reader_state>> owner(static_cast<std::shared_ptr<reader_state> *>(param));
	reader_state & state = **owner;
	std::string pending;
	char buffer[4096];
	DWORD n;
	while(ReadFile(state.pipe, buffer, sizeof(buffer), &n, nullptr) && n > 0) {
		pending.append(buffer, n);
		std::size_t pos;
		while((pos = pending.find('\n')) != std::string::npos) {
			std::string line = pending.substr(0, pos);
			if(!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			pending.erase(0, pos + 1);
			state.push(std::move(line), false);
		}
	}
	if(!pending.empty()) {
		state.push(pending, false);
	}
	state.push(std::string(), true);
	return 0;
}

} // namespace

job_result run_in_job(const fs::path & exe, const std::vector<std::string> & args, unsigned long timeout_ms,
                      const std::function<void(const std::string &)> & on_line) {
	SECURITY_ATTRIBUTES inherit = { sizeof(inherit), nullptr, TRUE };
	HANDLE read_raw = nullptr, write_raw = nullptr;
	if(!CreatePipe(&read_raw, &write_raw, &inherit, 0)) {
		throw fatal_error("CreatePipe failed: " + error_message(GetLastError()));
	}
	handle read_end(read_raw), write_end(write_raw);
	SetHandleInformation(read_end.h, HANDLE_FLAG_INHERIT, 0);
	// stderr gets its own handle, so that unarc.dll closing one of them doesn't affect the other
	HANDLE err_raw = nullptr;
	DuplicateHandle(GetCurrentProcess(), write_end.h, GetCurrentProcess(), &err_raw, 0, TRUE, DUPLICATE_SAME_ACCESS);
	handle err_end(err_raw);
	handle null_in(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr));

	// Kill everything in the job when the last handle closes, i.e. also if this process dies.
	handle job(CreateJobObjectW(nullptr, nullptr));
	if(job.h) {
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
		std::memset(&limits, 0, sizeof(limits));
		limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
	}

	std::string command_line = quote_arg(win(exe));
	for(const std::string & arg : args) {
		command_line += " " + quote_arg(arg);
	}
	std::wstring wide_command_line = widen(command_line);

	STARTUPINFOW si;
	std::memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = null_in.h;
	si.hStdOutput = write_end.h;
	si.hStdError = err_end.h ? err_end.h : write_end.h;
	PROCESS_INFORMATION pi;
	std::wstring wide_exe = widen(win(exe));
	if(!CreateProcessW(wide_exe.c_str(), &wide_command_line[0], nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr,
	                   nullptr, &si, &pi)) {
		throw fatal_error("cannot start " + exe.string() + ": " + error_message(GetLastError()));
	}
	handle process(pi.hProcess), thread(pi.hThread);
	if(job.h && !AssignProcessToJobObject(job.h, process.h)) {
		job.reset(); // e.g. nested jobs not supported; fall back to killing just the worker
	}
	ResumeThread(thread.h);
	write_end.reset();
	err_end.reset();
	null_in.reset();

	std::shared_ptr<reader_state> state = std::make_shared<reader_state>();
	state->ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	state->pipe = read_end.h;
	read_end.h = nullptr;
	std::shared_ptr<reader_state> * thread_ref = new std::shared_ptr<reader_state>(state);
	handle reader(state->ready ? CreateThread(nullptr, 0, read_lines, thread_ref, 0, nullptr) : nullptr);
	if(!reader.h) {
		delete thread_ref;
		if(job.h) {
			TerminateJobObject(job.h, 1);
		}
		TerminateProcess(process.h, 1);
		throw fatal_error("cannot start the output reader: " + error_message(GetLastError()));
	}

	// Output lines are handled on this thread while waiting; returns true once the reader has finished.
	auto drain = [&]() {
		bool finished;
		for(const std::string & line : state->take(finished)) {
			on_line(line);
		}
		return finished;
	};

	job_result result;
	ULONGLONG deadline = GetTickCount64() + timeout_ms;
	for(;;) {
		ULONGLONG now = GetTickCount64();
		DWORD wait = now >= deadline ? 0 : DWORD(std::min<ULONGLONG>(deadline - now, 0xfffffffeULL));
		HANDLE handles[2] = { process.h, state->ready };
		DWORD r = WaitForMultipleObjects(2, handles, FALSE, wait);
		drain();
		if(r == WAIT_OBJECT_0) {
			break;
		}
		if(r == WAIT_TIMEOUT || r == WAIT_FAILED) {
			result.timed_out = (r == WAIT_TIMEOUT);
			break;
		}
	}

	// Stop the worker (if still running) and every helper it left behind.
	if(job.h) {
		TerminateJobObject(job.h, 1);
	}
	TerminateProcess(process.h, 1);
	WaitForSingleObject(process.h, INFINITE);
	DWORD code = 1;
	GetExitCodeProcess(process.h, &code);
	result.exit_code = code;

	// The worker's output is already in the pipe; let the reader drain it. If something outside the job
	// still holds the pipe open, give up: the reader only uses its own heap-allocated state.
	ULONGLONG drain_deadline = GetTickCount64() + 60000;
	bool cancelled = false;
	while(!drain()) {
		ULONGLONG now = GetTickCount64();
		if(now >= drain_deadline) {
			if(cancelled) {
				break;
			}
			CancelSynchronousIo(reader.h);
			cancelled = true;
			drain_deadline = now + 5000;
			continue;
		}
		WaitForSingleObject(state->ready, DWORD(drain_deadline - now));
	}
	return result;
}

} // namespace fg
