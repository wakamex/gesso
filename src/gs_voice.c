// A port to C of the parts of WebRTC's audio processing module that Chromium applies to a
// microphone, at 16 kHz: its high-pass filter, its noise suppressor, and its second gain controller
// with the voice detector that steers it. Ported from modules/audio_processing (high_pass_filter.cc,
// ns/, agc2/) and common_audio/resampler of the WebRTC project, which is Copyright (c) The WebRTC
// project authors and used under its BSD-style licence (vendor/webrtc/LICENSE); the voice detector's
// weights are RNNoise's (vendor/rnnoise). The constants, the order of operations and the
// approximations are kept as they are there, so that the output matches the original's: on speech
// recordings the two differ by about one part in 32,768.
#include "gs_voice.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "rnn_vad_weights.h"

// ---- The noise suppressor (ns/) ----
//
// Each 10 ms frame is analysed and filtered in the frequency domain, through overlapping windows
// of 256 samples: the noise's spectrum is estimated from the quietest moments of each bin and from
// frames judged to hold no speech, and each bin is then scaled by a Wiener gain that keeps what
// stands above the noise. Samples are in the range of 16-bit integers throughout, which the
// constants assume.

enum { FFT = 256, BINS = FFT / 2 + 1, FRAME = 160, OVERLAP = FFT - FRAME, SIMULT = 3, HISTOGRAM = 1000 };
enum { SHORT_STARTUP = 50, LONG_STARTUP = 200, FEATURE_WINDOW = 500 };
#define LRT_FEATURE_THR 0.5f
#define BIN_SIZE_LRT 0.1f
#define BIN_SIZE_FLAT 0.05f
#define BIN_SIZE_DIFF 0.1f

typedef struct {
    float over_subtraction, minimum_gain;  // how hard it suppresses, and the most it attenuates a bin
    bool adjust_attenuation;
    int32_t frames;  // frames analysed, less one
    // The noise estimate.
    float white_level, pink_numerator, pink_exp;
    float prev_noise[BINS], conservative_noise[BINS], parametric_noise[BINS], noise[BINS];
    float density[SIMULT * BINS], log_quantile[SIMULT * BINS], quantile[BINS];
    int counter[SIMULT], updates;
    // The signal's features and the model of what speech looks like in them.
    float lrt, spectral_diff, spectral_flatness, avg_log_lrt[BINS];
    float diff_normalization, energy_sum;
    int hist_lrt[HISTOGRAM], hist_flatness[HISTOGRAM], hist_diff[HISTOGRAM], hist_countdown;
    float prior_lrt, flatness_threshold, diff_threshold, lrt_weight, flatness_weight, diff_weight;
    float prior_speech, speech[BINS];
    // The filter.
    float spectrum_prev_process[BINS], initial_estimate[BINS], filter[BINS];
    float prev_analysis_spectrum[BINS];
    float analyze_memory[OVERLAP], process_memory[OVERLAP], synthesis_memory[OVERLAP];
    float cos_table[FFT / 2], sin_table[FFT / 2];
} suppressor;

// The first 96 samples of the window: a sine's rise. The window is flat between its two slopes.
static float window_rise[96];

static float fast_log2(float in) {  // an approximation the original uses, from the float's bits
    union { float f; uint32_t a; } x = { in };
    float out = (float)x.a;
    out *= 1.1920929e-7f;  // 1 / 2^23
    out -= 126.942695f;
    return out;
}
static float log_approx(float x) { return fast_log2(x) * 0.69314718056f; }
static float pow_approx(float x, float p) { return powf(2.f, p * fast_log2(x)); }
static float exp_approx(float x) { return pow_approx(10.f, x * 0.4342944819f); }

// An in-place complex transform of 256 points; `sign` +1 forward, -1 inverse (unscaled).
static void transform(const suppressor *s, float *re, float *im, int sign) {
    for (int i = 1, j = 0; i < FFT; i++) {
        int bit = FFT >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float t = re[i]; re[i] = re[j], re[j] = t, t = im[i], im[i] = im[j], im[j] = t; }
    }
    for (int len = 2; len <= FFT; len <<= 1) {
        int step = FFT / len;
        for (int i = 0; i < FFT; i += len)
            for (int k = 0; k < len / 2; k++) {
                float wr = s->cos_table[k * step], wi = (float)sign * s->sin_table[k * step];
                float xr = re[i + k + len / 2] * wr - im[i + k + len / 2] * wi, xi = re[i + k + len / 2] * wi + im[i + k + len / 2] * wr;
                re[i + k + len / 2] = re[i + k] - xr, im[i + k + len / 2] = im[i + k] - xi;
                re[i + k] += xr, im[i + k] += xi;
            }
    }
}

// The spectrum of a windowed frame: its real and imaginary parts for the bins up to half the rate.
static void forward(const suppressor *s, const float *time, float *real, float *imag) {
    float re[FFT], im[FFT] = { 0 };
    memcpy(re, time, sizeof re);
    transform(s, re, im, 1);
    memcpy(real, re, sizeof(float) * BINS), memcpy(imag, im, sizeof(float) * BINS);
    imag[0] = imag[BINS - 1] = 0;
}

static void inverse(const suppressor *s, const float *real, const float *imag, float *time) {
    float re[FFT], im[FFT];
    for (int i = 0; i < BINS; i++) re[i] = real[i], im[i] = imag[i];
    for (int i = BINS; i < FFT; i++) re[i] = real[FFT - i], im[i] = -imag[FFT - i];
    transform(s, re, im, -1);
    for (int i = 0; i < FFT; i++) time[i] = re[i] / FFT;
}

static void apply_window(float *x) {
    for (int i = 0; i < 96; i++) x[i] *= window_rise[i];
    for (int i = 161, k = 95; i < FFT; i++, k--) x[i] *= window_rise[k];
}

static void magnitude(const float *real, const float *imag, float *spectrum) {
    spectrum[0] = fabsf(real[0]) + 1.f;
    spectrum[BINS - 1] = fabsf(real[BINS - 1]) + 1.f;
    for (int i = 1; i < BINS - 1; i++) spectrum[i] = sqrtf(real[i] * real[i] + imag[i] * imag[i]) + 1.f;
}

static void suppressor_init(suppressor *s) {
    memset(s, 0, sizeof *s);
    s->over_subtraction = 1.1f, s->minimum_gain = 0.125f, s->adjust_attenuation = true;  // the level Chromium asks for: 18 dB
    s->frames = -1;
    for (int i = 0; i < SIMULT * BINS; i++) s->density[i] = 0.3f, s->log_quantile[i] = 8.f;
    for (int i = 0; i < SIMULT; i++) s->counter[i] = (int)floor(LONG_STARTUP * (i + 1.f) * (1.f / SIMULT));
    s->updates = 1;
    s->lrt = LRT_FEATURE_THR, s->spectral_flatness = s->spectral_diff = 0.5f;
    for (int i = 0; i < BINS; i++) s->avg_log_lrt[i] = LRT_FEATURE_THR, s->filter[i] = 1.f, s->prev_analysis_spectrum[i] = 1.f;
    s->hist_countdown = 500;
    s->prior_lrt = LRT_FEATURE_THR, s->flatness_threshold = s->diff_threshold = 0.5f, s->lrt_weight = 1.f;
    s->prior_speech = 0.5f;
    for (int i = 0; i < FFT / 2; i++) s->cos_table[i] = (float)cos(2 * 3.14159265358979323846 * i / FFT), s->sin_table[i] = (float)sin(2 * 3.14159265358979323846 * i / FFT);
    for (int i = 0; i < 96; i++) window_rise[i] = (float)sin(3.14159265358979323846 * i / 192);
}

// The noise's spectrum as the level each bin stays under a quarter of the time, tracked in the log
// domain by three estimators started a third of their period apart.
static void quantile_estimate(suppressor *s, const float *spectrum, float *noise) {
    float log_spectrum[BINS];
    for (int i = 0; i < BINS; i++) log_spectrum[i] = log_approx(spectrum[i]);
    int give = -1;
    for (int e = 0, k = 0; e < SIMULT; e++, k += BINS) {
        const float one_by_counter = 1.f / (s->counter[e] + 1.f);
        for (int i = 0, j = k; i < BINS; i++, j++) {
            const float delta = s->density[j] > 1.f ? 40.f / s->density[j] : 40.f, multiplier = delta * one_by_counter;
            if (log_spectrum[i] > s->log_quantile[j]) s->log_quantile[j] += 0.25f * multiplier;
            else s->log_quantile[j] -= 0.75f * multiplier;
            if (fabs(log_spectrum[i] - s->log_quantile[j]) < 0.01f) s->density[j] = (s->counter[e] * s->density[j] + 1.f / (2.f * 0.01f)) * one_by_counter;
        }
        if (s->counter[e] >= LONG_STARTUP) {
            s->counter[e] = 0;
            if (s->updates >= LONG_STARTUP) give = k;
        }
        s->counter[e]++;
    }
    if (s->updates < LONG_STARTUP) give = BINS * (SIMULT - 1), s->updates++;
    if (give >= 0)
        for (int i = 0; i < BINS; i++) s->quantile[i] = exp_approx(s->log_quantile[give + i]);
    memcpy(noise, s->quantile, sizeof s->quantile);
}

