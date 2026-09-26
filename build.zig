// Builds the gesso static library with Zig's C toolchain, with SDL3 from the Zig package manager.
// Apps depend on this package and link the "gesso" artifact, which brings its headers, the stb
// headers, and SDL3 (headers and library) with it.
//   zig build                              native debug build of the library into zig-out/
//   zig build -Dtarget=x86_64-windows-gnu  Windows build from any host
const std = @import("std");

pub const flags: []const []const u8 = &.{ "-std=c11", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-missing-field-initializers" };

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const sdl = b.dependency("sdl", .{
        .target = target,
        .optimize = optimize,
        .preferred_linkage = .static,
        .strip = optimize != .Debug,
    }).artifact("SDL3");

    const mod = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true });
    mod.addCSourceFiles(.{
        .root = b.path("src"),
        .files = &.{ "gs_pix.c", "gs_text.c", "gs_mix.c", "gs_synth.c", "gs_midi.c", "gs_stats.c", "gs_stb.c" },
        .flags = flags,
    });
    mod.addIncludePath(b.path("src"));
    mod.addIncludePath(b.path("vendor/stb"));
    mod.linkLibrary(sdl);

    const lib = b.addLibrary(.{ .name = "gesso", .linkage = .static, .root_module = mod });
    lib.installHeadersDirectory(b.path("src"), "", .{ .include_extensions = &.{".h"} });
    lib.installHeadersDirectory(b.path("vendor/stb"), "", .{ .include_extensions = &.{".h"} });
    lib.installLibraryHeaders(sdl);
    b.installArtifact(lib);
}
