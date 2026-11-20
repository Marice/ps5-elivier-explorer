// EliVier Explorer - Screen "Probe": what the app can do with each drive on this console.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shows how the app got out of its sandbox (or did not), and for every place whether it is there,
// can be listed and written, its file system and free space. Square measures write and read
// speed on internal storage and the first writable USB drive. Every run is written to
// probe.txt in the report folder, to be read over FTP.

#include "concepts/concepts.hpp"

#include "concepts/explorer/look.hpp"
#include "core/save_file.hpp"
#include "core/tween.hpp"
#include "explorer/probe.hpp"
#include "explorer/trace.hpp"
#include "explorer/worker.hpp"

#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace hui::concepts
{

namespace
{

using gfx::Color;
using gfx::Rect;
using namespace explorer;
using hui::explorer::AccessReport;
using hui::explorer::PlaceCheck;
using hui::explorer::SpeedRun;

constexpr std::uint64_t kSpeedBytes = 100ull << 20;
constexpr std::size_t kSpeedChunk = 2u << 20;

constexpr const char *kTechniques[] = {
    "Plain POSIX on a worker thread: stat, opendir, statvfs, open, write, fsync, read",
    "Each place gets one small test file, written, read back and removed",
    "Speed: 100 MiB in 2 MiB writes, fsynced, read back, removed",
};

constexpr app::TourStep kTour[] = {
    {1.5f, 0, Direction::none, "probe"},
};

std::string g_root;
std::string g_report_folder;

class ProbeScreen final : public app::Concept
{
  public:
    explicit ProbeScreen(app::Context &context) : context_(context)
    {
    }

    const app::ConceptInfo &info() const override
    {
        static const app::ConceptInfo kInfo{
            "probe",
            "Probe",
            "What this console lets the app do",
            "src/concepts/probe.cpp",
            audio::SoundSet::glass,
            Color::rgb(0x1779ba),
            kTechniques,
        };
        return kInfo;
    }

    void enter() override
    {
        age_ = 0.0f;
        if (!started_)
        {
            started_ = true;
            check();
        }
    }

    void update(const InputFrame &input, float dt, app::Feedback &feedback) override
    {
        age_ += dt;
        bool busy = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            busy = busy_;
        }
        if (busy)
            return;
        if (input.is_pressed(Action::confirm))
        {
            feedback.play(audio::Cue::select);
            check();
        }
        else if (input.is_pressed(Action::west))
        {
            feedback.play(audio::Cue::select);
            speed();
        }
    }

    void draw(app::Frame &frame) const override
    {
        const ui::Theme &look = theme();
        begin_frame(frame, look);
        ui::Painter paint(frame.scene, context_.fonts, look);
        frame.scene.push_opacity(tween::cubic_out(age_ / 0.3f));

        std::lock_guard<std::mutex> lock(mutex_);
        draw_header(paint, "Probe", "What this console lets the app do", busy_ ? status_ : "");
        const AccessReport &access = hui::explorer::access_report();
        char line[256];
        std::snprintf(line, sizeof(line), "Access: %s%s%s   uid %d to %d   /app0 %s%s",
                      hui::explorer::route_name(access.route),
                      access.request_taken.empty() ? "" : " ", access.request_taken.c_str(),
                      access.uid_before, access.uid_after, access.app_folder ? "there" : "gone",
                      access.root_put_back ? "   sandbox root back" : "");
        float y = kTop + 10.0f;
        paint.body(line, kMargin, y, 26.0f, paint.page_text());
        y += 56.0f;

        // Columns: place, file system, list, write, free.
        const float columns[] = {kMargin, kMargin + 520.0f, kMargin + 760.0f, kMargin + 1060.0f,
                                 kMargin + 1360.0f};
        const char *const kHeads[] = {"PLACE", "FILE SYSTEM", "LIST", "WRITE", "FREE"};
        for (int i = 0; i < 5; ++i)
            paint.label(kHeads[i], columns[i], y, 18.0f, paint.page_text_muted());
        y += 14.0f;
        paint.fill({kMargin, y, kRight - kMargin, 1.5f}, 0.0f, look.outline);
        y += 34.0f;
        for (const PlaceCheck &check : places_)
        {
            const Color ink = check.exists ? look.text : look.text_muted;
            paint.body(check.label + "  " + check.path, columns[0], y, 22.0f, ink);
            if (check.exists)
            {
                paint.body(check.file_system.empty() ? "-" : check.file_system, columns[1], y,
                           22.0f, ink);
                std::snprintf(line, sizeof(line), check.listable ? "%d entries" : "no (errno %d)",
                              check.listable ? check.entries : check.list_error);
                paint.body(line, columns[2], y, 22.0f, check.listable ? look.success : look.danger);
                if (check.write_error == hui::explorer::kWriteSkipped)
                    std::snprintf(line, sizeof(line), "not tried");
                else if (!check.mounted && check.path != "/data" && check.path != "/user")
                    std::snprintf(line, sizeof(line), "no drive");
                else
                    std::snprintf(line, sizeof(line), check.writable ? "yes" : "no (errno %d)",
                                  check.write_error);
                const bool good = check.writable && check.mounted;
                paint.body(line, columns[3], y, 22.0f, good ? look.success : look.text_muted);
                paint.body(size_text(check.free_bytes) + " of " + size_text(check.total_bytes),
                           columns[4], y, 22.0f, ink);
            }
            else
            {
                paint.body("not there", columns[1], y, 22.0f, ink);
            }
            y += 36.0f;
        }
        y += 16.0f;
        for (const SpeedRun &run : speeds_)
        {
            if (run.result.ok)
                std::snprintf(line, sizeof(line), "Speed %s: write %.1f MB/s, read %.1f MB/s",
                              run.folder.c_str(), run.result.write_mb_s, run.result.read_mb_s);
            else
                std::snprintf(line, sizeof(line), "Speed %s: failed (errno %d)", run.folder.c_str(),
                              run.result.error);
            paint.body(line, kMargin, y, 22.0f, paint.page_text());
            y += 34.0f;
        }
        if (!saved_.empty())
            paint.body("Report: " + saved_, kMargin, y + 8.0f, 20.0f, paint.page_text_muted());

        static constexpr ui::Hint kHints[] = {
            {ui::Button::cross, "Check again"},
            {ui::Button::square, "Measure speed"},
        };
        draw_hints(frame.scene, context_.fonts, look, kHints);
        frame.scene.pop_opacity();
    }

    std::span<const app::TourStep> tour() const override
    {
        return kTour;
    }

  private:
    void check()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            busy_ = true;
            status_ = "CHECKING";
        }
        worker_.queue(
            [this]
            {
                hui::explorer::trace("probe: checking the places");
                std::vector<PlaceCheck> places =
                    hui::explorer::check_places(g_root, hui::explorer::probe_places());
                hui::explorer::trace("probe: %zu places checked", places.size());
                std::lock_guard<std::mutex> lock(mutex_);
                places_ = std::move(places);
                busy_ = false;
                save_locked();
            });
    }

    void speed()
    {
        std::vector<std::string> folders;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            busy_ = true;
            status_ = "MEASURING";
            for (const PlaceCheck &check : places_)
            {
                const bool usb = check.path.rfind("/mnt/usb", 0) == 0;
                if (check.writable && (check.path == "/data" || usb))
                    folders.push_back(check.path);
                if (usb && check.writable)
                    break; // the first writable USB drive is enough
            }
        }
        worker_.queue(
            [this, folders]
            {
                std::vector<SpeedRun> runs;
                for (const std::string &folder : folders)
                {
                    SpeedRun run;
                    run.folder = folder;
                    run.chunk = kSpeedChunk;
                    run.result =
                        hui::explorer::measure_speed(g_root + folder, kSpeedBytes, kSpeedChunk);
                    runs.push_back(run);
                }
                std::lock_guard<std::mutex> lock(mutex_);
                speeds_ = std::move(runs);
                busy_ = false;
                save_locked();
            });
    }

    // Writes probe.txt; the caller holds mutex_.
    void save_locked()
    {
        if (g_report_folder.empty())
            return;
        save::ensure_directory(g_report_folder);
        const std::string path = g_report_folder + "/probe.txt";
        const std::string text =
            hui::explorer::probe_report(hui::explorer::access_report(), places_, speeds_);
        saved_ = save::write_atomic(path, text).empty() ? path : path + " (not written)";
        hui::explorer::trace("probe: report %s", saved_.c_str());
    }

    app::Context &context_;
    float age_ = 0.0f;
    bool started_ = false;
    mutable std::mutex mutex_;
    bool busy_ = false;
    std::string status_;
    std::vector<PlaceCheck> places_;
    std::vector<SpeedRun> speeds_;
    std::string saved_;
    hui::explorer::JobWorker worker_; // last: joined first, before what its jobs use
};

} // namespace

void set_probe_paths(std::string root, std::string report_folder)
{
    g_root = std::move(root);
    g_report_folder = std::move(report_folder);
}

std::unique_ptr<app::Concept> make_probe(app::Context &context)
{
    return std::make_unique<ProbeScreen>(context);
}

} // namespace hui::concepts