// Before the speech probability is known: the quantile estimate, blended over the first half second
// with a white or pink noise fitted to the frames so far.
static void noise_pre_update(suppressor *s, const float *spectrum, float spectral_sum) {
    quantile_estimate(s, spectrum, s->noise);
    if (s->frames >= SHORT_STARTUP) return;
    const int start = 5;
    float sum_log_i_log_magn = 0, sum_log_i = 0, sum_log_i_square = 0, sum_log_magn = 0;
    for (int i = start; i < BINS; i++) {
        float log_i = (float)(round(log((double)i) * 1e6) / 1e6);  // (the original reads these from a table to six places)
        sum_log_i += log_i, sum_log_i_square += log_i * log_i;
        float log_signal = log_approx(spectrum[i]);
        sum_log_magn += log_signal, sum_log_i_log_magn += log_i * log_signal;
    }
    s->white_level += spectral_sum * (1.f / BINS) * s->over_subtraction;
    float denom = sum_log_i_square * (BINS - start) - sum_log_i * sum_log_i;
    float num = sum_log_i_square * sum_log_magn - sum_log_i * sum_log_i_log_magn;
    float adjustment = num / denom;
    adjustment = fmaxf(adjustment, 0.f);
    s->pink_numerator += adjustment;
    num = sum_log_i * sum_log_magn - (BINS - start) * sum_log_i_log_magn;
    adjustment = num / denom;
    adjustment = fmaxf(fminf(adjustment, 1.f), 0.f);
    s->pink_exp += adjustment;
    const float one_by_frames = 1.f / (s->frames + 1.f);
    float parametric_exp = 0, parametric_num = 0;
    if (s->pink_exp > 0.f) {
        parametric_num = exp_approx(s->pink_numerator * one_by_frames);
        parametric_num *= s->frames + 1.f;
        parametric_exp = s->pink_exp * one_by_frames;
    }
    for (int i = 0; i < BINS; i++) {
        if (s->pink_exp == 0.f) s->parametric_noise[i] = s->white_level;
        else s->parametric_noise[i] = parametric_num / pow_approx((float)(i < start ? start : i), parametric_exp);
    }
    for (int i = 0; i < BINS; i++) {
        s->noise[i] *= s->frames;
        float tmp = s->parametric_noise[i] * (SHORT_STARTUP - s->frames);
        s->noise[i] += tmp * one_by_frames;
        s->noise[i] *= 1.f / SHORT_STARTUP;
    }
}

// Once the speech probability is known: the noise follows the signal where there is no speech.
static void noise_post_update(suppressor *s, const float *spectrum) {
    float gamma = 0.9f;
    for (int i = 0; i < BINS; i++) {
        const float speech = s->speech[i], non_speech = 1.f - speech;
        float update = gamma * s->prev_noise[i] + (1.f - gamma) * (non_speech * spectrum[i] + speech * s->prev_noise[i]);
        float gamma_old = gamma;
        gamma = speech > 0.2f ? 0.99f : 0.9f;
        if (speech < 0.2f) s->conservative_noise[i] += 0.05f * (spectrum[i] - s->conservative_noise[i]);
        if (gamma == gamma_old) s->noise[i] = update;
        else {
            s->noise[i] = gamma * s->prev_noise[i] + (1.f - gamma) * (non_speech * spectrum[i] + speech * s->prev_noise[i]);
            s->noise[i] = fminf(s->noise[i], update);
        }
    }
}

static void first_of_two_peaks(float bin_size, const int *histogram, float *position, int *weight) {
    int peak = 0, second = 0, second_weight = 0;
    float second_position = 0;
    *position = 0, *weight = 0;
    for (int i = 0; i < HISTOGRAM; i++) {
        const float mid = (i + 0.5f) * bin_size;
        if (histogram[i] > peak) {
            second = peak, second_weight = *weight, second_position = *position;
            peak = histogram[i], *weight = histogram[i], *position = mid;
        } else if (histogram[i] > second) {
            second = histogram[i], second_weight = histogram[i], second_position = mid;
        }
    }
    if (fabs(second_position - *position) < 2 * bin_size && second_weight > 0.5f * *weight) {
        *weight += second_weight;
        *position = 0.5f * (*position + second_position);
    }
}

// Every 500 frames the thresholds that tell speech from noise are set again from the histograms of
// the three features over those frames.
static void prior_model_update(suppressor *s) {
    float average = 0, average_compl = 0, average_squared = 0;
    int count = 0;
    for (int i = 0; i < 10; i++) {
        float mid = (i + 0.5f) * BIN_SIZE_LRT;
        average += s->hist_lrt[i] * mid, count += s->hist_lrt[i];
    }
    if (count > 0) average = average / count;
    for (int i = 0; i < HISTOGRAM; i++) {
        float mid = (i + 0.5f) * BIN_SIZE_LRT;
        average_squared += s->hist_lrt[i] * mid * mid, average_compl += s->hist_lrt[i] * mid;
    }
    average_squared *= 1.f / FEATURE_WINDOW, average_compl *= 1.f / FEATURE_WINDOW;
    bool low_fluctuations = average_squared - average * average_compl < 0.05f;
    s->prior_lrt = low_fluctuations ? 1.f : fminf(1.f, fmaxf(0.2f, 1.2f * average));
    float flat_position, diff_position;
    int flat_weight, diff_weight;
    first_of_two_peaks(BIN_SIZE_FLAT, s->hist_flatness, &flat_position, &flat_weight);
    first_of_two_peaks(BIN_SIZE_DIFF, s->hist_diff, &diff_position, &diff_weight);
    const int use_flat = flat_weight < 0.3f * 500 || flat_position < 0.6f ? 0 : 1;
    const int use_diff = diff_weight < 0.3f * 500 || low_fluctuations ? 0 : 1;
    s->diff_threshold = fminf(1.f, fmaxf(0.16f, 1.2f * diff_position));
    float one_by_sum = 1.f / (1.f + use_flat + use_diff);
    s->lrt_weight = one_by_sum;
    if (use_flat) s->flatness_threshold = fminf(.95f, fmaxf(0.1f, 0.9f * flat_position)), s->flatness_weight = one_by_sum;
    else s->flatness_weight = 0;
    s->diff_weight = use_diff ? one_by_sum : 0;
}

