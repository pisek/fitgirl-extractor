const std = @import("std");

const version_macro = "\"" ++ @import("build.zig.zon").version ++ "\"";

// fitgirl-extractor.exe must be a 32-bit Windows program: it loads FitGirl's 32-bit unarc.dll in-process.
const windows_x86: std.Target.Query = .{ .cpu_arch = .x86, .os_tag = .windows, .abi = .gnu };

pub fn build(b: *std.Build) void {
    const optimize = b.option(std.builtin.OptimizeMode, "optimize", "Optimization mode (default: small)") orelse .small;
    const target = b.resolveTargetQuery(windows_x86);

    const innoextract = b.dependency("innoextract", .{});
    const boost = b.dependency("boost", .{});

    const zlib = buildZlib(b, target, optimize);
    const bzip2 = buildBzip2(b, target, optimize);
    const lzma = buildLzma(b, target, optimize);
    const boost_libs = buildBoost(b, target, optimize, boost, zlib, bzip2);

    const exe_mod = b.createModule(.{ .target = target, .optimize = optimize, .link_libcpp = true });
    exe_mod.addIncludePath(b.path("src"));
    exe_mod.addIncludePath(innoextract.path("src"));
    exe_mod.addSystemIncludePath(boost.path(""));
    exe_mod.addConfigHeader(innoextractConfig(b, innoextract));

    const ie_flags: []const []const u8 = &.{ "-std=c++17", "-Wno-deprecated-declarations", "-Wno-deprecated-builtins" };
    const ie_defines = [_][2][]const u8{
        .{ "_WIN32_WINNT", "0x0601" },
        .{ "NOMINMAX", "1" },
        .{ "WIN32_LEAN_AND_MEAN", "1" },
        .{ "UNICODE", "1" },
        .{ "_UNICODE", "1" },
        .{ "BOOST_ALL_NO_LIB", "1" },
        .{ "BOOST_SYSTEM_DISABLE_THREADS", "1" },
        .{ "BOOST_SYSTEM_USE_UTF8", "1" },
        .{ "LZMA_API_STATIC", "1" },
    };
    for (ie_defines) |d| exe_mod.addCMacro(d[0], d[1]);
    exe_mod.addCMacro("FITGIRL_VERSION", version_macro);

    exe_mod.addCSourceFiles(.{ .root = innoextract.path("src"), .files = &innoextract_sources, .flags = ie_flags });
    exe_mod.addCSourceFiles(.{ .root = b.path("src"), .files = &app_sources, .flags = &.{ "-std=c++17", "-Wall", "-Wextra" } });

    exe_mod.linkLibrary(boost_libs);
    exe_mod.linkLibrary(lzma);
    exe_mod.linkLibrary(zlib);
    exe_mod.linkLibrary(bzip2);
    for ([_][]const u8{ "bcrypt", "advapi32", "shell32", "ole32" }) |lib| exe_mod.linkSystemLibrary(lib, .{});

    const exe = b.addExecutable(.{ .name = "fitgirl-extractor", .root_module = exe_mod });
    exe.subsystem = .console;
    b.installArtifact(exe);

    // The exe statically links these; their licenses go along with binary releases.
    for ([_][3][]const u8{
        .{ "innoextract", "LICENSE", "innoextract.txt" },
        .{ "boost", "LICENSE_1_0.txt", "boost.txt" },
        .{ "xz", "COPYING.0BSD", "liblzma.txt" },
        .{ "zlib", "LICENSE", "zlib.txt" },
        .{ "bzip2", "LICENSE", "bzip2.txt" },
    }) |l| b.getInstallStep().dependOn(&b.addInstallFile(b.dependency(l[0], .{}).path(l[1]), b.fmt("licenses/{s}", .{l[2]})).step);

    // Unit tests for the platform-independent helpers, built and run on the host.
    const test_mod = b.createModule(.{
        .target = b.graph.host,
        .optimize = .debug,
        .link_libcpp = true,
    });
    test_mod.addIncludePath(b.path("src"));
    test_mod.addCMacro("FITGIRL_VERSION", version_macro);
    test_mod.addCSourceFiles(.{ .root = b.path("."), .files = &.{ "src/core.cpp", "tests/test_core.cpp" }, .flags = &.{ "-std=c++17", "-Wall", "-Wextra" } });
    const tests = b.addExecutable(.{ .name = "test-core", .root_module = test_mod });
    const run_tests = b.addRunArtifact(tests);
    b.step("test", "Run unit tests on the host").dependOn(&run_tests.step);
}

