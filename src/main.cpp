// ps5-homebrew-ui - Application entry point.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Opens the display, controller and audio, then runs the shell every frame:
// read input, update, play the sounds it asked for, draw, present. Lifecycle
// markers and frame pacing are logged for hardware runs.

#include "app/shell.hpp"
#include "app/tour.hpp"
#include "audio/cues.hpp"
#include "audio/mixer.hpp"
#include "audio/music.hpp"
#include "core/frame_stats.hpp"
#include "core/input.hpp"
#include "core/save_file.hpp"
#include "core/settings.hpp"
#include "core/version.hpp"
#include "demo/catalog.hpp"
#include "explorer/access_report.hpp"
#include "explorer/trace.hpp"
#include "concepts/concepts.hpp"
#include "gfx/canvas.hpp"
#include "gfx/gl_texture_host.hpp"
#include "gfx/image.hpp"
#include "gfx/hdr_presenter.hpp"
#include "gfx/renderer.hpp"
#include "platform/ps5/audio_out.hpp"
#include "platform/ps5/crash.hpp"
#include "platform/ps5/display_egl.hpp"
#include "platform/ps5/hen_access.hpp"
#include "platform/ps5/hdr_output.hpp"
#include "viewer/media_memory.hpp"
#include "platform/ps5/hw_video.hpp"
#include "platform/ps5/surround_out.hpp"
#include "platform/ps5/video_out_hook.hpp"
#include "platform/ps5/pad.hpp"
#include "platform/ps5/probes.hpp"
#include "platform/ps5/system.hpp"
#include "probe/plan.hpp"
#include "probe/report.hpp"
#include "ui/fonts.hpp"

#include <cstring>
#include <GL/glcorearb.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <vector>

extern "C" void hui_heap_stats(std::size_t *live_bytes, std::size_t *peak_bytes,
                               std::size_t *blocks, std::size_t *failures);