// The three features of a frame (its likelihood ratio against the noise, how flat its spectrum is,
// how unlike the noise's shape it is), and from them the probability of speech in each bin.
static void speech_probability_update(suppressor *s, const float *prior_snr, const float *post_snr, const float *spectrum, float spectral_sum, float energy) {
    if (s->frames < LONG_STARTUP) {
        s->diff_normalization *= s->frames;
        s->diff_normalization += energy;
        s->diff_normalization /= (s->frames + 1);
    }
    // Spectral flatness: the geometric mean of the spectrum over its arithmetic mean.
    bool zero = false;
    for (int i = 1; i < BINS && !zero; i++) zero = spectrum[i] == 0.f;
    if (zero) s->spectral_flatness -= 0.3f * s->spectral_flatness;
    else {
        float num = 0;
        for (int i = 1; i < BINS; i++) num += log_approx(spectrum[i]);
        float denom = (spectral_sum - spectrum[0]) * (1.f / BINS);
        num *= 1.f / BINS;
        s->spectral_flatness += 0.3f * (exp_approx(num) / denom - s->spectral_flatness);
    }
    // Spectral difference: what of the signal's variance across bins the noise's shape does not explain.
    float noise_average = 0;
    for (int i = 0; i < BINS; i++) noise_average += s->conservative_noise[i];
    noise_average *= 1.f / BINS;
    float signal_average = spectral_sum * (1.f / BINS), covariance = 0, noise_variance = 0, signal_variance = 0;
    for (int i = 0; i < BINS; i++) {
        float signal_diff = spectrum[i] - signal_average, noise_diff = s->conservative_noise[i] - noise_average;
        covariance += signal_diff * noise_diff, noise_variance += noise_diff * noise_diff, signal_variance += signal_diff * signal_diff;
    }
    covariance *= 1.f / BINS, noise_variance *= 1.f / BINS, signal_variance *= 1.f / BINS;
    float diff = (signal_variance - (covariance * covariance) / (noise_variance + 0.0001f)) / (s->diff_normalization + 0.0001f);
    s->spectral_diff += 0.3f * (diff - s->spectral_diff);
    s->energy_sum += energy;
    if (--s->hist_countdown > 0) {
        if (s->lrt < HISTOGRAM * BIN_SIZE_LRT && s->lrt >= 0.f) s->hist_lrt[(int)((1.f / BIN_SIZE_LRT) * s->lrt)]++;
        if (s->spectral_flatness < HISTOGRAM * BIN_SIZE_FLAT && s->spectral_flatness >= 0.f) s->hist_flatness[(int)(s->spectral_flatness * (1.f / BIN_SIZE_FLAT))]++;
        if (s->spectral_diff < HISTOGRAM * BIN_SIZE_DIFF && s->spectral_diff >= 0.f) s->hist_diff[(int)(s->spectral_diff * (1.f / BIN_SIZE_DIFF))]++;
    } else {
        prior_model_update(s);
        memset(s->hist_lrt, 0, sizeof s->hist_lrt), memset(s->hist_flatness, 0, sizeof s->hist_flatness), memset(s->hist_diff, 0, sizeof s->hist_diff);
        s->hist_countdown = FEATURE_WINDOW;
        s->energy_sum = s->energy_sum / FEATURE_WINDOW;
        s->diff_normalization = 0.5f * (s->energy_sum + s->diff_normalization);
        s->energy_sum = 0;
    }
    // The likelihood ratio, averaged over time in each bin and then across bins.
    float lrt_sum = 0;
    for (int i = 0; i < BINS; i++) {
        float tmp1 = 1.f + 2.f * prior_snr[i], tmp2 = 2.f * prior_snr[i] / (tmp1 + 0.0001f), bessel = (post_snr[i] + 1.f) * tmp2;
        s->avg_log_lrt[i] += .5f * (bessel - log_approx(tmp1) - s->avg_log_lrt[i]);
    }
    for (int i = 0; i < BINS; i++) lrt_sum += s->avg_log_lrt[i];
    s->lrt = lrt_sum * (1.f / BINS);

    float width = s->lrt < s->prior_lrt ? 8.f : 4.f;
    float indicator0 = 0.5f * ((float)tanh(width * (s->lrt - s->prior_lrt)) + 1.f);
    width = s->spectral_flatness > s->flatness_threshold ? 8.f : 4.f;
    float indicator1 = 0.5f * ((float)tanh(1.f * width * (s->flatness_threshold - s->spectral_flatness)) + 1.f);
    width = s->spectral_diff < s->diff_threshold ? 8.f : 4.f;
    float indicator2 = 0.5f * ((float)tanh(width * (s->spectral_diff - s->diff_threshold)) + 1.f);
    float prior = s->lrt_weight * indicator0 + s->flatness_weight * indicator1 + s->diff_weight * indicator2;
    s->prior_speech += 0.1f * (prior - s->prior_speech);
    s->prior_speech = fmaxf(fminf(s->prior_speech, 1.f), 0.01f);
    float gain_prior = (1.f - s->prior_speech) / (s->prior_speech + 0.0001f);
    for (int i = 0; i < BINS; i++) s->speech[i] = 1.f / (1.f + gain_prior * exp_approx(-s->avg_log_lrt[i]));
}

static void suppressor_analyze(suppressor *s, const float *frame) {
    memcpy(s->prev_noise, s->noise, sizeof s->noise);
    float energy = 0;
    for (int i = 0; i < OVERLAP; i++) energy += s->analyze_memory[i] * s->analyze_memory[i];
    for (int i = 0; i < FRAME; i++) energy += frame[i] * frame[i];
    if (!(energy > 0.f)) return;  // (silence teaches it nothing)
    if (++s->frames < 0) s->frames = 0;
    float extended[FFT], real[BINS], imag[BINS], spectrum[BINS], post_snr[BINS], prior_snr[BINS];
    memcpy(extended, s->analyze_memory, sizeof s->analyze_memory), memcpy(extended + OVERLAP, frame, sizeof(float) * FRAME);
    memcpy(s->analyze_memory, extended + FFT - OVERLAP, sizeof s->analyze_memory);
    apply_window(extended);
    forward(s, extended, real, imag);
    magnitude(real, imag, spectrum);
    float signal_energy = 0, spectral_sum = 0;
    for (int i = 0; i < BINS; i++) signal_energy += real[i] * real[i] + imag[i] * imag[i];
    signal_energy /= BINS;
    for (int i = 0; i < BINS; i++) spectral_sum += spectrum[i];
    noise_pre_update(s, spectrum, spectral_sum);
    for (int i = 0; i < BINS; i++) {
        float prev = s->prev_analysis_spectrum[i] / (s->prev_noise[i] + 0.0001f) * s->filter[i];
        post_snr[i] = spectrum[i] > s->noise[i] ? spectrum[i] / (s->noise[i] + 0.0001f) - 1.f : 0.f;
        prior_snr[i] = 0.98f * prev + (1.f - 0.98f) * post_snr[i];
    }
    speech_probability_update(s, prior_snr, post_snr, spectrum, spectral_sum, signal_energy);
    noise_post_update(s, spectrum);
    memcpy(s->prev_analysis_spectrum, spectrum, sizeof spectrum);
}

static void suppressor_process(suppressor *s, float *frame) {
    float extended[FFT], real[BINS], imag[BINS], spectrum[BINS];
    memcpy(extended, s->process_memory, sizeof s->process_memory), memcpy(extended + OVERLAP, frame, sizeof(float) * FRAME);
    memcpy(s->process_memory, extended + FFT - OVERLAP, sizeof s->process_memory);
    apply_window(extended);
    float energy_before = 0;
    for (int i = 0; i < FFT; i++) energy_before += extended[i] * extended[i];
    forward(s, extended, real, imag);
    magnitude(real, imag, spectrum);
    // The Wiener filter: each bin's gain from its signal-to-noise ratio, smoothed from frame to frame.
    for (int i = 0; i < BINS; i++) {
        float prev_tsa = s->spectrum_prev_process[i] / (s->prev_noise[i] + 0.0001f) * s->filter[i];
        float current_tsa = spectrum[i] > s->noise[i] ? spectrum[i] / (s->noise[i] + 0.0001f) - 1.f : 0.f;
        float snr_prior = 0.98f * prev_tsa + (1.f - 0.98f) * current_tsa;
        s->filter[i] = snr_prior / (s->over_subtraction + snr_prior);
        s->filter[i] = fmaxf(fminf(s->filter[i], 1.f), s->minimum_gain);
    }
    if (s->frames < SHORT_STARTUP) {
        for (int i = 0; i < BINS; i++) {
            s->initial_estimate[i] += spectrum[i];
            float initial = s->initial_estimate[i] - s->over_subtraction * s->parametric_noise[i];
            initial /= s->initial_estimate[i] + 0.0001f;
            initial = fmaxf(fminf(initial, 1.f), s->minimum_gain);
            initial *= SHORT_STARTUP - s->frames;
            s->filter[i] *= s->frames;
            s->filter[i] += initial;
            s->filter[i] *= 1.f / SHORT_STARTUP;
        }
    }
    memcpy(s->spectrum_prev_process, spectrum, sizeof spectrum);
    for (int i = 0; i < BINS; i++) real[i] *= s->filter[i], imag[i] *= s->filter[i];
    inverse(s, real, imag, extended);
    float energy_after = 0;
    for (int i = 0; i < FFT; i++) energy_after += extended[i] * extended[i];
    apply_window(extended);
    // An overall gain that gives back some of the level the filter took from speech.
    float adjustment = 1.f;
    if (s->adjust_attenuation && s->frames > LONG_STARTUP) {
        float gain = sqrtf(energy_after / (energy_before + 1.f)), factor1 = 1.f, factor2 = 1.f;
        if (gain > 0.5f) {
            factor1 = 1.f + 1.3f * (gain - 0.5f);
            if (gain * factor1 > 1.f) factor1 = 1.f / gain;
        }
        if (gain < 0.5f) {
            gain = fmaxf(gain, s->minimum_gain);
            factor2 = 1.f - 0.3f * (0.5f - gain);
        }
        adjustment = s->prior_speech * factor1 + (1.f - s->prior_speech) * factor2;
    }
    for (int i = 0; i < FFT; i++) extended[i] *= adjustment;
    for (int i = 0; i < OVERLAP; i++) frame[i] = s->synthesis_memory[i] + extended[i];
    memcpy(frame + OVERLAP, extended + OVERLAP, sizeof(float) * (FRAME - OVERLAP));
    memcpy(s->synthesis_memory, extended + FRAME, sizeof s->synthesis_memory);
    for (int i = 0; i < FRAME; i++) frame[i] = fminf(fmaxf(frame[i], -32768.f), 32767.f);
}