const app_sources = [_][]const u8{
    "main.cpp",
    "core.cpp",
    "worker.cpp",
    "winutil.cpp",
    "release.cpp",
};

// innoextract minus its command-line front end (cli/main.cpp, which needs Boost.ProgramOptions),
// debug-only code and unit-test helpers.
const innoextract_sources = [_][]const u8{
    "cli/extract.cpp",
    "cli/gog.cpp",
    "cli/goggalaxy.cpp",
    "crypto/adler32.cpp",
    "crypto/checksum.cpp",
    "crypto/crc32.cpp",
    "crypto/hasher.cpp",
    "crypto/md5.cpp",
    "crypto/sha1.cpp",
    "crypto/sha256.cpp",
    "crypto/arc4.cpp",
    "crypto/pbkdf2.cpp",
    "crypto/xchacha20.cpp",
    "loader/exereader.cpp",
    "loader/offsets.cpp",
    "setup/component.cpp",
    "setup/data.cpp",
    "setup/delete.cpp",
    "setup/directory.cpp",
    "setup/expression.cpp",
    "setup/file.cpp",
    "setup/filename.cpp",
    "setup/header.cpp",
    "setup/icon.cpp",
    "setup/info.cpp",
    "setup/ini.cpp",
    "setup/item.cpp",
    "setup/language.cpp",
    "setup/message.cpp",
    "setup/permission.cpp",
    "setup/registry.cpp",
    "setup/run.cpp",
    "setup/task.cpp",
    "setup/type.cpp",
    "setup/version.cpp",
    "setup/windows.cpp",
    "stream/block.cpp",
    "stream/chunk.cpp",
    "stream/file.cpp",
    "stream/lzma.cpp",
    "stream/slice.cpp",
    "util/console.cpp",
    "util/encoding.cpp",
    "util/load.cpp",
    "util/log.cpp",
    "util/process.cpp",
    "util/time.cpp",
    "util/windows.cpp",
};

fn innoextractConfig(b: *std.Build, innoextract: *std.Build.Dependency) *std.Build.Step.ConfigHeader {
    return b.addConfigHeader(.{ .style = .{ .cmake = innoextract.path("src/configure.hpp.in") }, .include_path = "configure.hpp" }, .{
        .INNOEXTRACT_HAVE_ISATTY = false,
        .INNOEXTRACT_HAVE_IOCTL = false,
        .INNOEXTRACT_HAVE_TIMEGM = false,
        .INNOEXTRACT_HAVE_GMTIME_R = false,
        .INNOEXTRACT_HAVE_UTIMENSAT = false,
        .INNOEXTRACT_HAVE_DYNAMIC_UTIMENSAT = false,
        .INNOEXTRACT_HAVE_AT_FDCWD = false,
        .INNOEXTRACT_HAVE_UTIMES = false,
        .INNOEXTRACT_HAVE_DLSYM = false,
        .INNOEXTRACT_HAVE_BUILTIN_BSWAP16 = true,
        .INNOEXTRACT_HAVE_BUILTIN_BSWAP32 = true,
        .INNOEXTRACT_HAVE_BUILTIN_BSWAP64 = true,
        .INNOEXTRACT_HAVE_BSWAP_16 = false,
        .INNOEXTRACT_HAVE_BSWAP_32 = false,
        .INNOEXTRACT_HAVE_BSWAP_64 = false,
        .INNOEXTRACT_HAVE_ALIGNOF = true,
        .INNOEXTRACT_HAVE_STD_CODECVT_UTF8_UTF16 = true,
        .INNOEXTRACT_HAVE_STD_UNIQUE_PTR = true,
        .INNOEXTRACT_HAVE_DECRYPTION = true,
        .INNOEXTRACT_HAVE_LZMA = true,
        .INNOEXTRACT_HAVE_ICONV = false,
        .INNOEXTRACT_HAVE_WIN32_CONV = true,
        .INNOEXTRACT_HAVE_BUILTIN_CONV = false,
        .INNOEXTRACT_HAVE_POSIX_SPAWNP = false,
        .INNOEXTRACT_HAVE_UNISTD_ENVIRON = false,
        .INNOEXTRACT_HAVE_FORK = false,
        .INNOEXTRACT_HAVE_EXECVP = false,
        .INNOEXTRACT_HAVE_WAITPID = false,
    });
}

