// Builds the gesso static library with Zig's C toolchain, with SDL3 from the Zig package manager.
// Apps depend on this package and link the "gesso" artifact, which brings its headers, the stb
// headers, and SDL3 (headers and library) with it.
//   zig build                              native debug build of the library into zig-out/
//   zig build -Dtarget=x86_64-windows-gnu  Windows build from any host
//   zig build -Dvideo                      also the video modules, on a trimmed FFmpeg libavcodec
//   zig build -Dtarget=aarch64-macos -Dmacos_sdk=PATH   macOS build from another system, with an Xcode SDK copied from a Mac
//   zig build test                         run the tests
const std = @import("std");

pub const flags: []const []const u8 = &.{ "-std=c11", "-ffp-contract=off", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-missing-field-initializers" };

// Points a module at a macOS SDK's headers, frameworks and libraries.
fn useSdk(mod: *std.Build.Module, sdk: ?std.Build.LazyPath) void {
    const dir = sdk orelse return;
    mod.addSystemIncludePath(dir.path(mod.owner, "usr/include"));
    mod.addSystemFrameworkPath(dir.path(mod.owner, "System/Library/Frameworks"));
    mod.addLibraryPath(dir.path(mod.owner, "usr/lib"));
}

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const video = b.option(bool, "video", "Build the video modules on FFmpeg's libavcodec (fetched only when set)") orelse false;
    const echo = b.option(bool, "echo", "Give gs_voice WebRTC's echo canceller (its sources are fetched only when set)") orelse false;

    // A macOS SDK (the MacOSX.sdk folder from Xcode) lets a macOS build run on any host.
    const macos_sdk = b.option(std.Build.LazyPath, "macos_sdk", "Path of a macOS SDK, for building for macOS on another system");

    const sdl = (if (macos_sdk) |sdk| b.dependency("sdl", .{
        .target = target,
        .optimize = optimize,
        .preferred_linkage = .static,
        .strip = optimize != .debug,
        .system_include_path = sdk.path(b, "usr/include"),
        .system_framework_path = sdk.path(b, "System/Library/Frameworks"),
        .library_path = sdk.path(b, "usr/lib"),
    }) else b.dependency("sdl", .{
        .target = target,
        .optimize = optimize,
        .preferred_linkage = .static,
        .strip = optimize != .debug,
    })).artifact("SDL3");

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
        .files = &.{ "gs_pix.c", "gs_text.c", "gs_mix.c", "gs_synth.c", "gs_midi.c", "gs_stats.c", "gs_pace.c", "gs_seq.c", "gs_jobs.c", "gs_dsp.c", "gs_sha256.c", "gs_json.c", "gs_webm.c", "gs_stream.c", "gs_opus.c", "gs_http.c", "gs_secret.c", "gs_oauth.c", "gs_hls.c", "gs_ui.c", "gs_draw.c", "gs_image.c", "gs_ts.c", "gs_mp4.c", "gs_abr.c", "gs_eta.c", "gs_ed25519.c", "gs_update.c" },
        .flags = flags,
    });
    mod.addIncludePath(b.path("src"));
    mod.addIncludePath(b.path("vendor/stb"));
    mod.addIncludePath(b.path("vendor/kb"));
    mod.addIncludePath(b.path("vendor/minimp3"));
    mod.addIncludePath(b.path("vendor/rnnoise"));
    // The vendored libraries do arithmetic the undefined-behaviour sanitizer of debug builds stops on
    // (unaligned reads in kb_text_shape, shifts of negative values in stb_image_write and TweetNaCl), harmless on the
    // compilers gesso uses; they are built without it.
    mod.addCSourceFile(.{ .file = b.path("src/gs_kb.c"), .flags = &.{ "-std=c11", "-w", "-fno-sanitize=undefined" } });
    mod.addCSourceFile(.{ .file = b.path("src/gs_stb.c"), .flags = &.{ "-std=c11", "-w", "-fno-sanitize=undefined" } });
    mod.addCSourceFile(.{ .file = b.path("src/gs_mp3.c"), .flags = &.{ "-std=c11", "-w", "-fno-sanitize=undefined" } });
    mod.addCSourceFile(.{ .file = b.path("src/gs_tweetnacl.c"), .flags = &.{ "-std=c11", "-w", "-fno-sanitize=undefined" } });
    useSdk(mod, macos_sdk);
    mod.linkLibrary(sdl);
    mod.linkLibrary(opus);
    mod.addIncludePath(opus_src.path("include"));
    if (target.result.os.tag == .windows) for ([_][]const u8{ "winhttp", "crypt32" }) |l| mod.linkSystemLibrary(l, .{});
    // gs_desk, one implementation per system.
    mod.addCSourceFiles(.{ .root = b.path("src"), .files = &.{"gs_desk.c"}, .flags = flags });
    switch (target.result.os.tag) {
        .windows => mod.addCSourceFiles(.{ .root = b.path("src"), .files = &.{ "gs_desk_windows.c", "gs_notify_windows.c" }, .flags = flags }),
        .macos => {
            mod.addCSourceFiles(.{ .root = b.path("src"), .files = &.{ "gs_desk_macos.c", "gs_desk_unix.c" }, .flags = flags });
            for ([_][]const u8{ "gs_desk_macos.m", "gs_notify_macos.m" }) |f| mod.addCSourceFile(.{ .file = b.path(b.fmt("src/{s}", .{f})), .flags = &.{ "-fobjc-arc", "-Wall", "-Wextra", "-Wno-unused-parameter" } });
            for ([_][]const u8{ "AppKit", "Carbon", "ApplicationServices", "UserNotifications", "Security" }) |f| mod.linkFramework(f, .{});
        },
        .linux => mod.addCSourceFiles(.{ .root = b.path("src"), .files = &.{ "gs_desk_linux.c", "gs_desk_unix.c", "gs_notify_linux.c" }, .flags = flags }),
        else => {},
    }
    if (target.result.isGnuLibC() and target.result.os.versionRange().gnuLibCVersion().?.order(.{ .major = 2, .minor = 29, .patch = 0 }) == .lt)
        mod.addCSourceFile(.{ .file = b.path("src/gs_glibc_compat.c"), .flags = flags });
    const av = if (video) addFfmpeg(b, target, macos_sdk) else null;
    if (av) |a| {
        mod.linkLibrary(a.lib);
        for (a.include) |dir| mod.addIncludePath(dir);
        mod.addCSourceFiles(.{ .root = b.path("src"), .files = &.{ "gs_video.c", "gs_aac.c", "gs_live.c", "gs_movie.c" }, .flags = flags });
    }
    // gs_sound plays AAC through gs_aac, where gesso has it.
    mod.addCSourceFile(.{ .file = b.path("src/gs_sound.c"), .flags = if (av != null) flags ++ &[_][]const u8{"-DGS_HAVE_AAC"} else flags });

    // gs_voice cancels echo through WebRTC's canceller, where gesso has it.
    const aec = if (echo) addEchoCanceller(b, target, macos_sdk) else null;
    if (aec) |a| {
        mod.linkLibrary(a.lib);
        mod.link_libcpp = true;
        const shim = b.createModule(.{ .target = target, .optimize = optimize, .link_libcpp = true, .sanitize_c = .off });
        useSdk(shim, macos_sdk);
        for (a.include) |dir| shim.addIncludePath(dir);
        shim.addCSourceFile(.{ .file = b.path("src/gs_echo.cc"), .flags = a.flags });
        mod.addObject(b.addObject(.{ .name = "gs_echo", .root_module = shim }));
    }
    mod.addCSourceFile(.{ .file = b.path("src/gs_voice.c"), .flags = if (aec != null) flags ++ &[_][]const u8{"-DGS_HAVE_ECHO"} else flags });

    const lib = b.addLibrary(.{ .name = "gesso", .linkage = .static, .root_module = mod });
    lib.installHeadersDirectory(b.path("src"), "", .{ .include_extensions = &.{".h"} });
    lib.installHeadersDirectory(b.path("vendor/stb"), "", .{ .include_extensions = &.{".h"} });
    lib.installLibraryHeaders(sdl);
    b.installArtifact(lib);

    // zig build test: every module's checks (tests/), with the video modules' under -Dvideo.
    const tests = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true });
    tests.addCSourceFiles(.{ .root = b.path("tests"), .files = &.{ "main.c", "abr.c", "eta.c", "ed25519.c", "hls.c", "http.c", "image.c", "json.c", "live.c", "desk.c", "media.c", "movie.c", "notify.c", "oauth.c", "secret.c", "sound.c", "stream.c", "text.c", "ui.c", "update.c", "voice.c" }, .flags = if (av != null) flags ++ &[_][]const u8{"-DGS_TEST_VIDEO"} else flags });
    tests.addIncludePath(b.path("src"));
    if (av) |a| for (a.include) |dir| tests.addIncludePath(dir);
    useSdk(tests, macos_sdk);
    tests.linkLibrary(lib);
    const test_run = b.addRunArtifact(b.addExecutable(.{ .name = "gesso-tests", .root_module = tests }));
    test_run.addDirectoryArg(b.path("tests/streams"));
    b.step("test", "Run gesso's tests").dependOn(&test_run.step);

    // zig build bench-text -- OUT.png: a page of mixed scripts and emoji rendered with the system's fonts.
    const text_bench = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true });
    text_bench.addCSourceFile(.{ .file = b.path("bench/text.c"), .flags = flags });
    text_bench.addIncludePath(b.path("src"));
    text_bench.addIncludePath(b.path("vendor/stb"));
    useSdk(text_bench, macos_sdk);
    text_bench.linkLibrary(lib);
    const text_exe = b.addExecutable(.{ .name = "bench-text", .root_module = text_bench });
    const text_run = b.addRunArtifact(text_exe);
    text_run.step.dependOn(&b.addInstallArtifact(text_exe, .{}).step);
    text_run.addPassthruArgs();
    b.step("bench-text", "Render a test page of scripts and emoji to a PNG").dependOn(&text_run.step);

    // zig build bench-ui -- OUT.png: a page of gs_ui's controls and gs_draw's shapes.
    const ui_bench = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true });
    ui_bench.addCSourceFile(.{ .file = b.path("bench/ui.c"), .flags = flags });
    ui_bench.addIncludePath(b.path("src"));
    ui_bench.addIncludePath(b.path("vendor/stb"));
    useSdk(ui_bench, macos_sdk);
    ui_bench.linkLibrary(lib);
    const ui_exe = b.addExecutable(.{ .name = "bench-ui", .root_module = ui_bench });
    const ui_run = b.addRunArtifact(ui_exe);
    ui_run.step.dependOn(&b.addInstallArtifact(ui_exe, .{}).step);
    ui_run.addPassthruArgs();
    b.step("bench-ui", "Render a page of the interface's controls and shapes to a PNG").dependOn(&ui_run.step);

    // zig build bench-weight -- FONT.ttf WEIGHT STATIC.ttf: a variable font at a weight against a static instance of it.
    const weight_bench = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true });
    weight_bench.addCSourceFile(.{ .file = b.path("bench/weight.c"), .flags = flags });
    weight_bench.addIncludePath(b.path("src"));
    weight_bench.addIncludePath(b.path("vendor/stb"));
    useSdk(weight_bench, macos_sdk);
    weight_bench.linkLibrary(lib);
    const weight_exe = b.addExecutable(.{ .name = "bench-weight", .root_module = weight_bench });
    const weight_run = b.addRunArtifact(weight_exe);
    weight_run.step.dependOn(&b.addInstallArtifact(weight_exe, .{}).step);
    weight_run.addPassthruArgs();
    b.step("bench-weight", "Compare a variable font at a weight with a static instance of it").dependOn(&weight_run.step);

    // zig build bench-overlay -- [SECONDS]: gs_desk's overlay window, shown on the desktop.
    const overlay_bench = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true });
    overlay_bench.addCSourceFile(.{ .file = b.path("bench/overlay.c"), .flags = flags });
    overlay_bench.addIncludePath(b.path("src"));
    useSdk(overlay_bench, macos_sdk);
    overlay_bench.linkLibrary(lib);
    const overlay_exe = b.addExecutable(.{ .name = "bench-overlay", .root_module = overlay_bench });
    const overlay_run = b.addRunArtifact(overlay_exe);
    const overlay_install = b.addInstallArtifact(overlay_exe, .{});
    overlay_run.step.dependOn(&overlay_install.step);
    overlay_run.addPassthruArgs();
    b.step("bench-overlay", "Show the overlay window on the desktop for a few seconds").dependOn(&overlay_run.step);

    // zig build bench-notify -- [TITLE] [BODY] [SECONDS]: a desktop notification, waiting for its click.
    const notify_bench = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true });
    notify_bench.addCSourceFile(.{ .file = b.path("bench/notify.c"), .flags = flags });
    notify_bench.addIncludePath(b.path("src"));
    useSdk(notify_bench, macos_sdk);
    notify_bench.linkLibrary(lib);
    const notify_exe = b.addExecutable(.{ .name = "bench-notify", .root_module = notify_bench });
    const notify_install = b.addInstallArtifact(notify_exe, .{});
    const notify_run = b.addRunArtifact(notify_exe);
    notify_run.step.dependOn(&notify_install.step);
    notify_run.addPassthruArgs();
    b.step("bench-notify", "Show a desktop notification and wait for its click").dependOn(&notify_run.step);
    const benches = b.step("benches", "Build the benches without running them");
    benches.dependOn(&notify_install.step);
    benches.dependOn(&overlay_install.step);

    // zig build bench-sound -- FILE: a sound file opened and played through by gs_sound, without a device.
    const sound_bench = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true });
    sound_bench.addCSourceFile(.{ .file = b.path("bench/sound.c"), .flags = flags });
    sound_bench.addIncludePath(b.path("src"));
    useSdk(sound_bench, macos_sdk);
    sound_bench.linkLibrary(lib);
    const sound_exe = b.addExecutable(.{ .name = "bench-sound", .root_module = sound_bench });
    const sound_install = b.addInstallArtifact(sound_exe, .{});
    const sound_run = b.addRunArtifact(sound_exe);
    sound_run.step.dependOn(&sound_install.step);
    sound_run.addPassthruArgs();
    b.step("bench-sound", "Open a sound file and play it through without a device").dependOn(&sound_run.step);
    benches.dependOn(&sound_install.step);

    // zig build bench-voice -- IN.wav OUT.wav [STAGES]: gs_voice over a WAV file.
    const voice_bench = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true });
    voice_bench.addCSourceFile(.{ .file = b.path("bench/voice.c"), .flags = flags });
    voice_bench.addIncludePath(b.path("src"));
    useSdk(voice_bench, macos_sdk);
    voice_bench.linkLibrary(lib);
    const voice_exe = b.addExecutable(.{ .name = "bench-voice", .root_module = voice_bench });
    const voice_install = b.addInstallArtifact(voice_exe, .{});
    const voice_run = b.addRunArtifact(voice_exe);
    voice_run.step.dependOn(&voice_install.step);
    voice_run.addPassthruArgs();
    b.step("bench-voice", "Run gs_voice over a WAV file").dependOn(&voice_run.step);
    benches.dependOn(&voice_install.step);

    // zig build bench-video -Dvideo -- FILE.h264: decoding paths compared on this machine (bench/video.c).
    if (av) |a| {
        const bench = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true });
        bench.addCSourceFile(.{ .file = b.path("bench/video.c"), .flags = flags });
        bench.addIncludePath(b.path("src"));
        for (a.include) |dir| bench.addIncludePath(dir);
        useSdk(bench, macos_sdk);
        bench.linkLibrary(lib);
        const exe = b.addExecutable(.{ .name = "bench-video", .root_module = bench });
        const install = b.addInstallArtifact(exe, .{});
        const run = b.addRunArtifact(exe);
        run.step.dependOn(&install.step);
        run.addPassthruArgs();
        b.step("bench-video", "Play an H.264 file through gs_video and report CPU and memory").dependOn(&run.step);

        // zig build bench-movie -Dvideo -- FILE [software]: a movie played through gs_movie's course in a window (bench/movie.c).
        const movie_bench = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true });
        movie_bench.addCSourceFile(.{ .file = b.path("bench/movie.c"), .flags = flags });
        movie_bench.addIncludePath(b.path("src"));
        for (a.include) |dir| movie_bench.addIncludePath(dir);
        useSdk(movie_bench, macos_sdk);
        movie_bench.linkLibrary(lib);
        const movie_exe = b.addExecutable(.{ .name = "bench-movie", .root_module = movie_bench });
        const movie_install = b.addInstallArtifact(movie_exe, .{});
        const movie_run = b.addRunArtifact(movie_exe);
        movie_run.step.dependOn(&movie_install.step);
        movie_run.addPassthruArgs();
        b.step("bench-movie", "Play a movie file through gs_movie and report how it is decoded").dependOn(&movie_run.step);
        benches.dependOn(&movie_install.step);
    }
}