// ---- The voice detector (agc2/rnn_vad/) ----
//
// How likely a frame is to hold speech, from a small recurrent network trained for RNNoise. The
// frame is resampled to 24 kHz and kept in a buffer of 36 ms; the pitch is searched for in what
// linear prediction leaves of it, and the network is given the spectrum's shape in 22 bands as a
// cepstrum, how that changes over three frames, how alike the newest 20 ms and the 20 ms one pitch
// period before are in the lower bands, the pitch period, and how much the spectrum has varied
// over the last eight frames.

enum { VAD_FRAME = 240, VAD_WINDOW = 480, MAX_PITCH = 384, MIN_PITCH = 30, PITCH_BUFFER = MAX_PITCH + VAD_WINDOW, INITIAL_LAGS = MAX_PITCH - 3 * MIN_PITCH };
enum { BANDS = 22, LOWER_BANDS = 6, OPUS_BANDS = 20, CEPSTRA = 8, FEATURES = 42, UNITS = 24, VAD_RESET_FRAMES = 150 };
enum { KERNEL = 32, KERNEL_OFFSETS = 32 };

typedef struct { float re, im; } cplx;
typedef struct { int period; float strength; } pitch_info;

typedef struct {
    // The resampler from 16 kHz: a windowed sinc of 32 taps, at 32 offsets between samples.
    float kernel[KERNEL * (KERNEL_OFFSETS + 1)], input[FRAME + KERNEL];
    double virtual_index;
    int r0, r3, r4, block;
    bool primed, first_pass;
    float pitch_buffer[PITCH_BUFFER];
    pitch_info last_pitch;
    // The spectral features.
    cplx twiddle[VAD_WINDOW];
    float half_window[VAD_WINDOW / 2], dct[BANDS * BANDS];
    float cepstra[CEPSTRA][BANDS], distances[(CEPSTRA - 1) * (CEPSTRA - 1)];
    int cepstra_tail;
    float state[UNITS];  // the network's memory
    int frames_to_reset;
} detector;

static void resampler_regions(detector *d, bool second_load) {
    d->r0 = second_load ? KERNEL : KERNEL / 2;
    d->r3 = d->r0 + FRAME - KERNEL, d->r4 = d->r0 + FRAME - KERNEL / 2;
    d->block = d->r4 - KERNEL / 2;
}

static void detector_init(detector *d) {
    memset(d, 0, sizeof *d);
    const double pi = 3.14159265358979323846, ratio = (double)FRAME / VAD_FRAME, scale = (ratio > 1.0 ? 1.0 / ratio : 1.0) * 0.9;
    for (int offset = 0; offset <= KERNEL_OFFSETS; offset++) {
        const float subsample = (float)offset / KERNEL_OFFSETS;
        for (int i = 0; i < KERNEL; i++) {
            const float pre_sinc = (float)(pi * (i - KERNEL / 2 - subsample)), x = (i - subsample) / KERNEL;
            const float window = (float)(0.42 - 0.5 * cos(2.0 * pi * x) + 0.08 * cos(4.0 * pi * x));  // (Blackman)
            d->kernel[i + offset * KERNEL] = (float)(window * (pre_sinc == 0 ? scale : sin(scale * pre_sinc) / pre_sinc));
        }
    }
    resampler_regions(d, false);
    d->first_pass = true;
    for (int i = 0; i < VAD_WINDOW; i++) d->twiddle[i] = (cplx){ (float)cos(2.0 * pi * i / VAD_WINDOW), (float)-sin(2.0 * pi * i / VAD_WINDOW) };
    for (int i = 0; i < VAD_WINDOW / 2; i++) {
        const double sine = sin(0.5 * pi * (i + 0.5) / (VAD_WINDOW / 2));
        d->half_window[i] = (float)(1.f / (float)VAD_WINDOW * sin(0.5 * pi * sine * sine));
    }
    for (int i = 0; i < BANDS; i++) {
        for (int j = 0; j < BANDS; j++) d->dct[i * BANDS + j] = (float)cos((i + 0.5) * j * pi / BANDS);
        d->dct[i * BANDS] = (float)(d->dct[i * BANDS] * sqrt(0.5));
    }
    d->frames_to_reset = VAD_RESET_FRAMES;
}

// The resampler takes its input a frame at a time when it runs out; the first it takes is silence.
static void resampler_read(detector *d, const float *source, float *to) {
    if (d->first_pass) memset(to, 0, FRAME * sizeof *to), d->first_pass = false;
    else memcpy(to, source, FRAME * sizeof *to);
}

static void resampler_run(detector *d, const float *source, int frames, float *out) {
    const double ratio = (double)FRAME / VAD_FRAME;
    if (!d->primed && frames) resampler_read(d, source, d->input + d->r0), d->primed = true;
    while (frames) {
        for (int i = (int)ceil((d->block - d->virtual_index) / ratio); i > 0; i--) {
            const int source_index = (int)d->virtual_index;
            const double virtual_offset = (d->virtual_index - source_index) * KERNEL_OFFSETS;
            const int offset = (int)virtual_offset;
            const float *k1 = d->kernel + offset * KERNEL, *k2 = k1 + KERNEL, *in = d->input + source_index;
            const double between = virtual_offset - offset;
            float sum1 = 0, sum2 = 0;
            for (int n = 0; n < KERNEL; n++) sum1 += in[n] * k1[n], sum2 += in[n] * k2[n];
            *out++ = (float)((1.0 - between) * sum1 + between * sum2);
            d->virtual_index += ratio;
            if (!--frames) return;
        }
        d->virtual_index -= d->block;
        memcpy(d->input, d->input + d->r3, KERNEL * sizeof(float));
        if (d->r0 == KERNEL / 2) resampler_regions(d, true);
        resampler_read(d, source, d->input + d->r0);
    }
}

static float dot(const float *x, const float *y, int n) {
    float sum = 0;
    for (int i = 0; i < n; i++) sum += x[i] * y[i];
    return sum;
}

// What linear prediction of order four, smoothed, cannot tell of the buffer from its past.
static void lp_residual(const float *x, float *y) {
    float corr[5], pre[4] = { 0 }, lpc[5] = { 0 };
    for (int lag = 0; lag < 5; lag++) corr[lag] = dot(x, x + lag, PITCH_BUFFER - lag);
    if (corr[0] != 0.f) {
        corr[0] *= 1.0001f, corr[1] -= corr[1] * 0.000064f, corr[2] -= corr[2] * 0.000256f, corr[3] -= corr[3] * 0.000576f, corr[4] -= corr[4] * 0.001024f;
        float error = corr[0];
        for (int i = 0; i < 4; i++) {
            float reflection = 0.f;
            for (int j = 0; j < i; j++) reflection += pre[j] * corr[i - j];
            reflection += corr[i + 1];
            if (fabsf(error) < 1e-6f) error = copysignf(1e-6f, error);
            reflection /= -error;
            pre[i] = reflection;
            for (int j = 0; j < ((i + 1) >> 1); j++) {
                const float a = pre[j], b = pre[i - 1 - j];
                pre[j] = a + reflection * b, pre[i - 1 - j] = b + reflection * a;
            }
            error -= reflection * reflection * error;
            if (error < 0.001f * corr[0]) break;
        }
        pre[0] *= 0.9f, pre[1] *= 0.9f * 0.9f, pre[2] *= 0.9f * 0.9f * 0.9f, pre[3] *= 0.9f * 0.9f * 0.9f * 0.9f;
        lpc[0] = pre[0] + 0.8f, lpc[1] = pre[1] + 0.8f * pre[0], lpc[2] = pre[2] + 0.8f * pre[1], lpc[3] = pre[3] + 0.8f * pre[2], lpc[4] = 0.8f * pre[3];
    }
    for (int i = 0; i < PITCH_BUFFER; i++) {
        float sum = x[i];
        for (int j = 0; j < 5 && j < i; j++) sum += x[i - 1 - j] * lpc[j];
        y[i] = sum;
    }
}

// How alike the newest 20 ms and the 20 ms starting `inverted_lag` into the buffer are; the lag
// between the two is MAX_PITCH less that.
static float pitch_correlation(const float *buffer, int inverted_lag) { return dot(buffer + MAX_PITCH, buffer + inverted_lag, VAD_WINDOW); }

// Which of a lag's neighbours, if either, the true peak leans toward.
static int pitch_offset(float previous, float current, float next) {
    if (next - previous > 0.7f * (current - previous)) return 1;
    if (previous - next > 0.7f * (current - next)) return -1;
    return 0;
}

static void pitch_correlations(const float *buffer, int from, int to, float *correlation, int *lags, int *count) {
    if (from > 0) correlation[from - 1] = 0.f;
    if (to < INITIAL_LAGS - 1) correlation[to + 1] = 0.f;
    for (int lag = from; lag <= to; lag++) correlation[lag] = pitch_correlation(buffer, lag), lags[(*count)++] = lag;
}