fn cLibrary(b: *std.Build, name: []const u8, target: std.Build.ResolvedTarget, optimize: std.builtin.OptimizeMode, cpp: bool) *std.Build.Step.Compile {
    return b.addLibrary(.{
        .name = name,
        .linkage = .static,
        .root_module = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true, .link_libcpp = cpp }),
    });
}

fn buildZlib(b: *std.Build, target: std.Build.ResolvedTarget, optimize: std.builtin.OptimizeMode) *std.Build.Step.Compile {
    const dep = b.dependency("zlib", .{});
    const lib = cLibrary(b, "z", target, optimize, false);
    lib.root_module.addCSourceFiles(.{ .root = dep.path(""), .files = &.{
        "adler32.c", "compress.c", "crc32.c",  "deflate.c",  "gzclose.c", "gzlib.c",   "gzread.c",
        "gzwrite.c", "infback.c",  "inffast.c", "inflate.c", "inftrees.c", "trees.c",  "uncompr.c",
        "zutil.c",
    }, .flags = &.{ "-std=c11", "-DHAVE_UNISTD_H=0" } });
    lib.installHeadersDirectory(dep.path(""), "", .{ .include_extensions = &.{ "zlib.h", "zconf.h" } });
    return lib;
}

fn buildBzip2(b: *std.Build, target: std.Build.ResolvedTarget, optimize: std.builtin.OptimizeMode) *std.Build.Step.Compile {
    const dep = b.dependency("bzip2", .{});
    const lib = cLibrary(b, "bz2", target, optimize, false);
    lib.root_module.addCSourceFiles(.{ .root = dep.path(""), .files = &.{
        "blocksort.c", "huffman.c", "crctable.c", "randtable.c", "compress.c", "decompress.c", "bzlib.c",
    }, .flags = &.{"-std=c11"} });
    lib.installHeadersDirectory(dep.path(""), "", .{ .include_extensions = &.{"bzlib.h"} });
    return lib;
}

