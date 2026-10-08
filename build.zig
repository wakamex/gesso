// Builds the gesso static library with Zig's C toolchain, with SDL3 from the Zig package manager.
// Apps depend on this package and link the "gesso" artifact, which brings its headers, the stb
// headers, and SDL3 (headers and library) with it.
//   zig build                              native debug build of the library into zig-out/
//   zig build -Dtarget=x86_64-windows-gnu  Windows build from any host
//   zig build -Dvideo                      also the video modules, on a trimmed FFmpeg libavcodec
const std = @import("std");

pub const flags: []const []const u8 = &.{ "-std=c11", "-ffp-contract=off", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-missing-field-initializers" };

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const video = b.option(bool, "video", "Build the video modules on FFmpeg's libavcodec (fetched only when set)") orelse false;

    const sdl = b.dependency("sdl", .{
        .target = target,
        .optimize = optimize,
        .preferred_linkage = .static,
        .strip = optimize != .Debug,
    }).artifact("SDL3");

    // libopus (BSD), for Opus-compressed SoundFont samples. Only its decoder is used; the linker drops the rest.
    const opus_src = b.dependency("opus", .{});
    const opus_mod = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true });
    opus_mod.addCSourceFiles(.{
        .root = opus_src.path(""),
        .files = &.{
        "celt/bands.c",
        "celt/celt.c",
        "celt/celt_encoder.c",
        "celt/celt_decoder.c",
        "celt/cwrs.c",
        "celt/entcode.c",
        "celt/entdec.c",
        "celt/entenc.c",
        "celt/kiss_fft.c",
        "celt/laplace.c",
        "celt/mathops.c",
        "celt/mdct.c",
        "celt/modes.c",
        "celt/pitch.c",
        "celt/celt_lpc.c",
        "celt/quant_bands.c",
        "celt/rate.c",
        "celt/vq.c",
        "silk/CNG.c",
        "silk/code_signs.c",
        "silk/init_decoder.c",
        "silk/decode_core.c",
        "silk/decode_frame.c",
        "silk/decode_parameters.c",
        "silk/decode_indices.c",
        "silk/decode_pulses.c",
        "silk/decoder_set_fs.c",
        "silk/dec_API.c",
        "silk/enc_API.c",
        "silk/encode_indices.c",
        "silk/encode_pulses.c",
        "silk/gain_quant.c",
        "silk/interpolate.c",
        "silk/LP_variable_cutoff.c",
        "silk/NLSF_decode.c",
        "silk/NSQ.c",
        "silk/NSQ_del_dec.c",
        "silk/PLC.c",
        "silk/shell_coder.c",
        "silk/tables_gain.c",
        "silk/tables_LTP.c",
        "silk/tables_NLSF_CB_NB_MB.c",
        "silk/tables_NLSF_CB_WB.c",
        "silk/tables_other.c",
        "silk/tables_pitch_lag.c",
        "silk/tables_pulses_per_block.c",
        "silk/VAD.c",
        "silk/control_audio_bandwidth.c",
        "silk/quant_LTP_gains.c",
        "silk/VQ_WMat_EC.c",
        "silk/HP_variable_cutoff.c",
        "silk/NLSF_encode.c",
        "silk/NLSF_VQ.c",
        "silk/NLSF_unpack.c",
        "silk/NLSF_del_dec_quant.c",
        "silk/process_NLSFs.c",
        "silk/stereo_LR_to_MS.c",
        "silk/stereo_MS_to_LR.c",
        "silk/check_control_input.c",
        "silk/control_SNR.c",
        "silk/init_encoder.c",
        "silk/control_codec.c",
        "silk/A2NLSF.c",
        "silk/ana_filt_bank_1.c",
        "silk/biquad_alt.c",
        "silk/bwexpander_32.c",
        "silk/bwexpander.c",
        "silk/debug.c",
        "silk/decode_pitch.c",
        "silk/inner_prod_aligned.c",
        "silk/lin2log.c",
        "silk/log2lin.c",
        "silk/LPC_analysis_filter.c",
        "silk/LPC_inv_pred_gain.c",
        "silk/table_LSF_cos.c",
        "silk/NLSF2A.c",
        "silk/NLSF_stabilize.c",
        "silk/NLSF_VQ_weights_laroia.c",
        "silk/pitch_est_tables.c",
        "silk/resampler.c",
        "silk/resampler_down2_3.c",
        "silk/resampler_down2.c",
        "silk/resampler_private_AR2.c",
        "silk/resampler_private_down_FIR.c",
        "silk/resampler_private_IIR_FIR.c",
        "silk/resampler_private_up2_HQ.c",
        "silk/resampler_rom.c",
        "silk/sigm_Q15.c",
        "silk/sort.c",
        "silk/sum_sqr_shift.c",
        "silk/stereo_decode_pred.c",
        "silk/stereo_encode_pred.c",
        "silk/stereo_find_predictor.c",
        "silk/stereo_quant_pred.c",
        "silk/LPC_fit.c",
        "silk/float/apply_sine_window_FLP.c",
        "silk/float/corrMatrix_FLP.c",
        "silk/float/encode_frame_FLP.c",
        "silk/float/find_LPC_FLP.c",
        "silk/float/find_LTP_FLP.c",
        "silk/float/find_pitch_lags_FLP.c",
        "silk/float/find_pred_coefs_FLP.c",
        "silk/float/LPC_analysis_filter_FLP.c",
        "silk/float/LTP_analysis_filter_FLP.c",
        "silk/float/LTP_scale_ctrl_FLP.c",
        "silk/float/noise_shape_analysis_FLP.c",
        "silk/float/process_gains_FLP.c",
        "silk/float/regularize_correlations_FLP.c",
        "silk/float/residual_energy_FLP.c",
        "silk/float/warped_autocorrelation_FLP.c",
        "silk/float/wrappers_FLP.c",
        "silk/float/autocorrelation_FLP.c",
        "silk/float/burg_modified_FLP.c",
        "silk/float/bwexpander_FLP.c",
        "silk/float/energy_FLP.c",
        "silk/float/inner_product_FLP.c",
        "silk/float/k2a_FLP.c",
        "silk/float/LPC_inv_pred_gain_FLP.c",
        "silk/float/pitch_analysis_core_FLP.c",
        "silk/float/scale_copy_vector_FLP.c",
        "silk/float/scale_vector_FLP.c",
        "silk/float/schur_FLP.c",
        "silk/float/sort_FLP.c",
        "src/opus.c",
        "src/opus_decoder.c",
        "src/opus_encoder.c",
        "src/extensions.c",
        "src/opus_multistream.c",
        "src/opus_multistream_encoder.c",
        "src/opus_multistream_decoder.c",
        "src/repacketizer.c",
        "src/opus_projection_encoder.c",
        "src/opus_projection_decoder.c",
        "src/mapping_matrix.c",
        "src/analysis.c",
        "src/mlp.c",
        "src/mlp_data.c",
        },
        .flags = &.{ "-std=gnu99", "-DOPUS_BUILD", "-DVAR_ARRAYS", "-DHAVE_LRINTF", "-DHAVE_LRINT", "-w" },
    });
    for ([_][]const u8{ "include", "celt", "silk", "silk/float", "." }) |dir| opus_mod.addIncludePath(opus_src.path(dir));
    const opus = b.addLibrary(.{ .name = "opus", .linkage = .static, .root_module = opus_mod });

    const mod = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true });
    mod.addCSourceFiles(.{
        .root = b.path("src"),
        .files = &.{ "gs_pix.c", "gs_text.c", "gs_mix.c", "gs_synth.c", "gs_midi.c", "gs_stats.c", "gs_pace.c", "gs_seq.c", "gs_jobs.c", "gs_dsp.c", "gs_sha256.c", "gs_json.c", "gs_webm.c", "gs_stream.c", "gs_opus.c", "gs_stb.c" },
        .flags = flags,
    });
    mod.addIncludePath(b.path("src"));
    mod.addIncludePath(b.path("vendor/stb"));
    mod.linkLibrary(sdl);
    mod.linkLibrary(opus);
    mod.addIncludePath(opus_src.path("include"));
    const av = if (video) addFfmpeg(b, target) else null;
    if (av) |a| {
        mod.linkLibrary(a.lib);
        for (a.include) |dir| mod.addIncludePath(dir);
        mod.addCSourceFiles(.{ .root = b.path("src"), .files = &.{"gs_video.c"}, .flags = flags });
    }

    const lib = b.addLibrary(.{ .name = "gesso", .linkage = .static, .root_module = mod });
    lib.installHeadersDirectory(b.path("src"), "", .{ .include_extensions = &.{".h"} });
    lib.installHeadersDirectory(b.path("vendor/stb"), "", .{ .include_extensions = &.{".h"} });
    lib.installLibraryHeaders(sdl);
    b.installArtifact(lib);

    // zig build bench-video -Dvideo -- FILE.h264: decoding paths compared on this machine (bench/video.c).
    if (av) |a| {
        const bench = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true });
        bench.addCSourceFile(.{ .file = b.path("bench/video.c"), .flags = flags });
        bench.addIncludePath(b.path("src"));
        for (a.include) |dir| bench.addIncludePath(dir);
        bench.linkLibrary(lib);
        const exe = b.addExecutable(.{ .name = "bench-video", .root_module = bench });
        const install = b.addInstallArtifact(exe, .{});
        const run = b.addRunArtifact(exe);
        run.step.dependOn(&install.step);
        if (b.args) |args| run.addArgs(args);
        b.step("bench-video", "Play an H.264 file through gs_video and report CPU and memory").dependOn(&run.step);
    }
}