// The pitch period in samples at 48 kHz: the two best lags at 12 kHz, refined around each at 24 kHz
// and by half a sample, then checked against its fractions, since the lag found is often a multiple
// of the true period.
static int pitch_estimate(detector *d, const float *buffer) {
    enum { HALF = PITCH_BUFFER / 2, LAGS = (MAX_PITCH - 3 * MIN_PITCH) / 2 };
    float half[HALF], energy[MAX_PITCH + 1], correlation[INITIAL_LAGS];
    for (int i = 0; i < HALF; i++) half[i] = buffer[2 * i];
    int best = 0, second = 1;
    float best_n = -1.f, best_d = 0.f, second_n = -1.f, second_d = 0.f, denominator = 1.f + dot(half, half, VAD_WINDOW / 2 + 1);
    for (int lag = 0; lag < LAGS; lag++) {
        const float c = dot(half + lag, half + MAX_PITCH / 2, VAD_WINDOW / 2);
        if (c > 0.f && c * c * second_d > second_n * denominator) {
            if (c * c * best_d > best_n * denominator) second = best, second_n = best_n, second_d = best_d, best = lag, best_n = c * c, best_d = denominator;
            else second = lag, second_n = c * c, second_d = denominator;
        }
        denominator -= half[lag] * half[lag];
        denominator += half[lag + VAD_WINDOW / 2] * half[lag + VAD_WINDOW / 2];
        denominator = fmaxf(0.f, denominator);
    }
    best *= 2, second *= 2;
    float yy = dot(buffer, buffer, VAD_WINDOW);
    energy[0] = yy;
    for (int lag = 0; lag < MAX_PITCH; lag++) {
        yy -= buffer[lag] * buffer[lag];
        yy += buffer[lag + VAD_WINDOW] * buffer[lag + VAD_WINDOW];
        yy = fmaxf(1.f, yy);
        energy[lag + 1] = yy;
    }
    // Around the two candidates at 24 kHz.
    int lags[10], count = 0;
    const int low = SDL_min(best, second), high = SDL_max(best, second);
    const int from1 = SDL_max(low - 2, 0), to1 = SDL_min(low + 2, INITIAL_LAGS - 1), from2 = SDL_max(high - 2, 0), to2 = SDL_min(high + 2, INITIAL_LAGS - 1);
    if (to1 + 1 >= from2) pitch_correlations(buffer, from1, to2, correlation, lags, &count);
    else pitch_correlations(buffer, from1, to1, correlation, lags, &count), pitch_correlations(buffer, from2, to2, correlation, lags, &count);
    int best_lag = 0;
    best_n = -1.f, best_d = 0.f;
    for (int i = 0; i < count; i++) {
        const int lag = lags[i];
        if (correlation[lag] > 0.f && correlation[lag] * correlation[lag] * best_d > best_n * energy[lag])
            best_lag = lag, best_n = correlation[lag] * correlation[lag], best_d = energy[lag];
    }
    int lag_48 = best_lag * 2;
    if (best_lag != 0 && best_lag < INITIAL_LAGS - 1) lag_48 += pitch_offset(correlation[best_lag + 1], correlation[best_lag], correlation[best_lag - 1]);
    // Against the period's fractions.
    static const int multipliers[14] = { 3, 2, 3, 2, 5, 2, 3, 2, 3, 2, 5, 2, 3, 2 }, thresholds[14] = { 20, 45, 80, 125, 180, 245, 320, 405, 500, 605, 720, 845, 980, 1125 };
    const float x_energy = energy[MAX_PITCH];
    const int initial = SDL_min((2 * MAX_PITCH - lag_48) / 2, MAX_PITCH - 1), last = d->last_pitch.period / 2;
    int period = initial;
    float xy = pitch_correlation(buffer, MAX_PITCH - period), y_energy = energy[MAX_PITCH - period], strength = xy / sqrtf(1.f + x_energy * y_energy);
    const float initial_strength = strength;
    for (int divisor = 2; divisor <= 2 * initial / (2 * MIN_PITCH - 1); divisor++) {
        const int alternative = (2 * initial + divisor) / (2 * divisor);
        int dual = (2 * multipliers[divisor - 2] * initial + divisor) / (2 * divisor);
        if (divisor == 2 && dual > MAX_PITCH) dual = initial;
        const float alternative_xy = 0.5f * (pitch_correlation(buffer, MAX_PITCH - alternative) + pitch_correlation(buffer, MAX_PITCH - dual));
        const float alternative_yy = 0.5f * (energy[MAX_PITCH - alternative] + energy[MAX_PITCH - dual]);
        const float alternative_strength = alternative_xy / sqrtf(1.f + x_energy * alternative_yy);
        float lower = 0.f;  // (a period near the last frame's is taken more readily)
        if (abs(alternative - last) <= 1) lower = d->last_pitch.strength;
        else if (abs(alternative - last) == 2 && initial > thresholds[divisor - 2]) lower = 0.5f * d->last_pitch.strength;
        float threshold = fmaxf(0.3f, 0.7f * initial_strength - lower);
        if (alternative < 3 * MIN_PITCH) threshold = fmaxf(0.4f, 0.85f * initial_strength - lower);
        if (alternative_strength > threshold) period = alternative, strength = alternative_strength, xy = alternative_xy, y_energy = alternative_yy;
    }
    xy = fmaxf(0.f, xy);
    const float final_strength = fminf(strength, y_energy <= xy ? 1.f : xy / (y_energy + 1.f));
    int offset = 0;
    if (period > 0 && period < MAX_PITCH) {
        const int lag = MAX_PITCH - period;
        offset = pitch_offset(pitch_correlation(buffer, lag + 1), pitch_correlation(buffer, lag), pitch_correlation(buffer, lag - 1));
    }
    d->last_pitch = (pitch_info){ SDL_max(2 * MIN_PITCH, 2 * period + offset), final_strength };
    return d->last_pitch.period;
}

// A discrete Fourier transform of n points taken `stride` apart, n a product of twos, threes and fives.
static void fourier(const detector *d, const cplx *in, int stride, cplx *out, int n) {
    if (n == 1) { out[0] = in[0]; return; }
    const int p = n % 2 == 0 ? 2 : n % 3 == 0 ? 3 : 5, m = n / p;
    for (int r = 0; r < p; r++) fourier(d, in + r * stride, stride * p, out + r * m, m);
    for (int k = 0; k < m; k++) {
        cplx t[5];
        for (int r = 0; r < p; r++) t[r] = out[r * m + k];
        for (int q = 0; q < p; q++) {
            cplx sum = t[0];
            for (int r = 1; r < p; r++) {
                const cplx w = d->twiddle[r * (k + q * m) * (VAD_WINDOW / n) % VAD_WINDOW];
                sum.re += t[r].re * w.re - t[r].im * w.im, sum.im += t[r].re * w.im + t[r].im * w.re;
            }
            out[q * m + k] = sum;
        }
    }
}

static void windowed_spectrum(const detector *d, const float *frame, cplx *out) {
    cplx in[VAD_WINDOW];
    for (int i = 0, j = VAD_WINDOW - 1; i < VAD_WINDOW / 2; i++, j--) in[i] = (cplx){ frame[i] * d->half_window[i], 0 }, in[j] = (cplx){ frame[j] * d->half_window[i], 0 };
    fourier(d, in, 1, out, VAD_WINDOW);
    out[0].im = 0.f;
}

// Two spectra multiplied and summed into the bands of Opus's scale, each bin shared between the
// band it is in and the next by how far through the band it lies.
static void band_correlation(const cplx *x, const cplx *y, float *bands) {
    static const int sizes[OPUS_BANDS - 1] = { 4, 4, 4, 4, 4, 4, 4, 4, 8, 8, 8, 8, 16, 16, 16, 24, 24, 32, 48 };
    int k = 0;
    bands[0] = 0.f;
    for (int i = 0; i < OPUS_BANDS - 1; i++) {
        bands[i + 1] = 0.f;
        for (int j = 0; j < sizes[i]; j++, k++) {
            const float v = x[k].re * y[k].re + x[k].im * y[k].im, share = (float)j / (float)sizes[i] * v;
            bands[i] += v - share, bands[i + 1] += share;
        }
    }
    bands[0] *= 2.f;
}

static void cosine_transform(const detector *d, const float *in, int count, float *out, int wanted) {
    for (int i = 0; i < wanted; i++) {
        out[i] = 0.f;
        for (int j = 0; j < count; j++) out[i] += in[j] * d->dct[j * BANDS + i];
        out[i] *= 0.301511345f;
    }
}

static const float *cepstrum_before(const detector *d, int delay) { return d->cepstra[(d->cepstra_tail - 1 - delay + 2 * CEPSTRA) % CEPSTRA]; }