// Decoder-only liblzma with the raw LZMA1/LZMA2 filters innoextract needs; no threads.
fn buildLzma(b: *std.Build, target: std.Build.ResolvedTarget, optimize: std.builtin.OptimizeMode) *std.Build.Step.Compile {
    const dep = b.dependency("xz", .{});
    const lib = cLibrary(b, "lzma", target, optimize, false);
    const m = lib.root_module;
    for ([_][]const u8{
        "src/liblzma/api",     "src/liblzma/common", "src/liblzma/check", "src/liblzma/lz",
        "src/liblzma/rangecoder", "src/liblzma/lzma", "src/liblzma/delta", "src/liblzma/simple",
        "src/common",
    }) |dir| m.addIncludePath(dep.path(dir));
    for ([_][2][]const u8{
        .{ "HAVE_STDBOOL_H", "1" },     .{ "HAVE_STDINT_H", "1" },        .{ "HAVE_INTTYPES_H", "1" },
        .{ "HAVE_DECODERS", "1" },      .{ "HAVE_DECODER_LZMA1", "1" },   .{ "HAVE_DECODER_LZMA2", "1" },
        .{ "HAVE_CHECK_CRC32", "1" },   .{ "HAVE_CHECK_CRC64", "1" },     .{ "HAVE_CHECK_SHA256", "1" },
        .{ "TUKLIB_PHYSMEM_WINDOWS", "1" }, .{ "TUKLIB_SYMBOL_PREFIX", "lzma_" }, .{ "LZMA_API_STATIC", "1" },
        .{ "HAVE_VISIBILITY", "0" },    .{ "TUKLIB_FAST_UNALIGNED_ACCESS", "1" },
    }) |d| m.addCMacro(d[0], d[1]);
    m.addCSourceFiles(.{ .root = dep.path("src"), .files = &.{
        "common/tuklib_physmem.c",
        "liblzma/check/check.c",
        "liblzma/check/crc32_fast.c",
        "liblzma/check/crc64_fast.c",
        "liblzma/check/sha256.c",
        "liblzma/common/block_util.c",
        "liblzma/common/common.c",
        "liblzma/common/easy_preset.c",
        "liblzma/common/filter_common.c",
        "liblzma/common/hardware_physmem.c",
        "liblzma/common/index.c",
        "liblzma/common/stream_flags_common.c",
        "liblzma/common/string_conversion.c",
        "liblzma/common/vli_size.c",
        "liblzma/common/alone_decoder.c",
        "liblzma/common/auto_decoder.c",
        "liblzma/common/block_buffer_decoder.c",
        "liblzma/common/block_decoder.c",
        "liblzma/common/block_header_decoder.c",
        "liblzma/common/easy_decoder_memusage.c",
        "liblzma/common/file_info.c",
        "liblzma/common/filter_buffer_decoder.c",
        "liblzma/common/filter_decoder.c",
        "liblzma/common/filter_flags_decoder.c",
        "liblzma/common/index_decoder.c",
        "liblzma/common/index_hash.c",
        "liblzma/common/stream_buffer_decoder.c",
        "liblzma/common/stream_decoder.c",
        "liblzma/common/stream_flags_decoder.c",
        "liblzma/common/vli_decoder.c",
        "liblzma/lzma/lzma_encoder_presets.c",
        "liblzma/lzma/lzma_decoder.c",
        "liblzma/lzma/lzma2_decoder.c",
        "liblzma/lz/lz_decoder.c",
    }, .flags = &.{"-std=c11"} });
    lib.installHeadersDirectory(dep.path("src/liblzma/api"), "", .{});
    return lib;
}

fn buildBoost(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    boost: *std.Build.Dependency,
    zlib: *std.Build.Step.Compile,
    bzip2: *std.Build.Step.Compile,
) *std.Build.Step.Compile {
    const lib = cLibrary(b, "boost", target, optimize, true);
    const m = lib.root_module;
    m.addSystemIncludePath(boost.path(""));
    m.addIncludePath(boost.path("libs/filesystem/src"));
    m.linkLibrary(zlib);
    m.linkLibrary(bzip2);
    for ([_][2][]const u8{
        .{ "BOOST_ALL_NO_LIB", "1" },             .{ "BOOST_FILESYSTEM_SOURCE", "1" },
        .{ "BOOST_FILESYSTEM_STATIC_LINK", "1" }, .{ "BOOST_IOSTREAMS_SOURCE", "1" },
        .{ "BOOST_IOSTREAMS_STATIC_LINK", "1" },  .{ "BOOST_FILESYSTEM_HAS_BCRYPT", "1" },
        .{ "BOOST_FILESYSTEM_SINGLE_THREADED", "1" }, .{ "BOOST_SYSTEM_DISABLE_THREADS", "1" },
        .{ "BOOST_SYSTEM_USE_UTF8", "1" },        .{ "BOOST_USE_WINDOWS_H", "1" },
        .{ "_WIN32_WINNT", "0x0601" },            .{ "NOMINMAX", "1" },
    }) |d| m.addCMacro(d[0], d[1]);
    const flags: []const []const u8 = &.{ "-std=c++17", "-Wno-deprecated-declarations" };
    m.addCSourceFiles(.{ .root = boost.path("libs/filesystem/src"), .files = &.{
        "codecvt_error_category.cpp", "directory.cpp",   "exception.cpp",          "operations.cpp",
        "path.cpp",                   "path_traits.cpp", "portability.cpp",        "unique_path.cpp",
        "utf8_codecvt_facet.cpp",     "windows_file_codecvt.cpp",
    }, .flags = flags });
    m.addCSourceFiles(.{ .root = boost.path("libs/iostreams/src"), .files = &.{ "zlib.cpp", "bzip2.cpp", "file_descriptor.cpp" }, .flags = flags });
    return lib;
}
