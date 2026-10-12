// Unit tests for the platform-independent helpers (run with `zig build test`).
#include "core.hpp"

#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

#define CHECK(cond) check((cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ(a, b) check_eq((a), (b), #a " == " #b, __FILE__, __LINE__)

void check(bool ok, const char * what, const char * file, int line) {
	checks++;
	if(!ok) {
		failures++;
		std::cerr << file << ":" << line << ": FAILED: " << what << "\n";
	}
}

template <typename A, typename B>
void check_eq(const A & a, const B & b, const char * what, const char * file, int line) {
	checks++;
	if(!(a == b)) {
		failures++;
		std::cerr << file << ":" << line << ": FAILED: " << what << "\n";
	}
}

template <typename E>
bool throws(const std::function<void()> & f) {
	try {
		f();
	} catch(const E &) {
		return true;
	}
	return false;
}

typedef std::vector<std::string> strings;

std::string bytes(const char * data, std::size_t size) {
	return std::string(data, size);
}

std::string make_pe(unsigned characteristics) {
	std::string data(0x100, '\0');
	data[0] = 'M', data[1] = 'Z';
	data[0x3c] = char(0x80);
	std::memcpy(&data[0x80], "PE\0\0", 4);
	data[0x80 + 22] = char(characteristics & 0xff);
	data[0x80 + 23] = char(characteristics >> 8);
	return data;
}

void test_parse_args() {
	fg::options o = fg::parse_args({ "C:\\Pack", "-d", "D:\\Game", "--only", "fg-0*.bin", "--exclude=*optional*",
	                                 "--timeout", "0.5", "-v", "--skip-verify" });
	CHECK_EQ(o.pack, std::string("C:\\Pack"));
	CHECK_EQ(o.dest, std::string("D:\\Game"));
	CHECK_EQ(o.only, strings({ "fg-0*.bin" }));
	CHECK_EQ(o.exclude, strings({ "*optional*" }));
	CHECK_EQ(o.timeout_hours, 0.5);
	CHECK(o.verbose && o.skip_verify && !o.skip_bin_check && !o.list);

	fg::options l = fg::parse_args({ "--list", "/Users/me/Pack" });
	CHECK(l.list && l.dest.empty() && l.pack == "/Users/me/Pack");
	CHECK(fg::parse_args({ "--dest=X", "--", "-odd-name" }).pack == "-odd-name");
	CHECK(fg::parse_args({ "--help" }).help);
	CHECK(fg::parse_args({ "--version" }).show_version);

	CHECK(throws<fg::usage_error>([] { fg::parse_args({ "Pack" }); }));                    // no --dest
	CHECK(throws<fg::usage_error>([] { fg::parse_args({ "-d", "X" }); }));                 // no pack
	CHECK(throws<fg::usage_error>([] { fg::parse_args({ "A", "B", "-d", "X" }); }));       // two packs
	CHECK(throws<fg::usage_error>([] { fg::parse_args({ "A", "--dest" }); }));             // missing value
	CHECK(throws<fg::usage_error>([] { fg::parse_args({ "A", "--list", "--timeout", "0" }); }));
	CHECK(throws<fg::usage_error>([] { fg::parse_args({ "A", "--list", "--timeout", "1h" }); }));
	CHECK(throws<fg::usage_error>([] { fg::parse_args({ "A", "--list", "--bogus" }); }));
}

void test_natural_sort_and_selection() {
	strings names = { "fg-10.bin", "fg-2.bin", "fg-01.bin", "fg-selective-english.bin", "FG-03.BIN" };
	CHECK_EQ(fg::select_names(names, {}, {}),
	         strings({ "fg-01.bin", "fg-2.bin", "FG-03.BIN", "fg-10.bin", "fg-selective-english.bin" }));
	CHECK_EQ(fg::select_names(names, {}, { "*selective*" }), strings({ "fg-01.bin", "fg-2.bin", "FG-03.BIN", "fg-10.bin" }));
	CHECK_EQ(fg::select_names(names, { "fg-0[12].bin", "fg-[!0-9]*" }, {}),
	         strings({ "fg-01.bin", "fg-selective-english.bin" }));
	CHECK(fg::natural_less("fg-9.bin", "fg-10.bin"));
	CHECK(!fg::natural_less("fg-10.bin", "fg-9.bin"));
	CHECK(fg::natural_less("fg-99999999999999999999.bin", "fg-100000000000000000000.bin")); // no overflow
}

void test_glob() {
	CHECK(fg::glob_match("*.bin", "fg-01.BIN"));
	CHECK(fg::glob_match("fg-??.bin", "fg-01.bin"));
	CHECK(!fg::glob_match("fg-?.bin", "fg-01.bin"));
	CHECK(fg::glob_match("*a*b*", "xxaxxbxx"));
	CHECK(!fg::glob_match("*a*b", "xxaxxbxx"));
	CHECK(fg::glob_match("fg-0[1-3].bin", "fg-02.bin"));
	CHECK(!fg::glob_match("fg-0[!1-3].bin", "fg-02.bin"));
	CHECK(fg::glob_match("[x", "[x")); // unterminated class is literal
	CHECK(fg::glob_match("", "") && !fg::glob_match("", "a"));
}