static float distance_between(const detector *d, int delay1, int delay2) {
    int row = CEPSTRA - 1 - delay1, column = CEPSTRA - 1 - delay2;
    if (row > column) { const int t = row; row = column, column = t; }
    return d->distances[row * (CEPSTRA - 1) + column - 1];
}

// The features of the newest 20 ms and of the 20 ms one pitch period before. True for silence,
// when there are none.
static bool spectral_features(detector *d, const float *reference, const float *lagged, float *features) {
    cplx reference_spectrum[VAD_WINDOW], lagged_spectrum[VAD_WINDOW];
    float reference_energy[OPUS_BANDS], lagged_energy[OPUS_BANDS], cross[OPUS_BANDS], log_energy[BANDS], cepstrum[BANDS], total = 0.f;
    windowed_spectrum(d, reference, reference_spectrum);
    band_correlation(reference_spectrum, reference_spectrum, reference_energy);
    for (int i = 0; i < OPUS_BANDS; i++) total += reference_energy[i];
    if (total < 0.04f) return true;
    windowed_spectrum(d, lagged, lagged_spectrum);
    band_correlation(lagged_spectrum, lagged_spectrum, lagged_energy);
    float log_max = -2.f, follow = -2.f;
    for (int i = 0; i < BANDS; i++) {  // (a band may not fall far under the loudest or under its lower neighbour)
        float x = i < OPUS_BANDS ? log10f(1e-2f + reference_energy[i]) : -2.f;
        x = fmaxf(log_max - 7.f, fmaxf(follow - 1.5f, x));
        log_max = fmaxf(log_max, x), follow = fmaxf(follow - 1.5f, x);
        log_energy[i] = x;
    }
    cosine_transform(d, log_energy, BANDS, cepstrum, BANDS);
    cepstrum[0] -= 12.f, cepstrum[1] -= 4.f;
    memcpy(d->cepstra[d->cepstra_tail], cepstrum, sizeof cepstrum);
    d->cepstra_tail = (d->cepstra_tail + 1) % CEPSTRA;
    // How far this cepstrum is from each of the seven before it, kept beside the same for those.
    memmove(d->distances, d->distances + CEPSTRA, ((CEPSTRA - 1) * (CEPSTRA - 1) - CEPSTRA) * sizeof(float));
    for (int i = 0; i < CEPSTRA - 1; i++) {
        const float *old = cepstrum_before(d, i + 1);
        float distance = 0.f;
        for (int k = 0; k < BANDS; k++) distance += (cepstrum[k] - old[k]) * (cepstrum[k] - old[k]);
        d->distances[(CEPSTRA - 1 - i) * (CEPSTRA - 1) - 1] = distance;
    }
    const float *current = cepstrum_before(d, 0), *previous1 = cepstrum_before(d, 1), *previous2 = cepstrum_before(d, 2);
    for (int i = 0; i < LOWER_BANDS; i++) {
        features[i] = current[i] + previous1[i] + previous2[i];
        features[BANDS + i] = current[i] - previous2[i];
        features[BANDS + LOWER_BANDS + i] = current[i] - 2 * previous1[i] + previous2[i];
    }
    memcpy(features + LOWER_BANDS, cepstrum + LOWER_BANDS, (BANDS - LOWER_BANDS) * sizeof(float));
    band_correlation(reference_spectrum, lagged_spectrum, cross);
    for (int i = 0; i < OPUS_BANDS; i++) cross[i] = cross[i] / sqrtf(0.001f + reference_energy[i] * lagged_energy[i]);
    float *correlation = features + BANDS + 2 * LOWER_BANDS;
    cosine_transform(d, cross, OPUS_BANDS, correlation, LOWER_BANDS);
    correlation[0] -= 1.3f, correlation[1] -= 0.9f;
    float variability = 0.f;
    for (int delay1 = 0; delay1 < CEPSTRA; delay1++) {
        float least = 3.402823466e+38f;
        for (int delay2 = 0; delay2 < CEPSTRA; delay2++)
            if (delay1 != delay2) least = fminf(least, distance_between(d, delay1, delay2));
        variability += least;
    }
    features[FEATURES - 1] = variability / CEPSTRA - 2.1f;
    return false;
}

static float tansig(float x) {
    if (!(x < 8.f)) return 1.f;
    if (!(x > -8.f)) return -1.f;
    float sign = 1.f;
    if (x < 0.f) x = -x, sign = -1.f;
    const int i = (int)floorf(0.5f + 25 * x);
    float y = rnn_tansig_table[i];
    x -= 0.04f * (float)i;
    y = y + x * (1.f - y * y) * (1.f - y * x);
    return sign * y;
}

static float sigmoid(float x) { return 0.5f + 0.5f * tansig(0.5f * x); }

// A sum over a layer's inputs for one of its outputs; the weights are stored input by input.
static float weighted(const float *input, int inputs, const int8_t *weights, int stride, int at) {
    float sum = 0.f;
    for (int i = 0; i < inputs; i++) sum += input[i] * (1.f / 256.f * (float)weights[i * stride + at]);
    return sum;
}

// The network: 24 units fully connected, 24 gated recurrent units, and one output.
static float network(detector *d, const float *features) {
    float dense[UNITS], update[UNITS], reset[UNITS], reset_state[UNITS], output;
    for (int o = 0; o < UNITS; o++) dense[o] = tansig(1.f / 256.f * (float)rnn_input_bias[o] + weighted(features, FEATURES, rnn_input_weights, UNITS, o));
    for (int gate = 0; gate < 2; gate++)
        for (int o = 0; o < UNITS; o++) {
            float x = 1.f / 256.f * (float)rnn_gru_bias[gate * UNITS + o];
            x += weighted(dense, UNITS, rnn_gru_weights, 3 * UNITS, gate * UNITS + o);
            x += weighted(d->state, UNITS, rnn_gru_recurrent_weights, 3 * UNITS, gate * UNITS + o);
            (gate ? reset : update)[o] = sigmoid(x);
        }
    for (int o = 0; o < UNITS; o++) reset_state[o] = d->state[o] * reset[o];
    for (int o = 0; o < UNITS; o++) {
        float x = 1.f / 256.f * (float)rnn_gru_bias[2 * UNITS + o];
        x += weighted(dense, UNITS, rnn_gru_weights, 3 * UNITS, 2 * UNITS + o);
        x += weighted(reset_state, UNITS, rnn_gru_recurrent_weights, 3 * UNITS, 2 * UNITS + o);
        d->state[o] = update[o] * d->state[o] + (1.f - update[o]) * fmaxf(0.f, x);
    }
    output = 1.f / 256.f * (float)rnn_output_bias[0];
    for (int i = 0; i < UNITS; i++) output += d->state[i] * (1.f / 256.f * (float)rnn_output_weights[i]);
    return sigmoid(output);
}

// The probability that a frame at 16 kHz holds speech. The network's memory is cleared every second
// and a half, and at silence.
static float detector_analyze(detector *d, const float *frame) {
    float resampled[VAD_FRAME], residual[PITCH_BUFFER], features[FEATURES];
    if (--d->frames_to_reset <= 0) memset(d->state, 0, sizeof d->state), d->frames_to_reset = VAD_RESET_FRAMES;
    if (d->first_pass) resampler_run(d, frame, (int)(d->block / ((double)FRAME / VAD_FRAME)), resampled);
    resampler_run(d, frame, VAD_FRAME, resampled);
    memmove(d->pitch_buffer, d->pitch_buffer + VAD_FRAME, (PITCH_BUFFER - VAD_FRAME) * sizeof(float));
    memcpy(d->pitch_buffer + PITCH_BUFFER - VAD_FRAME, resampled, sizeof resampled);
    lp_residual(d->pitch_buffer, residual);
    const int period = pitch_estimate(d, residual);
    features[FEATURES - 2] = 0.01f * (float)(period - 300);
    if (spectral_features(d, d->pitch_buffer + MAX_PITCH, d->pitch_buffer + MAX_PITCH - period / 2, features)) {
        memset(d->state, 0, sizeof d->state);
        return 0.f;
    }
    return network(d, features);
}

// ---- The gain controller (agc2/) ----
//
// WebRTC's second gain controller, in the form Chromium uses on desktops: an adaptive digital gain
// followed by a limiter. It estimates the level of the speech (frames a voice detector is sure of),
// keeps a headroom for its peaks, and moves the gain, by at most 6 dB a second, toward what brings
// speech 5 dB under full scale plus that headroom, without lifting the noise floor above -50 dB.

#define MIN_DBFS -90.30899869919436f
#define VAD_CONFIDENCE 0.95f
enum { ADJACENT_SPEECH_FRAMES = 12, SUBFRAMES = 20 };