namespace
{

// The app's own files. A HEN that frees the app with the console's root takes /app0 away; the
// install folder holds the same files then.
constexpr const char *kAppFolder = "/app0";
constexpr const char *kInstallFolder = "/data/homebrew/LAPY69432";
std::string g_app_folder = kAppFolder;
// Settings live in the title's own save area.
constexpr const char *kDataRoot = "/download0/hui";
// Development output (the tour's pictures and report, the log). A title's
// sandbox has no /data, so it lives in the title's own storage. While the
// title runs, a PC can *read* the folder over FTP as
// /mnt/sandbox/<TITLE_ID>_000/download0/hui/dev, but cannot write there.
constexpr const char *kDevRoot = "/download0/hui/dev";
// Requests therefore come in through the install folder, which a PC can
// write: /data/homebrew/<TITLE_ID>/dev/request.txt is /app0/dev/request.txt
// here. One line: "tour <design id|all> <token>" or "quit - <token>". A
// request is honoured once per token (the last one is remembered in kDevRoot).
constexpr const char *kRequestFile = "/dev/request.txt";
// How long a finished tour stays up so the PC can copy its evidence (the
// storage is unmounted when the title closes) before the app closes itself.
constexpr double kCollectSeconds = 240.0;
// The same for a probe run, which has one short text file to collect, not hundreds of pictures.
constexpr double kProbeCollectSeconds = 120.0;
// Quarter of a quarter: the title's storage is small and a tour takes
// hundreds of pictures. Enough to compare layout and colour with the PC's.
constexpr int kCaptureWidth = 480;
constexpr int kCaptureHeight = 270;

void log_heap(std::uint64_t frames)
{
    std::size_t live = 0;
    std::size_t peak = 0;
    std::size_t blocks = 0;
    std::size_t failures = 0;
    hui_heap_stats(&live, &peak, &blocks, &failures);
    hui::sys::log("[HUI] heap frames=%llu live=%zu peak=%zu blocks=%zu failures=%zu",
                  static_cast<unsigned long long>(frames), live, peak, blocks, failures);
}

// Opens at *resolution (an index into Settings::kResolutions), falling back
// to 1080p; *resolution reports the mode that opened.
bool open_display(hui::ps5::Display &display, int *resolution)
{
    using hui::Settings;
    const Settings::Resolution &mode = Settings::kResolutions[*resolution];
    if (display.open(mode.width, mode.height))
    {
        hui::sys::log("[HUI] display mode %s %dx%d", mode.label, display.width(), display.height());
        return true;
    }
    if (*resolution == 0)
        return false;
    hui::sys::log("[HUI] display mode %s failed, using 1080p", mode.label);
    *resolution = 0;
    return display.open(Settings::kResolutions[0].width, Settings::kResolutions[0].height);
}

bool load_font(hui::gfx::Renderer &renderer, const char *name, hui::gfx::Font *font,
               hui::ui::FontRef *ref)
{
    std::string data;
    const std::string path = g_app_folder + "/assets/fonts/" + name;
    if (!hui::save::read_file(path, &data) || !font->load(data))
    {
        hui::sys::log("[HUI] font %s failed: %s", name, font->error().c_str());
        return false;
    }
    ref->font = font;
    ref->texture = renderer.batch().create_font_texture(*font);
    return true;
}

// The frame as drawn, bottom row first, as a 24-bit BMP (tour pictures).
bool save_picture(const std::string &path, int width, int height)
{
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    const std::uint32_t row = (static_cast<std::uint32_t>(width) * 3 + 3) & ~3u;
    const std::uint32_t size = 54 + row * static_cast<std::uint32_t>(height);
    unsigned char header[54] = {'B', 'M'};
    const auto put = [&](int at, std::uint32_t value)
    {
        for (int i = 0; i < 4; ++i)
            header[at + i] = static_cast<unsigned char>(value >> (8 * i));
    };
    put(2, size);
    put(10, 54);
    put(14, 40);
    put(18, static_cast<std::uint32_t>(width));
    put(22, static_cast<std::uint32_t>(height));
    header[26] = 1;
    header[28] = 24;
    put(34, size - 54);
    std::FILE *file = std::fopen(path.c_str(), "wb");
    if (file == nullptr)
        return false;
    bool ok = std::fwrite(header, 1, sizeof(header), file) == sizeof(header);
    std::vector<unsigned char> line(row);
    for (int y = 0; ok && y < height; ++y)
    {
        const unsigned char *in = pixels.data() + static_cast<std::size_t>(y) * width * 4;
        for (int x = 0; x < width; ++x)
        {
            line[static_cast<std::size_t>(x) * 3 + 0] = in[x * 4 + 2];
            line[static_cast<std::size_t>(x) * 3 + 1] = in[x * 4 + 1];
            line[static_cast<std::size_t>(x) * 3 + 2] = in[x * 4 + 0];
        }
        ok = std::fwrite(line.data(), 1, line.size(), file) == line.size();
    }
    return std::fclose(file) == 0 && ok;
}

// A request line "verb argument token" for `wanted_verb` that no earlier launch honoured. The token
// is remembered at once, so a request that crashes the app is not honoured again by the next start.
bool take_request(const char *wanted_verb, std::string *argument)
{
    std::string request;
    if (!hui::save::read_file(g_app_folder + kRequestFile, &request, 256))
        return false;
    char verb[16] = {};
    char word[64] = {};
    char token[64] = {};
    if (std::sscanf(request.c_str(), "%15s %63s %63s", verb, word, token) != 3 ||
        std::strcmp(verb, wanted_verb) != 0)
        return false;
    const std::string handled_path = std::string(kDevRoot) + "/handled.txt";
    std::string handled;
    hui::save::read_file(handled_path, &handled, 128);
    if (handled == token)
        return false;
    hui::save::ensure_directory(kDevRoot);
    hui::save::write_atomic(handled_path, token);
    *argument = word;
    return true;
}

// A probe launch: no shell, no music, no controller. The probes of the request run, the answers
// go to dev/probe/report.txt (and the log, line by line as they come), and the app stays up for
// a while so that the PC can copy them (the storage goes away when the title closes) before it
// closes itself. A "quit" request ends the wait early.
void run_probe_launch(hui::ps5::Display &display, int *resolution, const std::string &name,
                      const hui::probe::Report &early)
{
    using namespace hui;
    probe::ProbePlan plan;
    probe::Report report;
    std::string error;
    save::ensure_directory(std::string(kDevRoot) + "/probe");
    sys::log("[HUI] probe run requested: %s", name.c_str());
    explorer::trace("probe launch: request '%s'", name.c_str());
    report.add("run", "request", name);
    if (probe::parse_probe_plan(name, plan, &error))
    {
        // decoder-bind ran at the start of main(), before the HEN; it is not called again here.
        if (!early.lines().empty())
            plan.decoder_bind = false;
        ps5::run_probes(plan, display, report);
    }
    else
    {
        report.note("run", "refused: " + error);
        sys::log("[HUI] probe refused: %s", error.c_str());
    }
    report.insert_front(early);
    save::write_atomic(std::string(kDevRoot) + "/probe/report.txt", report.text());
    // A copy beside the trace, where a PC can still read it after the app has closed.
    const std::string trace_file = explorer::trace_path();
    const std::size_t slash = trace_file.rfind('/');
    if (slash != std::string::npos)
        save::write_atomic(trace_file.substr(0, slash) + "/probe-report.txt", report.text());
    sys::log("[HUI] probe run finished lines=%zu", report.lines().size());
    explorer::trace("probe run finished: %zu lines; waiting %.0f s to be collected",
                    report.lines().size(), kProbeCollectSeconds);

    // An HDR attempt leaves the display closed; a plain one is enough for the wait.
    if (display.width() == 0 && !open_display(display, resolution))
        sys::log("[HUI] probe: no display for the wait; waiting without one");
    double waited = 0.0;
    std::int64_t last = sys::monotonic_us();
    for (std::uint64_t frame = 0;; ++frame)
    {
        if (display.width() > 0)
        {
            glClearColor(0.04f, 0.04f, 0.05f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            if (!display.swap())
                sys::sleep_us(16000);
        }
        else
        {
            sys::sleep_us(16000);
        }
        const std::int64_t now = sys::monotonic_us();
        waited += static_cast<double>(now - last) / 1e6;
        last = now;
        std::string ignored;
        if (frame % 60 == 0 && (take_request("quit", &ignored) || waited > kProbeCollectSeconds))
        {
            sys::log("[HUI] closing: probe %s", waited > kProbeCollectSeconds
                                                    ? "evidence window over"
                                                    : "quit requested");
            explorer::trace("probe launch: closing");
            sys::quit();
        }
    }
}

} // namespace

int main()
{
    using namespace hui;
    sys::log("[HUI] entry");
    // The trace first: /data is there with etaHEN's "Allow /data in sandbox"; the app's own
    // storage is the fallback (a PC cannot read it once the title has closed).
    static const char *const kTraceFolders[] = {"/data/elivier-explorer", "/download0/hui"};
    explorer::trace_open(kTraceFolders);
    ps5::catch_crashes(reinterpret_cast<const void *>(&open_display));
    explorer::trace("entry; trace in %s", explorer::trace_path());
    // Out of the sandbox before anything else: the HEN must see one thread, and every file the
    // app reads afterwards may live elsewhere.
    // A probe request is read before anything else: decoder-bind runs now, before the HEN frees
    // the app, because that is when EVO Player brings its decoders up (docs/decision-log.md, D48).
    std::string probe_name;
    probe::Report early_report;
    const bool probe_asked = take_request("probe", &probe_name);
    if (probe_asked && probe_name == "decoder-bind")
    {
        explorer::trace("probe launch: decoder-bind, before the HEN frees the app");
        ps5::run_decoder_bind_probe(early_report, "early");
    }
    if (probe_asked && probe_name == "hwdec")
    {
        explorer::trace("probe launch: hwdec, bringing a decoder up before the HEN frees the app");
        ps5::run_hwdec_bringup(early_report);
    }
    // The hardware decoders answer only before the HEN frees the app, and keep working after it
    // (docs/decision-log.md, D60): they are made now, once, for the whole run.
    if (!(probe_asked && (probe_name == "hwdec" || probe_name == "decoder-bind")))
        ps5::hw_video_bring_up();
    ps5::hen_prepare();
    explorer::trace("asking the HEN to free the app");
    explorer::AccessReport access = ps5::hen_free(4, kInstallFolder);
    sys::log("[EXPLORER] access %s in %d ms, uid %d to %d, /app0 %d",
             explorer::route_name(access.route), access.milliseconds, access.uid_before,
             access.uid_after, access.app_folder ? 1 : 0);
    if (!access.app_folder && !access.root_put_back)
        g_app_folder = kInstallFolder;
    explorer::trace("access %s, /app0 %d; app files from %s", explorer::route_name(access.route),
                    access.app_folder ? 1 : 0, g_app_folder.c_str());
    explorer::set_access_report(std::move(access));
    const std::string assets = g_app_folder + "/assets";
    sys::log("[HUI] storage dir=%d", save::ensure_directory(kDataRoot) ? 1 : 0);

    // The display opens at the saved resolution (1080p if that fails).
    const Settings saved = app::Shell::load_settings(kDataRoot);
    int resolution = ps5::Display::supports_display_modes() ? saved.resolution : 0;
    ps5::Display display;
    explorer::trace("opening the display (mode %d)", resolution);
    if (!open_display(display, &resolution))
    {
        sys::log("[HUI] fatal: display open failed");
        explorer::trace("FATAL: display open failed");
        sys::park();
    }
    explorer::trace("display open %dx%d", display.width(), display.height());
    ps5::trace_video_out_status("after the display opened");

    // A probe request turns this launch into a measurement run (docs/CONSOLE_VALIDATION.md).
    if (probe_asked)
        run_probe_launch(display, &resolution, probe_name, early_report);

    gfx::Renderer renderer;
    gfx::Font regular;
    gfx::Font semibold;
    gfx::Font display_font;
    gfx::Font mono;
    gfx::Font pixel;
    gfx::Font hand;
    ui::Fonts fonts;
    explorer::trace("renderer and fonts");
    if (!renderer.init() ||
        !load_font(renderer, "inter-regular.huifont", &regular, &fonts.regular) ||
        !load_font(renderer, "inter-semibold.huifont", &semibold, &fonts.semibold) ||
        !load_font(renderer, "montserrat-medium.huifont", &display_font, &fonts.display) ||
        !load_font(renderer, "dejavu-sans-mono.huifont", &mono, &fonts.mono) ||
        !load_font(renderer, "press-start-2p.huifont", &pixel, &fonts.pixel) ||
        !load_font(renderer, "patrick-hand.huifont", &hand, &fonts.hand))
    {
        sys::log("[HUI] fatal: renderer init failed");
        explorer::trace("FATAL: renderer init or a font failed (fonts from %s)",
                        g_app_folder.c_str());
        sys::park();
    }
    explorer::trace("fonts loaded; cover art");
    demo::Catalog catalog;
    const std::int64_t art_start = sys::monotonic_us();
    if (!catalog.build_covers(renderer, fonts))
        sys::log("[HUI] cover art failed; designs draw without it");
    sys::log("[HUI] cover art %zu covers in %lld ms", catalog.size(),
             static_cast<long long>((sys::monotonic_us() - art_start) / 1000));

    explorer::trace("pad and audio");
    ps5::Pad pad;
    pad.open();
    InputTracker tracker;
    audio::Mixer mixer;
    // The music stream attaches to the mixer before the audio thread starts.
    audio::MusicPlayer music;
    const int tracks =
        music.init(mixer, assets + "/audio/music", static_cast<std::uint64_t>(sys::monotonic_us()));
    sys::log("[HUI] music songs=%d", tracks);
    ps5::AudioOut audio_out;
    ps5::SurroundOut surround_out; // opened by a film with surround sound, closed after it
    // HDR10 for an HDR film in full screen (D68, D71): the frame is then packed for the scanout.
    gfx::HdrPresenter hdr_presenter;
    ps5::HdrOut hdr_out(hdr_presenter, display);
    audio_out.start(mixer);
    audio::SoundBank sounds;
    const auto bank = sounds.load(assets + "/audio/sfx");
    sys::log("[HUI] sounds files=%d rejected=%d", bank.files, bank.rejected);
    for (const std::string &error : bank.errors)
        sys::log("[HUI] sound rejected %s", error.c_str());

    hui::concepts::set_about_qr(hui::gfx::load_png_texture, assets + "/about/ko-fi-qr.png");
    hui::concepts::set_explorer_start("/data", "/mnt");
    hui::gfx::GlTextureHost texture_host;
    // Where films were left and the languages last picked, beside the settings (D73).
    viewer::MediaMemory media_memory(std::string(kDataRoot) + "/media.bin");
    media_memory.load();
    hui::concepts::set_explorer_services(&texture_host, &mixer, &surround_out, &hdr_out,
                                         &media_memory);
    explorer::trace("shell");
    app::Shell shell(fonts, catalog, kDataRoot, renderer.glass_texture());
    const std::string version = read_content_version(g_app_folder + "/sce_sys/param.json");
    shell.set_version(version);
    sys::log("[HUI] version %s designs=%zu", version.empty() ? "unknown" : version.c_str(),
             shell.concept_count());

    // A tour request turns this launch into an unattended validation run:
    // every design is shown with scripted input, pictures and a report are
    // written to kDevRoot, and the app closes itself a while later.
    std::unique_ptr<app::Tour> tour;
    save::ensure_directory(kDevRoot);
    const std::string handled_path = std::string(kDevRoot) + "/handled.txt";
    const std::string report_path = std::string(kDevRoot) + "/tour/report.txt";
    bool quit_requested = false;
    const auto poll_request = [&]()
    {
        std::string request;
        if (!save::read_file(g_app_folder + kRequestFile, &request, 256))
            return;
        char verb[16] = {};
        char argument[64] = {};
        char token[64] = {};
        if (std::sscanf(request.c_str(), "%15s %63s %63s", verb, argument, token) != 3)
            return;
        std::string handled;
        save::read_file(handled_path, &handled, 128);
        if (handled == token)
            return; // this request was honoured by an earlier launch (or frame)
        save::write_atomic(handled_path, token);
        if (std::strcmp(verb, "quit") == 0)
        {
            quit_requested = true;
        }
        else if (std::strcmp(verb, "tour") == 0 && !tour)
        {
            save::ensure_directory(std::string(kDevRoot) + "/tour");
            std::remove(report_path.c_str()); // a new report must not be taken for an old one
            const bool all = std::strcmp(argument, "all") == 0;
            tour = std::make_unique<app::Tour>(shell, all ? std::string() : std::string(argument));
            sys::log("[HUI] tour requested: %s token=%s", argument, token);
        }
    };
    poll_request();
    double collect_wait = -1.0; // seconds a finished tour has waited to be collected

    explorer::trace("first frame");
    std::int64_t previous = sys::monotonic_us();
    std::uint64_t frames = 0;
    FrameStats stats;
    // The FPS readout averages over half a second so the number is readable.
    double fps_seconds = 0.0;
    int fps_frames = 0;
    PadSample samples[64];
    gfx::Canvas capture;
    std::int64_t last_frame_start = sys::monotonic_us();
    for (;;)
    {
        const std::int64_t now = sys::monotonic_us();
        // Animation time is start-to-start (one full frame), not the gap
        // between the previous swap returning and this frame beginning.
        float dt = frames == 0 ? 1.0f / 60.0f : static_cast<float>(now - last_frame_start) / 1e6f;
        last_frame_start = now;
        if (dt > 0.05f)
            dt = 0.05f; // a hitch must not teleport the animations
        const std::size_t count = pad.read(samples);
        InputFrame input = tracker.update(std::span<const PadSample>(samples, count),
                                          static_cast<std::uint64_t>(now));
        if (tour)
            input = tour->step(dt);

        shell.update(input, dt);
        if (frames == 0)
            explorer::trace("frame 1: updated");
        if (shell.take_settings_changed())
        {
            const Settings &settings = shell.settings();
            mixer.set_bus_gain(audio::Bus::music, Settings::gain(settings.music_volume));
            mixer.set_bus_gain(audio::Bus::sfx, Settings::gain(settings.sfx_volume));
            mixer.set_bus_gain(audio::Bus::ui, Settings::gain(settings.ui_volume));
            InputSettings input_settings = tracker.settings();
            input_settings.swap_confirm = settings.swap_confirm;
            tracker.set_settings(input_settings);
        }
        const app::Feedback &feedback = shell.feedback();
        for (const audio::CueEvent &event : feedback.cues)
        {
            sounds.play(mixer, event.set == audio::SoundSet::count ? shell.sound_set() : event.set,
                        event);
            if (event.cue == audio::Cue::complete || event.cue == audio::Cue::welcome)
                music.duck();
        }
        if (shell.settings().haptics && feedback.rumble_strength > 0.0f)
            pad.rumble(feedback.rumble_strength, feedback.rumble_seconds);
        pad.tick(dt);
        if (shell.settings().light_bar)
        {
            const gfx::Color accent = shell.accent();
            pad.set_light_bar(static_cast<std::uint8_t>(accent.r * 255.0f),
                              static_cast<std::uint8_t>(accent.g * 255.0f),
                              static_cast<std::uint8_t>(accent.b * 255.0f));
        }
        music.pump(dt);

        shell.compose(renderer);
        if (frames == 0)
            explorer::trace("frame 1: composed");
        // In HDR10 mode the frame goes into the presenter's target and is packed from there.
        const GLuint target =
            hdr_out.active() ? hdr_presenter.begin(display.width(), display.height()) : 0;
        glBindFramebuffer(GL_FRAMEBUFFER, target);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        renderer.present(target, display.width(), display.height());
        if (target != 0)
            hdr_presenter.finish(display.width(), display.height());
        if (frames == 0)
            explorer::trace("frame 1: presented");
        if (tour && !tour->capture().empty())
        {
            // The picture is the same frame drawn once more into a small
            // off-screen target: reading the display surface back is slow, and
            // a quarter-size picture is enough to compare with the PC render.
            if (capture.texture() == 0)
                capture.create(kCaptureWidth, kCaptureHeight, 1);
            capture.bind();
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            renderer.present(capture.framebuffer(), kCaptureWidth, kCaptureHeight);
            glBindFramebuffer(GL_FRAMEBUFFER, capture.framebuffer());
            const std::string path = std::string(kDevRoot) + "/tour/" + tour->capture() + ".bmp";
            const bool ok = save_picture(path, kCaptureWidth, kCaptureHeight);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            sys::log("[HUI] tour picture %s ok=%d", tour->capture().c_str(), ok ? 1 : 0);
            tour->capture_done();
            last_frame_start = sys::monotonic_us(); // saving is slow; the frame was not
        }
        if (!display.swap())
        {
            sys::log("[HUI] fatal: swap failed frame=%llu error=%s",
                     static_cast<unsigned long long>(frames),
                     ps5::egl_error_name(display.last_error()));
            sys::park();
        }
        ++frames;
        const std::int64_t presented = sys::monotonic_us();
        const float frame_ms = static_cast<float>(presented - previous) / 1000.0f;
        app::Telemetry &telemetry = shell.telemetry();
        if (frames == 1)
        {
            sys::log("[HUI] first-swap ok shapes=%zu draws=%zu", renderer.last_instances(),
                     renderer.last_draw_calls());
            explorer::trace("first swap ok");
            const bool hidden = sys::hide_splash_screen();
            sys::log("[HUI] ready splash_hidden=%d", hidden ? 1 : 0);
            log_heap(frames);
        }
        else
        {
            stats.add(static_cast<double>(frame_ms));
            telemetry.push(frame_ms);
            if (tour)
                tour->record_frame(frame_ms, renderer.last_draw_calls(), renderer.last_instances());
        }
        previous = presented;
        telemetry.draw_calls = renderer.last_draw_calls();
        telemetry.instances = renderer.last_instances();
        telemetry.voices = mixer.active_voices();
        fps_seconds += static_cast<double>(frame_ms) / 1000.0;
        ++fps_frames;
        if (fps_seconds >= 0.5)
        {
            telemetry.fps = static_cast<float>(fps_frames / fps_seconds);
            telemetry.average_ms = static_cast<float>(fps_seconds * 1000.0 / fps_frames);
            fps_seconds = 0.0;
            fps_frames = 0;
        }

        if (tour && tour->finished())
        {
            std::string report;
            for (const std::string &line : tour->report())
            {
                sys::log("[HUI] %s", line.c_str());
                report += line + "\n";
            }
            save::write_atomic(report_path, report);
            sys::log("[HUI] tour finished designs=%zu", tour->report().size());
            // The storage is unmounted when the title closes, so the app stays
            // up for a while: the PC copies the evidence, then the app closes.
            tour.reset();
            collect_wait = 0.0;
        }
        if (collect_wait >= 0.0)
            collect_wait += static_cast<double>(frame_ms) / 1000.0;
        if (frames % 60 == 0)
        {
            poll_request();
            if (quit_requested || collect_wait > kCollectSeconds)
            {
                sys::log("[HUI] closing: %s",
                         quit_requested ? "quit requested" : "tour evidence window over");
                pad.close();
                sys::quit();
            }
        }
        if (stats.count() == 600)
        {
            char summary[160];
            stats.format(summary, sizeof(summary));
            sys::log("[HUI] %s draws=%zu shapes=%zu", summary, renderer.last_draw_calls(),
                     renderer.last_instances());
            sys::log("[HUI] audio grains=%llu errors=%llu voices=%d music=%s underruns=%llu",
                     static_cast<unsigned long long>(audio_out.grains()),
                     static_cast<unsigned long long>(audio_out.errors()), mixer.active_voices(),
                     music.current().c_str(),
                     static_cast<unsigned long long>(mixer.stream_underruns()));
            stats.reset();
            if (frames % 3600 < 600)
                log_heap(frames);
        }
    }
}