void test_md5_list() {
	std::string list = "; If you miss selective download files - that's OK\r\n"
	                   "\r\n"
	                   "B07516732147145D39FB8237CE184E1C *..\\fg-01.bin\r\n"
	                   "65bc80d3ab470d059e5ac3069bac5c83 *..\\Engine\\Binaries\\a b.dll\r\n"
	                   "not a hash *x\r\n"
	                   "65bc80d3ab470d059e5ac3069bac5c8 *short\r\n"
	                   "0e585ea67a33c7802ebbd1fd576f156c no-star.txt";
	std::vector<fg::md5_entry> e = fg::parse_md5_list(list);
	CHECK_EQ(e.size(), std::size_t(3));
	if(e.size() == 3) {
		CHECK_EQ(e[0].hash, std::string("b07516732147145d39fb8237ce184e1c"));
		CHECK_EQ(e[0].path, std::string("../fg-01.bin"));
		CHECK_EQ(e[1].path, std::string("../Engine/Binaries/a b.dll"));
		CHECK_EQ(e[2].path, std::string("no-star.txt"));
	}
}

void test_placeholders() {
	std::string ini = "[Srep]\r\nTempPath={app}\r\nX={tmp}\\{app}\r\n";
	CHECK_EQ(fg::replace_all(ini, { { "{app}", "Z:\\Games\\X" }, { "{tmp}", "C:\\t" } }),
	         std::string("[Srep]\r\nTempPath=Z:\\Games\\X\r\nX=C:\\t\\Z:\\Games\\X\r\n"));
	CHECK_EQ(fg::replace_all("{app}", { { "{app}", "{app}{app}" } }), std::string("{app}{app}")); // no re-expansion
}

void test_cls_namespace() {
	CHECK_EQ(fg::cls_plugin_name("CLS-srep.dll"), std::string("srep"));
	CHECK_EQ(fg::cls_plugin_name("cls-magic2l.DLL"), std::string("magic2l"));
	CHECK_EQ(fg::cls_plugin_name("cls-srep_x64.exe"), std::string());
	CHECK_EQ(fg::cls_plugin_name("cls-.dll"), std::string());

	CHECK(!fg::contains_global_namespace(bytes("..IO_MapFile_\0Read_MapFile_\0", 28)));
	CHECK(fg::contains_global_namespace(bytes("CLS\0Global\\\0Read", 16)));

	std::string srep = bytes("\0\0\0\0Global\\\0junk\0Global\\\0Read_MapFile_\0", 39);
	std::string expected = srep;
	expected[4] = '\0';
	expected[17] = '\0';
	CHECK_EQ(fg::blank_global_namespace(srep), std::vector<std::size_t>({ 4, 17 }));
	CHECK_EQ(srep, expected);

	std::string embedded = bytes("xGlobal\\\0", 9); // not standalone: left alone
	CHECK(fg::blank_global_namespace(embedded).empty());
	CHECK_EQ(embedded, bytes("xGlobal\\\0", 9));
}

void test_large_address_aware() {
	CHECK(!fg::pe_is_large_address_aware(make_pe(0x0102)));
	CHECK(fg::pe_is_large_address_aware(make_pe(0x0122)));
	CHECK(throws<std::runtime_error>([] { fg::pe_is_large_address_aware(std::string(0x100, '\0')); }));
	CHECK(throws<std::runtime_error>([] { fg::pe_is_large_address_aware("MZ"); }));
}

void test_worker_lines() {
	fg::worker_line f = fg::parse_worker_line("FILE 1234 Rivage\\Content\\a b.pak\r\n");
	CHECK(f.kind == fg::worker_line::file && f.size == 1234 && f.text == "Rivage\\Content\\a b.pak");

	fg::worker_line e = fg::parse_worker_line("ERROR ERROR: unsupported compression method");
	CHECK(e.kind == fg::worker_line::error);
	CHECK_EQ(fg::clean_error(e.text), std::string("unsupported compression method"));

	fg::worker_line i = fg::parse_worker_line("INFO origsize 276 289812688 ");
	CHECK(i.kind == fg::worker_line::info && i.text == "origsize" && i.size == 276 && i.value2 == 289812688);

	fg::worker_line r = fg::parse_worker_line("RESULT -1");
	CHECK(r.kind == fg::worker_line::result && r.code == -1);

	CHECK(fg::parse_worker_line("RESULT x").kind == fg::worker_line::other);
	CHECK(fg::parse_worker_line("FILE x name").kind == fg::worker_line::other);
	CHECK(fg::parse_worker_line("0144:err:virtual:something").kind == fg::worker_line::other);
}

void test_quote_arg() {
	CHECK_EQ(fg::quote_arg("plain"), std::string("plain"));
	CHECK_EQ(fg::quote_arg(""), std::string("\"\""));
	CHECK_EQ(fg::quote_arg("C:\\a b\\"), std::string("\"C:\\a b\\\\\""));
	CHECK_EQ(fg::quote_arg("-dpZ:\\Users\\me\\My Games"), std::string("\"-dpZ:\\Users\\me\\My Games\""));
	CHECK_EQ(fg::quote_arg("say \"hi\""), std::string("\"say \\\"hi\\\"\""));
	CHECK_EQ(fg::quote_arg("a\\\\\"b c"), std::string("\"a\\\\\\\\\\\"b c\""));
	CHECK_EQ(fg::quote_arg("no\\space\\"), std::string("no\\space\\")); // unquoted: backslashes stay literal
}

} // namespace

int main() {
	test_parse_args();
	test_natural_sort_and_selection();
	test_glob();
	test_md5_list();
	test_placeholders();
	test_cls_namespace();
	test_large_address_aware();
	test_worker_lines();
	test_quote_arg();
	std::cout << checks - failures << "/" << checks << " checks passed\n";
	return failures ? 1 : 0;
}