typedef struct { int time_to_confidence_ms; float numerator, denominator; } level_state;
typedef struct { float headroom_db, peaks[4], max_peaks_dbfs; int next, size, time_since_push_ms; } saturation_state;

typedef struct {
    // The noise floor: the least energy of a frame over each five seconds.
    float min_noise_energy, preliminary_noise_energy, noise_energy;
    bool first_period, preliminary_set;
    int noise_counter;
    // The speech level, and the headroom kept for its peaks.
    level_state preliminary_level, reliable_level;
    float level_dbfs;
    int level_adjacent;
    saturation_state preliminary_saturation, reliable_saturation;
    float headroom_db;
    int saturation_adjacent;
    // The gain, and the limiter after it.
    float last_gain_db, last_factor, current_factor;
    int frames_to_increase;
    float limiter_level, limiter_last_scaling;
} gain_controller;

static float to_dbfs(float v) { return v <= 1.0f ? MIN_DBFS : 20.0f * log10f(v) + MIN_DBFS; }
static float db_to_ratio(float v) { return powf(10.0f, v / 20.0f); }

static void saturation_reset(saturation_state *st) { *st = (saturation_state){ .headroom_db = 20.0f, .max_peaks_dbfs = -90.31f }; }

static void gain_init(gain_controller *g) {
    memset(g, 0, sizeof *g);
    g->min_noise_energy = g->preliminary_noise_energy = g->noise_energy = 16000 * 2.0f * 2.0f / 100;
    g->first_period = true, g->noise_counter = 500;
    float initial = SDL_clamp(-20.0f - 15.0f - 5.0f, -90.0f, 30.0f);  // under the initial headroom, the initial gain and the target's headroom
    g->preliminary_level = g->reliable_level = (level_state){ 400, initial, 1.0f };
    g->level_dbfs = initial;
    saturation_reset(&g->preliminary_saturation), saturation_reset(&g->reliable_saturation);
    g->headroom_db = 20.0f;
    g->last_gain_db = 15.0f, g->last_factor = g->current_factor = db_to_ratio(15.0f);
    g->frames_to_increase = ADJACENT_SPEECH_FRAMES;
    g->limiter_last_scaling = 1.0f;
}

// The limiter's curve: the gain for a level, by straight lines between 32 points from just under
// full scale, where it starts to bend, to where it holds the output at full scale.
static float limiter_gain(float level) {
    static const float x[32] = { 30057.296875f, 30148.986328125f, 30240.67578125f, 30424.052734375f, 30607.4296875f, 30790.806640625f, 30974.18359375f, 31157.560546875f,
                                 31340.939453125f, 31524.31640625f, 31707.693359375f, 31891.0703125f, 32074.447265625f, 32257.82421875f, 32441.201171875f, 32624.580078125f,
                                 32807.95703125f, 32991.33203125f, 33174.7109375f, 33358.08984375f, 33541.46484375f, 33724.84375f, 33819.53515625f, 34009.5390625f,
                                 34200.05859375f, 34389.81640625f, 34674.48828125f, 35054.375f, 35434.86328125f, 35814.81640625f, 36195.16796875f, 36575.03125f };
    static const float m[32] = { -3.515235675877192989e-07f, -1.050251626111275982e-06f, -2.085213736791047268e-06f, -3.443004743530764244e-06f, -4.773849468620028347e-06f,
                                 -6.077375928725814447e-06f, -7.353257842623861507e-06f, -8.601219633419532329e-06f, -9.821013009059242904e-06f, -1.101243378798244521e-05f,
                                 -1.217532644659513608e-05f, -1.330956911260727793e-05f, -1.441507538402220234e-05f, -1.549179251014720649e-05f, -1.653970684856176376e-05f,
                                 -1.755882840370759368e-05f, -1.854918446042574942e-05f, -1.951086778717581183e-05f, -2.044398024736437947e-05f, -2.1348627342376858e-05f,
                                 -2.222496914328075945e-05f, -2.265374678245279938e-05f, -2.242570917587727308e-05f, -2.220122041762806475e-05f, -2.19802095671184361e-05f,
                                 -2.176260204578284174e-05f, -2.133731686626560986e-05f, -2.092481918225530535e-05f, -2.052459603874012828e-05f, -2.013615448959171772e-05f,
                                 -1.975903069251216948e-05f, -1.939277899509761482e-05f };
    static const float q[32] = { 1.010565876960754395f, 1.031631827354431152f, 1.062929749488830566f, 1.104239225387573242f, 1.144973039627075195f, 1.185109615325927734f,
                                 1.224629044532775879f, 1.263512492179870605f, 1.301741957664489746f, 1.339300632476806641f, 1.376173257827758789f, 1.412345528602600098f,
                                 1.447803974151611328f, 1.482536554336547852f, 1.516532182693481445f, 1.549780607223510742f, 1.582272171974182129f, 1.613999366760253906f,
                                 1.644955039024353027f, 1.675132393836975098f, 1.704526185989379883f, 1.718986630439758301f, 1.711274504661560059f, 1.703639745712280273f,
                                 1.696081161499023438f, 1.688597679138183594f, 1.673851132392883301f, 1.659391283988952637f, 1.645209431648254395f, 1.631297469139099121f,
                                 1.617647409439086914f, 1.604251742362976074f };
    if (level <= x[0]) return 1.0f;
    if (level >= 36766.300710566735f) return 32768.f / level;
    int index = 0;
    while (index < 32 && x[index] < level) index++;  // the first point not under the level; the line before it
    index--;
    return m[index] * level + q[index];
}