// FFmpeg's libavcodec and libavutil, trimmed to the H.264 and AAC decoders and each system's hardware
// decoding, built from the release tarball with the files configure generated for each target
// WebRTC's echo canceller (AEC3) and the parts of its audio processing module it stands on, from
// the webrtc-audio-processing release, with Abseil's headers: the files of vendor/webrtc/aec3-sources.txt.
// BSD, linked statically. It is C++, so gesso and what links it take the C++ runtime with it.
const EchoCanceller = struct { lib: *std.Build.Step.Compile, include: []const std.Build.LazyPath, flags: []const []const u8 };

fn addEchoCanceller(b: *std.Build, target: std.Build.ResolvedTarget, sdk: ?std.Build.LazyPath) ?EchoCanceller {
    const t = target.result;
    const webrtc = b.lazyDependency("webrtc_audio_processing", .{});
    const abseil = b.lazyDependency("abseil", .{});
    const src = (webrtc orelse return null).path("webrtc");
    const absl = (abseil orelse return null).path(".");
    // Its checks and logging are compiled out, and it is always optimized and never sanitized.
    var list: std.ArrayList([]const u8) = .empty;
    list.appendSlice(b.allocator, &.{ "-std=c++17", "-w", "-DNDEBUG", "-DWEBRTC_APM_DEBUG_DUMP=0", "-DRTC_DISABLE_LOGGING", "-DRTC_DISABLE_CHECK_MSG", "-DRTC_DISABLE_METRICS" }) catch @panic("OOM");
    list.appendSlice(b.allocator, switch (t.os.tag) {
        .linux => &.{ "-DWEBRTC_POSIX", "-DWEBRTC_LINUX" },
        .macos => &.{ "-DWEBRTC_POSIX", "-DWEBRTC_MAC" },
        .windows => &.{ "-DWEBRTC_WIN", "-DNOMINMAX", "-DWIN32_LEAN_AND_MEAN", "-D_USE_MATH_DEFINES" },
        else => @panic("-Decho: WebRTC's echo canceller is set up for Linux, Windows and macOS only"),
    }) catch @panic("OOM");
    const x86 = t.cpu.arch == .x86_64;
    if (!x86 and t.cpu.arch != .aarch64) @panic("-Decho: WebRTC's echo canceller is set up for x86-64 and aarch64 only");
    if (!x86) list.append(b.allocator, "-DWEBRTC_HAS_NEON") catch @panic("OOM");
    const cxx = list.items;
    const mod = b.createModule(.{ .target = target, .optimize = .fast, .link_libcpp = true, .sanitize_c = .off, .pic = true });
    useSdk(mod, sdk);
    mod.addIncludePath(src);
    mod.addIncludePath(absl);
    // The files written with AVX2 are built for a processor that has it, whatever the build's own, and
    // run only where the processor is found to.
    var avx2_mod: ?*std.Build.Module = null;
    if (x86) {
        var query = target.query;
        query.cpu_features_add.addFeature(@intFromEnum(std.Target.x86.Feature.avx2));
        query.cpu_features_add.addFeature(@intFromEnum(std.Target.x86.Feature.fma));
        avx2_mod = b.createModule(.{ .target = b.resolveTargetQuery(query), .optimize = .fast, .link_libcpp = true, .sanitize_c = .off, .pic = true });
        useSdk(avx2_mod.?, sdk);
        avx2_mod.?.addIncludePath(src);
        avx2_mod.?.addIncludePath(absl);
    }
    var plain: std.ArrayList([]const u8) = .empty;
    var lines = std.mem.tokenizeAny(u8, @embedFile("vendor/webrtc/aec3-sources.txt"), "\r\n");
    while (lines.next()) |file| {
        if (file[0] == '#') continue;
        const avx2 = std.mem.indexOf(u8, file, "avx2") != null;
        const sse = std.mem.indexOf(u8, file, "sse") != null;
        const neon = std.mem.indexOf(u8, file, "neon") != null;
        if ((avx2 or sse) and !x86) continue;
        if (neon and x86) continue;
        if (std.mem.endsWith(u8, file, ".c")) {
            mod.addCSourceFile(.{ .file = src.path(b, file), .flags = cxx[1..] });
        } else if (avx2) {
            avx2_mod.?.addCSourceFile(.{ .file = src.path(b, file), .flags = cxx });
        } else plain.append(b.allocator, file) catch @panic("OOM");
    }
    mod.addCSourceFiles(.{ .root = src, .files = plain.items, .flags = cxx });
    if (avx2_mod) |m| mod.linkLibrary(b.addLibrary(.{ .name = "webrtc-aec3-avx2", .linkage = .static, .root_module = m }));
    const include = b.allocator.dupe(std.Build.LazyPath, &.{ src, absl }) catch @panic("OOM");
    return .{ .lib = b.addLibrary(.{ .name = "webrtc-aec3", .linkage = .static, .root_module = mod }), .include = include, .flags = cxx };
}

// (ffmpeg/<target>/, made by ffmpeg/tools/import.sh). LGPL 2.1 or later, linked statically.
const Ffmpeg = struct { lib: *std.Build.Step.Compile, include: []const std.Build.LazyPath };

fn addFfmpeg(b: *std.Build, target: std.Build.ResolvedTarget, sdk: ?std.Build.LazyPath) ?Ffmpeg {
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
    const mod = b.createModule(.{ .target = target, .optimize = .fast, .link_libc = true, .sanitize_c = .off, .pic = true });
    useSdk(mod, sdk);
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
        if (nasm == null) nasm = (b.lazyDependency("nasm", .{ .target = b.graph.host, .optimize = .fast }) orelse return null).artifact("nasm");
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