// FFmpeg's libavcodec and libavutil, trimmed to the H.264 and AAC decoders and each system's hardware
// decoding, built from the release tarball with the files configure generated for each target
// (ffmpeg/<target>/, made by ffmpeg/tools/import.sh). LGPL 2.1 or later, linked statically.
const Ffmpeg = struct { lib: *std.Build.Step.Compile, include: []const std.Build.LazyPath };

fn addFfmpeg(b: *std.Build, target: std.Build.ResolvedTarget) ?Ffmpeg {
    const t = target.result;
    const name, const sources = switch (t.os.tag) {
        .linux => .{ "linux-x86_64", @embedFile("ffmpeg/linux-x86_64/sources.txt") },
        .windows => .{ "windows-x86_64", @embedFile("ffmpeg/windows-x86_64/sources.txt") },
        .macos => .{ "macos-aarch64", @embedFile("ffmpeg/macos-aarch64/sources.txt") },
        else => @panic("-Dvideo: FFmpeg is configured for Linux, Windows and macOS only"),
    };
    if ((t.os.tag == .macos) != (t.cpu.arch == .aarch64) or (t.os.tag != .macos and t.cpu.arch != .x86_64))
        @panic("-Dvideo: FFmpeg is configured for x86-64 Linux and Windows and aarch64 macOS");
    const src = (b.lazyDependency("ffmpeg", .{}) orelse return null).path("");
    const gen = b.path(b.fmt("ffmpeg/{s}", .{name}));

    // FFmpeg relies on dead-code elimination (it does not link at -O0) and on its own bounds
    // reasoning, so it is always optimized and never sanitized, whatever the app's build mode.
    const mod = b.createModule(.{ .target = target, .optimize = .ReleaseFast, .link_libc = true, .sanitize_c = .off, .pic = true });
    mod.addIncludePath(gen);
    mod.addIncludePath(src);
    var av_flags: std.ArrayList([]const u8) = .empty;
    av_flags.appendSlice(b.allocator, &.{ "-std=c17", "-DHAVE_AV_CONFIG_H", "-D_ISOC11_SOURCE", "-D_FILE_OFFSET_BITS=64", "-D_LARGEFILE_SOURCE", "-DPIC", "-fomit-frame-pointer", "-fno-math-errno", "-fno-signed-zeros", "-w" }) catch @panic("OOM");
    switch (t.os.tag) {
        .linux => av_flags.appendSlice(b.allocator, &.{ "-D_POSIX_C_SOURCE=200112", "-D_XOPEN_SOURCE=600" }) catch @panic("OOM"),
        .windows => av_flags.appendSlice(b.allocator, &.{ "-D_POSIX_C_SOURCE=200112", "-D_XOPEN_SOURCE=600", "-DWIN32_LEAN_AND_MEAN", "-U__STRICT_ANSI__" }) catch @panic("OOM"),
        else => av_flags.append(b.allocator, "-fno-common") catch @panic("OOM"),
    }
    if (t.os.tag != .linux) mod.addIncludePath(src.path(b, "compat/stdbit"));
    if (t.os.tag == .macos) mod.addIncludePath(src.path(b, "compat/dispatch_semaphore"));
    if (t.os.tag == .linux) {
        mod.addIncludePath(b.path("vendor"));
        mod.addCSourceFile(.{ .file = b.path("ffmpeg/va_loader.c"), .flags = &.{"-std=c11"} });
    }

    var c_files: std.ArrayList([]const u8) = .empty;
    var nasm: ?*std.Build.Step.Compile = null;
    var lines = std.mem.tokenizeAny(u8, sources, "\r\n");
    while (lines.next()) |file| {
        if (!std.mem.endsWith(u8, file, ".asm")) {
            c_files.append(b.allocator, file) catch @panic("OOM");
            continue;
        }
        // NASM, built from source for the build host, so nothing needs installing.
        if (nasm == null) nasm = (b.lazyDependency("nasm", .{ .target = b.graph.host, .optimize = .ReleaseFast }) orelse return null).artifact("nasm");
        const run = b.addRunArtifact(nasm.?);
        run.addArgs(&.{ "-f", if (t.os.tag == .windows) "win64" else "elf64", "-DPIC" });
        run.addPrefixedDirectoryArg("-I", gen);
        run.addPrefixedDirectoryArg("-I", src);
        run.addPrefixedDirectoryArg("-I", src.path(b, std.fs.path.dirname(file).?));
        run.addArg("-Pconfig.asm");
        mod.addObjectFile(run.addPrefixedOutputFileArg("-o", b.fmt("{s}.o", .{std.fs.path.stem(file)})));
        run.addFileArg(src.path(b, file));
    }
    mod.addCSourceFiles(.{ .root = src, .files = c_files.items, .flags = av_flags.items });
    switch (t.os.tag) {
        .windows => for ([_][]const u8{ "bcrypt", "ole32", "user32" }) |l| mod.linkSystemLibrary(l, .{}),
        .macos => for ([_][]const u8{ "CoreFoundation", "CoreMedia", "CoreVideo", "CoreServices", "VideoToolbox", "QuartzCore" }) |f| mod.linkFramework(f, .{}),
        else => {},
    }
    // Apps reach libavcodec's headers through gesso: the release's, its generated avconfig.h, and libva's.
    const include = b.allocator.dupe(std.Build.LazyPath, &.{ gen, src, b.path("vendor") }) catch @panic("OOM");
    return .{ .lib = b.addLibrary(.{ .name = "avcodec", .linkage = .static, .root_module = mod }), .include = include };
}