static void gain_process(gain_controller *g, float *frame, float speech_probability) {
    float peak = 0, sum = 0, energy = 0;
    for (int i = 0; i < FRAME; i++) peak = fmaxf(fabsf(frame[i]), peak), sum += frame[i] * frame[i];
    const float peak_dbfs = to_dbfs(peak), rms_dbfs = to_dbfs(sqrtf(sum / FRAME));
    for (int i = 0; i < FRAME; i++) energy = energy + frame[i] * frame[i];
    // The noise floor.
    if (energy > g->min_noise_energy) {
        if (g->preliminary_set) g->preliminary_noise_energy = fminf(g->preliminary_noise_energy, energy);
        else g->preliminary_noise_energy = energy, g->preliminary_set = true;
        if (g->noise_counter == 0) {
            g->first_period = false;
            g->noise_energy = g->noise_energy < g->preliminary_noise_energy ? 0.5f * g->preliminary_noise_energy + 0.5f * g->noise_energy : g->preliminary_noise_energy;
            g->noise_counter = 500, g->preliminary_set = false;
        } else if (g->first_period) g->noise_energy = g->preliminary_noise_energy, g->noise_counter--;
        else g->noise_energy = fminf(g->noise_energy, g->preliminary_noise_energy), g->noise_counter--;
    }
    const float noise_square = g->noise_energy / FRAME, noise_rms_dbfs = noise_square <= 1.0f ? MIN_DBFS : 10.0f * log10f(noise_square) + MIN_DBFS;
    // The speech level: an average of the levels of frames that are surely speech, taken up only
    // once a dozen such frames have come in a row, and forgotten if the run ends sooner.
    const bool speech = !(speech_probability < VAD_CONFIDENCE);
    if (!speech) {
        if (g->level_adjacent >= ADJACENT_SPEECH_FRAMES) g->reliable_level = g->preliminary_level;
        else if (g->level_adjacent > 0) g->preliminary_level = g->reliable_level;
        g->level_adjacent = 0;
    } else {
        g->level_adjacent++;
        const bool full = g->preliminary_level.time_to_confidence_ms == 0;
        if (!full) g->preliminary_level.time_to_confidence_ms -= 10;
        const float leak = full ? 1.0f - 1.0f / 400 : 1.0f;
        g->preliminary_level.numerator = g->preliminary_level.numerator * leak + rms_dbfs * speech_probability;
        g->preliminary_level.denominator = g->preliminary_level.denominator * leak + speech_probability;
        if (g->level_adjacent >= ADJACENT_SPEECH_FRAMES) g->level_dbfs = SDL_clamp(g->preliminary_level.numerator / g->preliminary_level.denominator, -90.0f, 30.0f);
    }
    const bool confident = g->reliable_level.time_to_confidence_ms == 0 || (g->level_adjacent >= ADJACENT_SPEECH_FRAMES && g->preliminary_level.time_to_confidence_ms == 0);
    // The headroom: how far the speech's peaks, 400 ms at a time, stand above its level.
    if (!speech) {
        if (g->saturation_adjacent >= ADJACENT_SPEECH_FRAMES) g->reliable_saturation = g->preliminary_saturation;
        else if (g->saturation_adjacent > 0) g->preliminary_saturation = g->reliable_saturation;
        g->saturation_adjacent = 0;
    } else {
        g->saturation_adjacent++;
        saturation_state *st = &g->preliminary_saturation;
        st->max_peaks_dbfs = fmaxf(st->max_peaks_dbfs, peak_dbfs);
        st->time_since_push_ms += 10;
        if (st->time_since_push_ms > 400) {
            st->peaks[st->next++] = st->max_peaks_dbfs;
            if (st->next == 4) st->next = 0;
            if (st->size < 4) st->size++;
            st->max_peaks_dbfs = -90.31f, st->time_since_push_ms = 0;
        }
        const float delayed_peak = st->size ? st->peaks[st->size == 4 ? st->next : 0] : st->max_peaks_dbfs, difference = delayed_peak - g->level_dbfs;
        if (difference > st->headroom_db) st->headroom_db = st->headroom_db * 0.9988493699365052f + difference * (1.0f - 0.9988493699365052f);
        else st->headroom_db = st->headroom_db * 0.9997697679981565f + difference * (1.0f - 0.9997697679981565f);
        st->headroom_db = SDL_clamp(st->headroom_db, 12.0f, 25.0f);
        if (g->saturation_adjacent >= ADJACENT_SPEECH_FRAMES) g->headroom_db = st->headroom_db;
    }
    // The gain wanted: what brings the speech's peaks 5 dB under full scale, up to 50 dB; no more than
    // keeps the noise under -50 dB; and, while the level is not yet sure, no more than the limiter can take.
    const float input_level = g->level_dbfs + g->headroom_db, limiter_dbfs = to_dbfs(g->limiter_level);
    float target = input_level < -(5.0f + 50.0f) ? 50.0f : input_level < -5.0f ? -5.0f - input_level : 0.0f;
    target = fminf(target, fmaxf(-50.0f - noise_rms_dbfs, 0.0f));
    if (!confident && limiter_dbfs > -1.0f) target = fminf(fmaxf(-1.0f - (limiter_dbfs - g->last_gain_db), 0.0f), target);
    bool first_confident = false;
    if (speech_probability < VAD_CONFIDENCE) g->frames_to_increase = ADJACENT_SPEECH_FRAMES;
    else if (g->frames_to_increase > 0) g->frames_to_increase--, first_confident = g->frames_to_increase == 0;
    const float per_frame = 6.0f * 10 / 1000.0f;
    float difference = target - g->last_gain_db;
    if (g->frames_to_increase != 0) difference = fminf(difference, 0.0f);
    const float change = SDL_clamp(difference, -per_frame, first_confident ? per_frame * ADJACENT_SPEECH_FRAMES : per_frame);
    if (change != 0.f) g->current_factor = db_to_ratio(g->last_gain_db + change);
    // Applied with a ramp across the frame from the gain of the frame before.
    if (!(g->last_factor == g->current_factor && 1.f - 1.f / 32767.0f <= g->current_factor && g->current_factor <= 1.f + 1.f / 32767.0f)) {
        if (g->last_factor == g->current_factor) {
            for (int i = 0; i < FRAME; i++) frame[i] *= g->current_factor;
        } else {
            const float increment = (g->current_factor - g->last_factor) * (1.f / FRAME);
            float gain = g->last_factor;
            for (int i = 0; i < FRAME; i++) frame[i] *= gain, gain += increment;
        }
    }
    g->last_factor = g->current_factor;
    g->last_gain_db = g->last_gain_db + change;
    // The limiter: the level of each half millisecond (the peak, held ahead by one and released
    // slowly) gives a gain from the curve, and the gains are joined by straight lines.
    float envelope[SUBFRAMES] = { 0 }, scaling[SUBFRAMES + 1], per_sample[FRAME];
    const int sub = FRAME / SUBFRAMES;
    for (int k = 0; k < SUBFRAMES; k++)
        for (int i = 0; i < sub; i++) envelope[k] = fmaxf(envelope[k], fabsf(frame[k * sub + i]));
    for (int k = 0; k < SUBFRAMES - 1; k++)
        if (envelope[k] < envelope[k + 1]) envelope[k] = envelope[k + 1];
    for (int k = 0; k < SUBFRAMES; k++) {
        const float value = envelope[k];
        envelope[k] = value > g->limiter_level ? value : value * (1 - 0.9971259f) + g->limiter_level * 0.9971259f;
        g->limiter_level = envelope[k];
    }
    scaling[0] = g->limiter_last_scaling;
    for (int k = 0; k < SUBFRAMES; k++) scaling[k + 1] = limiter_gain(envelope[k]);
    const bool attack = scaling[0] > scaling[1];
    if (attack)
        for (int i = 0; i < sub; i++) per_sample[i] = powf(1.f - (float)(i / sub), 8.0f) * (scaling[0] - scaling[1]) + scaling[1];  // (the original divides whole numbers here, so the first stretch holds the old gain)
    for (int k = attack ? 1 : 0; k < SUBFRAMES; k++) {
        const float step = (scaling[k + 1] - scaling[k]) / sub;
        for (int j = 0; j < sub; j++) per_sample[k * sub + j] = scaling[k] + step * j;
    }
    for (int i = 0; i < FRAME; i++) frame[i] = SDL_clamp(frame[i] * per_sample[i], -32768.0f, 32767.0f);
    g->limiter_last_scaling = scaling[SUBFRAMES];
}

// ---- The whole ----

// The echo canceller is WebRTC's own code (gs_echo.cc), in builds that have it.
typedef struct gs_echo gs_echo;
#ifdef GS_HAVE_ECHO
gs_echo *gs_echo_new(void);
void gs_echo_free(gs_echo *e);
void gs_echo_played(gs_echo *e, const float *frame);
void gs_echo_captured(gs_echo *e, float *frame);
#endif

struct gs_voice {
    int stages;
    gs_echo *echo;
    float speech;  // the detector's answer for the last frame
    float hp_x[2], hp_y[2];  // the high-pass filter's memory
    suppressor ns;
    detector vad;
    gain_controller gain;
};

gs_voice *gs_voice_new(int rate, int stages) {
    if (rate != 16000) return NULL;
    gs_voice *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    v->stages = stages;
    suppressor_init(&v->ns);
    detector_init(&v->vad);
    gain_init(&v->gain);
    if (stages & GS_VOICE_ECHO) {
#ifdef GS_HAVE_ECHO
        v->echo = gs_echo_new();
#endif
        if (!v->echo) return free(v), NULL;
    }
    return v;
}

void gs_voice_free(gs_voice *v) {
    if (!v) return;
#ifdef GS_HAVE_ECHO
    if (v->echo) gs_echo_free(v->echo);
#endif
    free(v);
}

bool gs_voice_has_echo(void) {
#ifdef GS_HAVE_ECHO
    return true;
#else
    return false;
#endif
}

void gs_voice_played(gs_voice *v, const float *frame) {
#ifdef GS_HAVE_ECHO
    if (v->echo) gs_echo_played(v->echo, frame);
#else
    (void)v, (void)frame;
#endif
}

float gs_voice_speech(const gs_voice *v) { return v->speech; }

void gs_voice_frame(gs_voice *v, float *frame) {
    for (int i = 0; i < FRAME; i++) frame[i] *= 32768.f;  // (the original's scale, which its constants assume)
    if (v->stages & GS_VOICE_HIGH_PASS) {  // a second-order Butterworth filter at 100 Hz: rumble and any offset
        static const float b[3] = { 0.97261f, -1.94523f, 0.97261f }, a[2] = { -1.94448f, 0.94598f };
        for (int k = 0; k < FRAME; k++) {
            const float x = frame[k];
            frame[k] = b[0] * x + b[1] * v->hp_x[0] + b[2] * v->hp_x[1] - a[0] * v->hp_y[0] - a[1] * v->hp_y[1];
            v->hp_x[1] = v->hp_x[0], v->hp_x[0] = x, v->hp_y[1] = v->hp_y[0], v->hp_y[0] = frame[k];
        }
    }
#ifdef GS_HAVE_ECHO
    if (v->echo) {  // (after the filter, as the original orders them; it takes samples from -1 to 1)
        for (int i = 0; i < FRAME; i++) frame[i] *= 1.f / 32768.f;
        gs_echo_captured(v->echo, frame);
        for (int i = 0; i < FRAME; i++) frame[i] *= 32768.f;
    }
#endif
    if (v->stages & GS_VOICE_NOISE) suppressor_analyze(&v->ns, frame), suppressor_process(&v->ns, frame);
    if (v->stages & (GS_VOICE_GAIN | GS_VOICE_SPEECH)) v->speech = detector_analyze(&v->vad, frame);
    if (v->stages & GS_VOICE_GAIN) gain_process(&v->gain, frame, v->speech);
    for (int i = 0; i < FRAME; i++) frame[i] *= 1.f / 32768.f;
}
