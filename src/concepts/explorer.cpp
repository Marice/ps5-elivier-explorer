// EliVier Explorer - Screen "Files": the file explorer, two panes side by side.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Each pane is a hui::explorer::PaneModel (folder, sorted rows, selection, focus) shown in a
// ui::Table; a ui::SplitView holds the two and moves the focus across. The table only shows:
// Cross is taken from its input (it would mark the row) and sorting is the model's, so folders
// stay on top and a capped folder sorts the whole folder before it is cut. At a table's left or
// right edge the focus crosses to the other pane.

#include "concepts/concepts.hpp"

#include "concepts/explorer/icons.hpp"
#include "concepts/explorer/look.hpp"
#include "concepts/explorer/marquee.hpp"
#include "concepts/explorer/row_format.hpp"
#include "core/held_step.hpp"
#include "core/tween.hpp"
#include "explorer/copy_job.hpp"
#include "explorer/guard.hpp"
#include "explorer/local_source.hpp"
#include "explorer/pane_model.hpp"
#include "explorer/trace.hpp"
#include "explorer/worker.hpp"
#include "ui/components/component.hpp"
#include "ui/components/dialog.hpp"
#include "ui/components/hold_button.hpp"
#include "ui/components/input_prompt.hpp"
#include "ui/components/menu.hpp"
#include "ui/components/progress.hpp"
#include "ui/components/split_view.hpp"
#include "ui/components/table.hpp"
#include "ui/components/toast.hpp"
#include "viewer/registry.hpp"
#include "viewer/viewer.hpp"

#include <cerrno>
#include <cstdio>
#include <string>
#include <utility>

namespace hui::concepts
{

namespace
{

using gfx::Color;
using gfx::Rect;
using namespace explorer;
using hui::explorer::ConflictPolicy;
using hui::explorer::CopyPlan;
using hui::explorer::CopyProgress;
using hui::explorer::Entry;
using hui::explorer::EntryKind;
using hui::explorer::JobKind;
using hui::explorer::JobShared;
using hui::explorer::JobState;
using hui::explorer::LocalSource;
using hui::explorer::PaneModel;
using hui::explorer::PaneState;
using hui::explorer::SortKey;

constexpr const char *kTechniques[] = {
    "Two panes, each a pane model (folder, sorted rows, selection, focus) over a Source",
    "Folders over 2000 entries show the first 2000, so the table stays quick",
    "The table only shows: sorting and the selection marks are the model's",
    "At a table's left or right edge the focus crosses to the other pane (SplitView)",
    "Options opens a menu at the focused row; names are asked with the on-screen keyboard",
    "Copy, move and delete run on a worker thread; progress is polled, never waited for",
    "Delete and cancel are hold-to-confirm; a name that is already there is asked about",
};

constexpr app::TourStep kTour[] = {
    {1.2f, 0, Direction::none, "files"},
    {0.4f, 0, Direction::down, nullptr},
    {0.4f, 0, Direction::down, "files-moved"},
    {0.4f, 0, Direction::right, nullptr},
    {0.8f, 0, Direction::down, "files-right"},
    // Down to the long name and wait: after a second it scrolls.
    {0.15f, 0, Direction::down, nullptr},
    {0.15f, 0, Direction::down, nullptr},
    {2.6f, 0, Direction::none, "files-longname"},
    // Open the last file (a text one) and show it scrolled a little; the demo registry has the
    // viewer.
    {0.15f, 0, Direction::down, nullptr},
    {0.15f, 0, Direction::down, nullptr},
    {0.15f, 0, Direction::down, nullptr},
    {0.15f, 0, Direction::down, nullptr},
    {0.15f, 0, Direction::down, nullptr},
    {0.15f, 0, Direction::down, nullptr},
    {0.15f, 0, Direction::down, nullptr},
    {0.15f, 0, Direction::down, nullptr},
    {0.15f, 0, Direction::down, nullptr},
    {0.3f, action_bit(Action::confirm), Direction::none, nullptr},
    {0.2f, 0, Direction::down, nullptr},
    {0.2f, 0, Direction::right, nullptr},
    {0.8f, 0, Direction::none, "files-text"},
    {0.2f, action_bit(Action::back), Direction::none, nullptr},
    {0.5f, 0, Direction::none, nullptr},
    // The other viewers, walking up the list from the text file (the last in the stick's list): a
    // DDS texture, a picture, a binary file in the hex view, a playlist and a song with its cover.
    {0.2f, 0, Direction::up, nullptr},
    {0.3f, action_bit(Action::confirm), Direction::none, nullptr},
    {3.0f, 0, Direction::none, "files-dds"},
    {0.3f, action_bit(Action::back), Direction::none, nullptr},
    {0.3f, 0, Direction::up, nullptr},
    {0.3f, action_bit(Action::confirm), Direction::none, nullptr},
    {5.0f, 0, Direction::none, "files-image"},
    {0.3f, action_bit(Action::back), Direction::none, nullptr},
    {0.2f, 0, Direction::up, nullptr},
    {0.2f, 0, Direction::up, nullptr},
    {0.2f, 0, Direction::up, nullptr},
    {0.2f, 0, Direction::up, nullptr},
    {0.3f, action_bit(Action::confirm), Direction::none, nullptr},
    {0.2f, 0, Direction::down, nullptr},
    {0.2f, 0, Direction::down, nullptr},
    {0.2f, 0, Direction::right, nullptr},
    {0.2f, 0, Direction::right, nullptr},
    {0.6f, 0, Direction::none, "files-hex"},
    {0.3f, action_bit(Action::back), Direction::none, nullptr},
    {0.2f, 0, Direction::up, nullptr},
    {0.3f, action_bit(Action::confirm), Direction::none, nullptr},
    {3.0f, 0, Direction::none, "files-playlist"},
    {0.3f, action_bit(Action::back), Direction::none, nullptr},
    {0.3f, 0, Direction::up, nullptr},
    {0.3f, action_bit(Action::confirm), Direction::none, nullptr},
    {3.0f, 0, Direction::none, "files-sound"},
    {0.3f, action_bit(Action::back), Direction::none, nullptr},
    {0.5f, 0, Direction::none, nullptr},

    {0.3f, action_bit(Action::menu), Direction::none, nullptr},
    {0.7f, 0, Direction::none, "files-menu"},
    // Down to Delete, which only asks: holding Cross is what deletes, and the tour never holds.
    {0.12f, 0, Direction::down, nullptr},
    {0.12f, 0, Direction::down, nullptr},
    {0.12f, 0, Direction::down, nullptr},
    {0.12f, 0, Direction::down, nullptr},
    {0.12f, 0, Direction::down, nullptr},
    {0.12f, 0, Direction::down, nullptr},
    {0.12f, 0, Direction::down, nullptr},
    {0.2f, action_bit(Action::confirm), Direction::none, nullptr},
    {0.8f, 0, Direction::none, "files-delete"},
    {0.2f, action_bit(Action::back), Direction::none, nullptr},
    {0.5f, 0, Direction::none, nullptr},
    // R3 opens About on top (the shell does that; a test that drives only this design ignores
    // it).
    {0.3f, action_bit(Action::r3), Direction::none, nullptr},
    {0.9f, 0, Direction::none, "files-about"},
    {0.2f, action_bit(Action::back), Direction::none, nullptr},
    {0.5f, 0, Direction::none, nullptr},
};

constexpr float kHeaderHeight = 100.0f; // a pane's path and facts, above its table
// The Files screen has no title block (it is the only screen): the panes start near the top and
// end just above the hint row.
// Where an open file is drawn: under its title, above the hint row.
Rect viewer_area()
{
    return {kMargin, kTop - 20.0f, kRight - kMargin, kBottom - kTop + 40.0f};
}

constexpr float kProgressTop = 24.0f; // the panel a running job draws over the top of the panes
constexpr float kPanesTop = 36.0f;
constexpr float kPanesBottom = 962.0f;

const hui::viewer::HandlerRegistry *g_registry = nullptr;
hui::viewer::TextureHost *g_textures = nullptr;
hui::audio::Mixer *g_mixer = nullptr;

const hui::viewer::HandlerRegistry &registry()
{
    return g_registry != nullptr ? *g_registry : hui::viewer::default_registry();
}

std::string g_start_left = "/";
std::string g_start_right = "/";

SortKey key_of_column(int column)
{
    return column == kColSize ? SortKey::size
                              : (column == kColModified ? SortKey::modified : SortKey::name);
}

const char *key_name(SortKey key)
{
    switch (key)
    {
    case SortKey::name:
        return "name";
    case SortKey::size:
        return "size";
    case SortKey::modified:
        return "date";
    case SortKey::kind:
        return "type";
    }
    return "";
}

// What an errno means to the player.
const char *describe_error(int error)
{
    switch (error)
    {
    case EEXIST:
        return "That name is already used here";
    case EINVAL:
        return "That is not a name a file can have";
    case ENAMETOOLONG:
        return "That name is too long";
    case EPERM:
        return "Changes are only allowed in /data and on drives";
    case EXDEV:
        return "Another drive is mounted in there";
    case EACCES:
        return "No permission here";
    case EROFS:
        return "This drive is read-only";
    case ENOSPC:
        return "The drive is full";
    case ENOENT:
        return "It is not there any more";
    default:
        return "The console refused";
    }
}

ui::MenuItem menu_item(const char *label, int tag, bool disabled = false)
{
    ui::MenuItem item;
    item.label = label;
    item.tag = tag;
    item.disabled = disabled;
    return item;
}

enum MenuTag
{
    kTagOpenAs = 100, // and up: one per handler that can open the file
    kTagCopy = 1,
    kTagMove,
    kTagDelete,
    kTagRename,
    kTagNewFolder,
    kTagSelectAll,
    kTagInvert,
    kTagClear,
};

enum class PromptFor
{
    none,
    rename,
    new_folder,
};

// One side of the screen: the model and the table that shows it.
class PaneView
{
  public:
    PaneView(hui::explorer::Source &source, const std::string &start, bool toward_right)
        : model(source)
    {
        const ui::Theme &look = theme();
        table.style.theme = look;
        table.style.multi_select = true; // the mark column; marking itself is done by the pane
        table.style.entrance_step = 0.012f;
        table.style.row_height = 46.0f;
        table.style.text_size = 20.0f;
        // The edge that faces the other pane hands the focus over; the screen crosses the split.
        table.style.exits.right = toward_right;
        table.style.exits.left = !toward_right;
        ui::TableColumn name;
        name.title = "Name";
        name.flex = 3.0f;
        name.strong = true;
        name.sortable = false;
        // The cell draws itself: an icon, then the name (cut, or scrolling when it has had focus).
        name.cell = [this](ui::Canvas &canvas, const Rect &cell, const ui::TableRow &row,
                           float /*focus*/) { draw_name_cell(canvas, cell, row); };
        ui::TableColumn size;
        size.title = "Size";
        size.width = 120.0f;
        size.align = gfx::Align::right;
        size.numeric = true;
        size.sortable = false;
        ui::TableColumn modified;
        modified.title = "Modified";
        modified.width = 200.0f;
        modified.align = gfx::Align::right;
        modified.sortable = false;
        table.set_columns({name, size, modified});
        model.go_to(start);
        refresh(false);
    }

    void layout(const Rect &area)
    {
        area_ = area;
        table.set_bounds({area.x, area.y + kHeaderHeight, area.w, area.h - kHeaderHeight});
    }

    const Entry *entry_of_row(int row) const
    {
        if (row < 0 || row >= static_cast<int>(table.rows().size()))
            return nullptr;
        return model.entry_at(table.rows()[static_cast<std::size_t>(row)].tag);
    }

    // The table shows the model's rows again: after a folder or a sort changed.
    void refresh(bool replay_entrance)
    {
        table.set_rows(to_rows(model.state()));
        sync_marks();
        table.set_focus(model.state().focus_index, true);
        if (replay_entrance)
            table.enter();
    }

    // What an operation acts on: the marked names, or else the focused row.
    std::vector<std::string> picked() const
    {
        if (!model.state().selected.empty())
            return model.state().selected;
        const Entry *focused = model.focused();
        return focused != nullptr ? std::vector<std::string>{focused->name}
                                  : std::vector<std::string>{};
    }

    // A mark is kept by row id, which a name in another folder shares: set every one again.
    void sync_marks()
    {
        for (int row = 0; row < static_cast<int>(table.rows().size()); ++row)
        {
            const Entry *entry = entry_of_row(row);
            table.set_selected(row, entry != nullptr && model.is_selected(entry->name));
        }
    }

    void sort_by(SortKey key, bool descending, app::Feedback &feedback)
    {
        model.set_sort(key, descending);
        refresh(false);
        feedback.play(audio::Cue::tab);
    }

    // The pad, for the pane that has the focus. Cross is not passed on to the table.
    void handle(const InputFrame &input, float dt, app::Feedback &feedback)
    {
        // L1 / R1: a page at a time, and faster the longer they are held.
        if (const int step = pager_.step(input, dt))
            turn_page(step, feedback);
        const int row = table.focus();
        const Entry *focused = entry_of_row(row);
        const PaneState &state = model.state();
        if (input.is_pressed(Action::confirm))
        {
            if (table.in_header())
            {
                const SortKey key = key_of_column(table.column_cursor());
                sort_by(key, state.sort_key == key ? !state.sort_descending : false, feedback);
            }
            else if (focused != nullptr && model.enter(focused->name))
                refresh(true);
            else if (focused != nullptr && focused->kind != EntryKind::directory)
                open_request = focused->name; // a file: the screen picks its handler
            else if (focused != nullptr && focused->kind == EntryKind::directory)
                feedback.play(audio::Cue::error); // could not be listed: the header says why
        }
        else if (input.is_pressed(Action::back))
        {
            if (model.go_up())
                refresh(true);
            else
                feedback.play(audio::Cue::error);
        }
        else if (input.is_pressed(Action::north) && focused != nullptr)
        {
            model.toggle_selected(focused->name);
            table.set_selected(row, model.is_selected(focused->name));
            feedback.play(audio::Cue::toggle);
        }
        else if (input.is_pressed(Action::jump_next))
        {
            // R2: the next sort key (name, size, date); L2: the other direction.
            const SortKey now = state.sort_key;
            sort_by(now == SortKey::name   ? SortKey::size
                    : now == SortKey::size ? SortKey::modified
                                           : SortKey::name,
                    false, feedback);
        }
        else if (input.is_pressed(Action::jump_prev))
        {
            sort_by(state.sort_key, !state.sort_descending, feedback);
        }
    }

    // The table moves the focus; the model follows it.
    ui::Event move(const InputFrame &input, app::Feedback &feedback)
    {
        InputFrame forwarded = input;
        forwarded.pressed &= ~action_bit(Action::confirm);
        const ui::Event event = table.handle(forwarded, feedback);
        const int moved = table.focus();
        if (moved >= 0)
            model.set_focus(moved);
        return event;
    }

    void draw(ui::Painter &paint, ui::Canvas &canvas) const
    {
        const PaneState &state = model.state();
        const ui::Theme &look = paint.theme();
        const float x = area_.x;
        // The path, ellipsized: where you are matters more than where you started.
        paint.label(ui::fit_label(paint, state.path, 22.0f, area_.w), x, area_.y + 22.0f, 22.0f,
                    paint.page_text());
        char line[200];
        if (state.truncated)
            std::snprintf(line, sizeof(line), "%d of %d items",
                          static_cast<int>(state.entries.size()), state.total_entries);
        else
            std::snprintf(line, sizeof(line), "%d items", state.total_entries);
        std::string facts = line;
        if (state.space.known)
            facts += "   " + size_text(state.space.free_bytes) + " free of " +
                     size_text(state.space.total_bytes);
        paint.body(ui::fit_body(paint, facts, 18.0f, area_.w), x, area_.y + 52.0f, 18.0f,
                   paint.page_text_muted());
        if (state.list_error != 0)
        {
            std::snprintf(line, sizeof(line), "Could not open that folder (error %d)",
                          state.list_error);
            paint.body(line, x, area_.y + 80.0f, 18.0f, look.danger);
        }
        else
        {
            std::snprintf(line, sizeof(line), "Sorted by %s %s", key_name(state.sort_key),
                          state.sort_descending ? "descending" : "ascending");
            std::string sorted = line;
            if (!state.selected.empty())
                sorted += "   -   " + std::to_string(state.selected.size()) + " marked";
            if (!state.changeable)
                sorted += "   -   read only here: changes are allowed in /data and on drives";
            paint.body(sorted, x, area_.y + 80.0f, 18.0f, paint.page_text_muted());
        }
        table.draw(canvas);
        if (state.entries.empty())
            paint.body(state.list_error != 0 ? "This folder cannot be opened" : "Empty folder",
                       area_.x + area_.w * 0.5f, area_.y + kHeaderHeight + 150.0f, 24.0f,
                       paint.page_text_muted(), gfx::Align::center);
    }

    // The file a Cross press asked to open (the screen reads and clears it), else empty.
    std::string open_request;

    // Once a frame: how long the focused row has stayed focused, for the scrolling name.
    void tick(float dt, bool active)
    {
        active_ = active;
        const PaneState &state = model.state();
        if (state.focus_index != last_row_ || state.path != last_path_)
        {
            last_row_ = state.focus_index;
            last_path_ = state.path;
            focus_held_ = 0.0f;
        }
        else
        {
            focus_held_ += dt;
        }
    }

    // The probe a listing makes of an entry: name and size, nothing read.
    static hui::viewer::FileProbe probe_of(const Entry &entry)
    {
        return hui::viewer::FileProbe(entry.name, entry.size);
    }

    PaneModel model;
    ui::Table table;

  private:
    void draw_name_cell(ui::Canvas &canvas, const Rect &cell, const ui::TableRow &row) const
    {
        const Entry *entry = model.entry_at(row.tag);
        if (entry == nullptr)
            return;
        const ui::Theme &look = theme();
        ui::Painter paint(canvas.list, canvas.fonts, look);
        constexpr float kIcon = 28.0f;
        constexpr float kSize = 20.0f;
        const Rect box{cell.x + 4.0f, cell.cy() - kIcon * 0.5f, kIcon, kIcon};
        const bool folder = entry->kind == EntryKind::directory;
        const hui::viewer::FileProbe probe = probe_of(*entry);
        draw_file_icon(canvas.list, box, folder ? FileKind::folder : registry().icon_for(probe),
                       folder || registry().supported(probe), look.surface);
        const float x = box.x + kIcon + 12.0f;
        const float width = cell.x + cell.w - 6.0f - x;
        const float baseline = cell.cy() + kSize * 0.34f;
        const float text_width = paint.label_width(entry->name, kSize);
        if (text_width <= width)
        {
            paint.label(entry->name, x, baseline, kSize, look.text);
            return;
        }
        // Too long: cut with "..." until the row has been focused a while, then scroll it through.
        const bool focused = active_ && row.tag == model.state().focus_index;
        const float offset = focused ? marquee_offset(text_width, width, focus_held_) : 0.0f;
        if (offset <= 0.0f)
        {
            paint.label(ui::fit_label(paint, entry->name, kSize, width), x, baseline, kSize,
                        look.text);
            return;
        }
        canvas.list.push_clip({x, cell.y, width, cell.h});
        paint.label(entry->name, x - offset, baseline, kSize, look.text);
        canvas.list.pop_clip();
    }

    bool active_ = false;
    float focus_held_ = 0.0f;
    int last_row_ = -1;
    std::string last_path_;
    // Rows that fit in the table below its header, less one so the row that was last stays in view.
    int page_rows() const
    {
        const float rows = (table.bounds().h - table.style.header_height) / table.style.row_height;
        return rows > 2.0f ? static_cast<int>(rows) - 1 : 1;
    }

    void turn_page(int direction, app::Feedback &feedback)
    {
        const int rows = static_cast<int>(table.rows().size());
        const int from = table.focus() < 0 ? 0 : table.focus();
        const int to = paged_focus(from, rows, page_rows(), direction);
        if (to == from)
            return;
        table.set_focus(to, false);
        model.set_focus(to);
        if (!pager_.repeating())
            feedback.play(audio::Cue::focus); // a held pager is quiet: the glide is enough
    }

    HeldStep pager_{Action::page_prev, Action::page_next};
    Rect area_{};
};

class ExplorerScreen final : public app::Concept
{
  public:
    explicit ExplorerScreen(app::Context &context)
        : context_(context),
          panes_{PaneView(source_, g_start_left, true), PaneView(source_, g_start_right, false)}
    {
        const ui::Theme &look = theme();
        split_.style.theme = look;
        split_.style.ratio = 0.5f;
        split_.style.gap = 40.0f;
        split_.set_bounds({kMargin, kPanesTop, kRight - kMargin, kPanesBottom - kPanesTop});
        split_.set_focus(0, true);
        for (int i = 0; i < 2; ++i)
            panes_[i].layout(split_.pane_rect(i));
        const Rect screen{0.0f, 0.0f, gfx::kVirtualWidth, gfx::kVirtualHeight};
        menu_.style.theme = look;
        menu_.set_bounds(screen);
        prompt_.style.theme = look;
        prompt_.style.max_length = 128;
        prompt_.keyboard.style.bindings = ui::KeyboardBindings::standard();
        prompt_.set_bounds(screen);
        toasts_.style.theme = look;
        toasts_.set_bounds(screen);
        conflict_.style.theme = look;
        conflict_.set_bounds(screen);
        hold_delete_.style.theme = look;
        hold_delete_.label = "Hold to delete";
        hold_delete_.hint = "Keep holding";
        hold_delete_.set_bounds(
            {gfx::kVirtualWidth * 0.5f - 440.0f + 44.0f, 320.0f + 214.0f, 380.0f, 64.0f});
        hold_delete_.set_active(true);
        cancel_hold_.style.theme = look;
        cancel_hold_.style.action = Action::back;
        cancel_hold_.style.glyph = ui::Button::circle;
        cancel_hold_.style.role = ui::ButtonRole::secondary;
        cancel_hold_.style.height = 44.0f;
        cancel_hold_.style.text_size = 20.0f;
        cancel_hold_.label = "Cancel";
        cancel_hold_.hint = "Keep holding";
        cancel_hold_.set_bounds({kRight - 300.0f, kProgressTop + 16.0f, 280.0f, 44.0f});
        cancel_hold_.set_active(true);
        bar_.style.theme = look;
        bar_.set_bounds({kMargin + 24.0f, kProgressTop + 50.0f, kRight - kMargin - 350.0f, 10.0f});
    }

    // A job still running when the app closes is told to stop, then the worker is joined.
    ~ExplorerScreen() override
    {
        hui::explorer::request_cancel(&job_.progress, &job_.mutex);
        if (viewer_)
            viewer_->close();
    }

    const app::ConceptInfo &info() const override
    {
        static const app::ConceptInfo kInfo{
            "explorer",
            "Files",
            "Browse and manage the console's drives",
            "src/concepts/explorer.cpp",
            audio::SoundSet::glass,
            Color::rgb(0x1779ba),
            kTechniques,
        };
        return kInfo;
    }

    void enter() override
    {
        age_ = 0.0f;
        menu_.dismiss();
        prompt_.dismiss();
        for (PaneView &pane : panes_)
            pane.table.enter();
    }

    void update(const InputFrame &input, float dt, app::Feedback &feedback) override
    {
        clock_ += dt;
        age_ += dt;
        if (viewer_)
        {
            // A viewer has the pad and the screen to itself until it says it is done.
            viewer_->update(input, dt, feedback);
            if (viewer_->closing())
                close_viewer();
            toasts_.update(dt, feedback);
            return;
        }
        if (job_active_)
            poll_job(feedback);
        const int active = split_.focus() == 1 ? 1 : 0;
        PaneView &pane = panes_[active];
        if (conflict_.is_open())
        {
            // Skip is the safe answer to a back press.
            const ui::Event event = conflict_.handle(input, feedback);
            if (event == ui::Event::activated)
                answer(conflict_.choice(), feedback);
            else if (event == ui::Event::cancelled)
                answer(0, feedback);
        }
        else if (job_active_)
        {
            // The panes are locked while a job runs; the one thing to do is to stop it.
            if (cancel_hold_.handle(input, feedback) == ui::Event::activated)
                hui::explorer::request_cancel(&job_.progress, &job_.mutex);
        }
        else if (confirm_delete_)
        {
            if (input.is_pressed(Action::back))
            {
                confirm_delete_ = false;
                hold_delete_.reset();
            }
            else if (hold_delete_.handle(input, feedback) == ui::Event::activated)
            {
                confirm_delete_ = false;
                hold_delete_.reset();
                start_job(JobKind::remove, delete_pane_, delete_names_, feedback);
            }
        }
        else if (prompt_.is_open())
        {
            // The prompt, then nothing else: it takes every input while it is open.
            if (prompt_.handle(input, feedback) == ui::Event::activated)
                prompt_done(active, feedback);
        }
        else if (menu_.is_open())
        {
            if (menu_.handle(input, feedback) == ui::Event::activated)
                menu_chosen(active, menu_.items()[static_cast<std::size_t>(menu_.focus())].tag,
                            feedback);
        }
        else
        {
            if (input.is_pressed(Action::menu))
                open_menu(active, feedback);
            else if (input.is_pressed(Action::west))
                start_transfer(JobKind::copy, active, feedback);
            else
                pane.handle(input, dt, feedback);
            if (!pane.open_request.empty())
            {
                const std::string name = pane.open_request;
                pane.open_request.clear();
                open_file(active, name, nullptr);
            }
            const ui::Event event = pane.move(input, feedback);
            // Past a table's edge: the focus crosses the split (or is refused where there is no
            // pane).
            if (event == ui::Event::none && pane.table.exit() != Direction::none)
                split_.handle_exit(pane.table.exit(), input, feedback);
        }
        for (int i = 0; i < 2; ++i)
        {
            panes_[i].table.set_active(i == split_.focus() && !job_active_);
            panes_[i].tick(dt, i == split_.focus() && !job_active_);
            panes_[i].table.update(dt);
        }
        split_.update(dt);
        menu_.update(dt);
        prompt_.update(dt);
        conflict_.update(dt);
        if (job_active_)
            bar_.set_value(fraction(snap_));
        bar_.update(dt);
        cancel_hold_.update(dt);
        hold_delete_.update(dt);
        toasts_.update(dt, feedback);
    }

    void draw(app::Frame &frame) const override
    {
        const ui::Theme &look = theme();
        begin_frame(frame, look);
        ui::Painter paint(frame.scene, context_.fonts, look);
        frame.scene.push_opacity(tween::cubic_out(age_ / 0.3f));
        ui::Canvas canvas{frame.scene, context_.fonts, frame.glass_texture, clock_};
        ui::Canvas top{frame.overlay, context_.fonts, frame.glass_texture, clock_};
        if (viewer_)
        {
            // The open file has the whole screen: its name on top, then its own view and hints.
            draw_header(paint, viewer_->title().c_str(), viewer_path_.c_str());
            viewer_->draw(paint, canvas, viewer_area());
            toasts_.draw(top);
            draw_hints(frame.scene, context_.fonts, look, viewer_->hints());
            frame.scene.pop_opacity();
            return;
        }
        split_.draw(canvas);
        for (int i = 0; i < 2; ++i)
        {
            split_.begin_pane(canvas, i);
            panes_[i].draw(paint, canvas);
            split_.end_pane(canvas, i);
        }
        if (job_active_)
            draw_progress(top);
        if (confirm_delete_)
            draw_delete_panel(frame, top);
        menu_.draw(top);
        prompt_.draw(top);
        conflict_.draw(top);
        toasts_.draw(top);
        static constexpr ui::Hint kBusy[] = {
            {ui::Button::circle, "Hold to cancel"},
        };
        if (job_active_)
        {
            draw_hints(frame.scene, context_.fonts, look, kBusy);
        }
        else
        {
            // What Cross does depends on what is under it: open a folder, or the handler's verb.
            const ui::Hint hints[] = {
                {ui::Button::cross, cross_label()},
                {ui::Button::circle, "Up"},
                {ui::Button::triangle, "Mark"},
                {ui::Button::square, "Copy"},
                {ui::Button::options, "Menu"},
                {ui::Button::l2, "Sort", ui::Button::r2},
                {ui::Button::l1, "Page", ui::Button::r1},
                {ui::Button::right_stick, "About"},
            };
            draw_hints(frame.scene, context_.fonts, look, hints);
        }
        frame.scene.pop_opacity();
    }

    std::span<const app::TourStep> tour() const override
    {
        return kTour;
    }

  private:
    // The label for Cross: the handler's verb for the focused file, else "Open".
    const char *cross_label() const
    {
        const PaneView &pane = panes_[split_.focus() == 1 ? 1 : 0];
        const Entry *focused = pane.model.focused();
        if (focused == nullptr || focused->kind == EntryKind::directory)
            return "Open";
        const hui::viewer::FileHandler *handler = registry().best(PaneView::probe_of(*focused));
        return handler != nullptr ? handler->verb() : "Open";
    }

    static const char *verb(JobKind kind, bool past)
    {
        switch (kind)
        {
        case JobKind::copy:
            return past ? "Copied" : "Copying";
        case JobKind::move:
            return past ? "Moved" : "Moving";
        case JobKind::remove:
            return past ? "Deleted" : "Deleting";
        }
        return "";
    }

    // 0..1: by bytes when there are any, else by the files counted so far.
    static float fraction(const CopyProgress &progress)
    {
        float value = 0.0f;
        if (progress.bytes_total > 0)
            value = static_cast<float>(static_cast<double>(progress.bytes_done) /
                                       static_cast<double>(progress.bytes_total));
        else if (progress.files_total > 0)
            value =
                static_cast<float>(progress.files_done) / static_cast<float>(progress.files_total);
        return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
    }

    // A panel over the top of the panes (there is no room above them any more): what is running,
    // how far, and the hold that stops it.
    void draw_progress(ui::Canvas &top) const
    {
        const ui::Theme &look = theme();
        ui::Painter paint(top.list, context_.fonts, look);
        const Rect panel{kMargin, kProgressTop, kRight - kMargin, 76.0f};
        top.list.shadow(panel, look.radius_card, 24.0f, Color::rgb(0x000000, 0.25f));
        paint.surface(panel, look.radius_card, look.surface, look.outline, 1.0f);
        char line[200];
        std::snprintf(line, sizeof(line), "%s %d of %d files   %s of %s", verb(snap_.kind, false),
                      snap_.files_done, snap_.files_total, size_text(snap_.bytes_done).c_str(),
                      size_text(snap_.bytes_total).c_str());
        const float width = kRight - kMargin - 350.0f;
        paint.body(ui::fit_body(paint, line, 20.0f, width), kMargin + 24.0f, kProgressTop + 34.0f,
                   20.0f, look.text);
        bar_.draw(top);
        cancel_hold_.draw(top);
    }

    void draw_delete_panel(const app::Frame &frame, ui::Canvas &top) const
    {
        (void)frame;
        const ui::Theme &look = theme();
        ui::Painter over(top.list, context_.fonts, look);
        over.fill({0.0f, 0.0f, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0.0f,
                  Color::rgb(0x000000, 0.45f));
        const Rect panel{gfx::kVirtualWidth * 0.5f - 440.0f, 320.0f, 880.0f, 330.0f};
        over.surface(panel, look.radius_card, look.surface, look.outline, 1.0f);
        char title[80];
        std::snprintf(title, sizeof(title),
                      delete_names_.size() == 1 ? "Delete this item?" : "Delete %zu items?",
                      delete_names_.size());
        over.heading(title, panel.x + 44.0f, panel.y + 78.0f, 40.0f, look.text);
        over.body(ui::fit_body(over, delete_summary_, 22.0f, panel.w - 88.0f), panel.x + 44.0f,
                  panel.y + 128.0f, 22.0f, look.text_muted);
        over.body("This cannot be undone. Folders go with everything in them.", panel.x + 44.0f,
                  panel.y + 166.0f, 20.0f, look.text_muted);
        hold_delete_.draw(top);
        over.body("Circle: keep it", panel.x + panel.w - 44.0f, panel.y + panel.h - 36.0f, 20.0f,
                  look.text_muted, gfx::Align::right);
    }

    void open_menu(int active, app::Feedback &feedback)
    {
        PaneView &pane = panes_[active];
        const int row = pane.table.focus();
        const bool has_row = pane.entry_of_row(row) != nullptr;
        ui::MenuItem line;
        line.separator = true;
        ui::MenuItem remove = menu_item("Delete", kTagDelete, !has_row);
        remove.danger = true;
        std::vector<ui::MenuItem> items;
        // A file can be opened by every handler that knows it: the first is what Cross does.
        open_as_.clear();
        if (const Entry *entry = pane.entry_of_row(row);
            entry != nullptr && entry->kind != EntryKind::directory)
        {
            const std::string path = PaneModel::join(pane.model.state().path, entry->name);
            for (const hui::viewer::FileHandler *handler :
                 registry().all(probe_with_head(*entry, path)))
            {
                ui::MenuItem item;
                item.label = std::string("Open as ") + handler->label();
                item.tag = kTagOpenAs + static_cast<int>(open_as_.size());
                items.push_back(item);
                open_as_.push_back(handler);
            }
            if (!open_as_.empty())
                items.push_back(line);
        }
        for (const ui::MenuItem &item :
             {menu_item("Copy to other pane", kTagCopy, !has_row),
              menu_item("Move to other pane", kTagMove, !has_row), line,
              menu_item("Rename", kTagRename, !has_row), menu_item("New folder", kTagNewFolder),
              line, menu_item("Select all", kTagSelectAll),
              menu_item("Invert selection", kTagInvert),
              menu_item("Clear selection", kTagClear, pane.model.state().selected.empty()), line,
              remove})
            items.push_back(item);
        menu_.set_items(std::move(items));
        const Rect anchor = has_row ? pane.table.row_rect(row) : pane.table.bounds();
        menu_.open(anchor, feedback);
    }

    void menu_chosen(int active, int tag, app::Feedback &feedback)
    {
        PaneView &pane = panes_[active];
        switch (tag)
        {
        case kTagCopy:
            start_transfer(JobKind::copy, active, feedback);
            break;
        case kTagMove:
            start_transfer(JobKind::move, active, feedback);
            break;
        case kTagDelete:
            begin_delete(active);
            break;
        case kTagRename:
            if (const Entry *entry = pane.entry_of_row(pane.table.focus()))
            {
                rename_target_ = entry->name;
                prompt_for_ = PromptFor::rename;
                prompt_.set_title("Rename");
                prompt_.open(feedback, rename_target_);
            }
            break;
        case kTagNewFolder:
            prompt_for_ = PromptFor::new_folder;
            prompt_.set_title("New folder");
            prompt_.open(feedback, "");
            break;
        case kTagSelectAll:
            pane.model.select_all();
            pane.sync_marks();
            break;
        case kTagInvert:
            pane.model.invert_selection();
            pane.sync_marks();
            break;
        case kTagClear:
            pane.model.clear_selection();
            pane.sync_marks();
            break;
        default:
            if (tag >= kTagOpenAs && tag - kTagOpenAs < static_cast<int>(open_as_.size()))
            {
                if (const Entry *entry = pane.entry_of_row(pane.table.focus()))
                    open_file(active, entry->name,
                              open_as_[static_cast<std::size_t>(tag - kTagOpenAs)]);
            }
            break;
        }
    }

    // ---- viewers ---------------------------------------------------------------------------

    // A probe that can read the first bytes of the file, for sniffing the content.
    hui::viewer::FileProbe probe_with_head(const Entry &entry, const std::string &path) const
    {
        return hui::viewer::FileProbe(
            entry.name, entry.size,
            [this, path]
            {
                char buffer[hui::viewer::FileProbe::kHeadBytes];
                const std::int64_t n = source_.read_range(path, 0, buffer, sizeof(buffer));
                return n > 0 ? std::string(buffer, static_cast<std::size_t>(n)) : std::string();
            });
    }

    // Opens a file with `handler`, or with the one that knows it best when none is given.
    void open_file(int active, const std::string &name, const hui::viewer::FileHandler *handler)
    {
        PaneView &pane = panes_[active];
        const Entry *entry = nullptr;
        for (const Entry &candidate : pane.model.state().entries)
        {
            if (candidate.name == name)
                entry = &candidate;
        }
        if (entry == nullptr)
            return;
        const std::string path = PaneModel::join(pane.model.state().path, name);
        if (handler == nullptr)
            handler = registry().best(probe_with_head(*entry, path));
        if (handler == nullptr)
        {
            toasts_.push(ui::StatusKind::info, "Nothing can open this yet", name);
            return;
        }
        hui::viewer::ViewerContext context;
        context.area = viewer_area();
        context.fonts = &context_.fonts;
        context.worker = &worker_;
        context.textures = g_textures;
        context.mixer = g_mixer;
        hui::viewer::FileRef file;
        file.source = &source_;
        file.path = path;
        file.entry = *entry;
        viewer_ = handler->open(context, file);
        if (!viewer_)
        {
            toasts_.push(ui::StatusKind::danger, "Could not open it", name);
            return;
        }
        viewer_path_ = path;
        hui::explorer::trace("viewer: %s opens %s", handler->id(), path.c_str());
    }

    void close_viewer()
    {
        if (!viewer_)
            return;
        hui::explorer::trace("viewer: closed %s", viewer_path_.c_str());
        viewer_->close();
        viewer_.reset();
        for (PaneView &pane : panes_)
            pane.table.enter(); // the lists slide back in
    }

    // The prompt was answered: rename or make the folder, and say what happened.
    void prompt_done(int active, app::Feedback &feedback)
    {
        (void)feedback;
        PaneView &pane = panes_[active];
        const std::string text = prompt_.text();
        const PromptFor what = prompt_for_;
        prompt_for_ = PromptFor::none;
        if (what == PromptFor::none)
            return;
        const int error = what == PromptFor::rename ? pane.model.rename_entry(rename_target_, text)
                                                    : pane.model.make_folder(text);
        if (error != 0)
        {
            toasts_.push(ui::StatusKind::danger,
                         what == PromptFor::rename ? "Could not rename"
                                                   : "Could not make the folder",
                         describe_error(error));
            return;
        }
        pane.refresh(false);
        reload_other(active);
        toasts_.push(ui::StatusKind::success, what == PromptFor::rename ? "Renamed" : "Folder made",
                     text);
    }

    // The other pane may show the folder that just changed.
    void reload_other(int active)
    {
        PaneView &other = panes_[1 - active];
        if (other.model.state().path == panes_[active].model.state().path)
        {
            other.model.reload();
            other.refresh(false);
        }
    }

    // ---- jobs ------------------------------------------------------------------------------

    void start_transfer(JobKind kind, int active, app::Feedback &feedback)
    {
        PaneView &from = panes_[active];
        PaneView &to = panes_[1 - active];
        const std::vector<std::string> names = from.picked();
        if (names.empty())
        {
            feedback.play(audio::Cue::error);
            return;
        }
        if ((kind != JobKind::copy && !from.model.state().changeable) ||
            !to.model.state().changeable)
        {
            toasts_.push(ui::StatusKind::warning, "Changes are not allowed there",
                         "Only /data and the drives (usb, ext) can be changed. Anywhere else "
                         "can be read and copied from.");
            return;
        }
        if (kind == JobKind::move && from.model.state().path == to.model.state().path)
        {
            toasts_.push(ui::StatusKind::info, "Both panes are in the same folder",
                         "Open another folder in the other pane first");
            return;
        }
        start_job(kind, active, names, feedback);
    }

    void begin_delete(int active)
    {
        const std::vector<std::string> names = panes_[active].picked();
        if (names.empty())
            return;
        if (!panes_[active].model.state().changeable)
        {
            toasts_.push(ui::StatusKind::warning, "Nothing can be deleted here",
                         "Only /data and the drives (usb, ext) can be changed");
            return;
        }
        delete_names_ = names;
        delete_pane_ = active;
        delete_summary_ = names[0];
        if (names.size() == 2)
            delete_summary_ += " and " + names[1];
        else if (names.size() > 2)
            delete_summary_ +=
                ", " + names[1] + " and " + std::to_string(names.size() - 2) + " more";
        hold_delete_.reset();
        confirm_delete_ = true;
    }

    void start_job(JobKind kind, int active, std::vector<std::string> names,
                   app::Feedback &feedback)
    {
        (void)feedback;
        CopyPlan plan;
        plan.kind = kind;
        plan.names = std::move(names);
        plan.from_dir = panes_[active].model.state().path;
        plan.to_dir = panes_[1 - active].model.state().path;
        plan.policy = ConflictPolicy::ask;
        {
            std::lock_guard<std::mutex> lock(job_.mutex);
            job_.progress = CopyProgress{};
            job_.progress.kind = kind;
        }
        snap_ = CopyProgress{};
        snap_.kind = kind;
        job_source_ = active;
        job_active_ = true;
        conflict_answered_ = false;
        bar_.set_value(0.0f, true);
        cancel_hold_.reset();
        worker_.queue(
            [this, plan]
            { hui::explorer::run_copy_job(source_, source_, plan, &job_.progress, &job_.mutex); });
    }

    // Once a frame while a job runs: take its progress, ask about a conflict, notice the end.
    void poll_job(app::Feedback &feedback)
    {
        {
            std::lock_guard<std::mutex> lock(job_.mutex);
            snap_ = job_.progress;
        }
        if (snap_.state == JobState::waiting_conflict)
        {
            if (!conflict_.is_open() && !conflict_answered_)
            {
                ui::DialogContent content;
                content.icon = ui::StatusKind::question;
                content.title = "Already there";
                content.body = snap_.conflict_name + "\nis in the other folder already.";
                ui::DialogButton skip;
                skip.label = "Skip";
                ui::DialogButton overwrite;
                overwrite.label = "Overwrite";
                overwrite.destructive = true;
                ui::DialogButton both;
                both.label = "Keep both";
                content.buttons = {skip, overwrite, both};
                content.default_button = 0;
                conflict_.open(content, feedback);
            }
        }
        else
        {
            conflict_answered_ = false;
        }
        if (snap_.state == JobState::done || snap_.state == JobState::failed ||
            snap_.state == JobState::cancelled)
            finish_job();
    }

    void answer(int choice, app::Feedback &feedback)
    {
        (void)feedback;
        const ConflictPolicy answers[] = {ConflictPolicy::skip, ConflictPolicy::overwrite,
                                          ConflictPolicy::rename};
        hui::explorer::answer_conflict(&job_.progress, &job_.mutex,
                                       answers[choice < 0 || choice > 2 ? 0 : choice]);
        conflict_answered_ = true;
    }

    void finish_job()
    {
        job_active_ = false;
        conflict_.dismiss();
        const CopyProgress &result = snap_;
        char text[160];
        if (result.state == JobState::failed)
        {
            std::snprintf(text, sizeof(text), "%s (error %d)", describe_error(result.last_error),
                          result.last_error);
            toasts_.push(ui::StatusKind::danger, "Stopped", text);
        }
        else if (result.state == JobState::cancelled)
        {
            std::snprintf(text, sizeof(text), "%d file(s) were done before it stopped",
                          result.files_done);
            toasts_.push(ui::StatusKind::info, "Cancelled", text);
        }
        else
        {
            std::snprintf(text, sizeof(text), "%d file(s)", result.files_done);
            std::string body = text;
            if (result.skipped > 0)
                body += ", " + std::to_string(result.skipped) + " skipped";
            if (result.failed > 0)
                body += ", " + std::to_string(result.failed) +
                        " failed: " + describe_error(result.last_error);
            toasts_.push(result.failed > 0 ? ui::StatusKind::warning : ui::StatusKind::success,
                         verb(result.kind, true), body);
        }
        // What was done changed both folders; the marks were of the names just handled.
        panes_[job_source_].model.clear_selection();
        for (PaneView &pane : panes_)
        {
            pane.model.reload();
            pane.refresh(false);
        }
    }

    app::Context &context_;
    float age_ = 0.0f;
    float clock_ = 0.0f;
    LocalSource source_;
    PaneView panes_[2];
    ui::SplitView split_;
    ui::Menu menu_;
    ui::InputPrompt prompt_;
    ui::ToastStack toasts_;
    std::unique_ptr<hui::viewer::Viewer> viewer_; // the open file, when there is one
    std::string viewer_path_;
    std::vector<const hui::viewer::FileHandler *> open_as_; // what the open menu lists
    ui::Dialog conflict_;
    ui::HoldButton hold_delete_;
    ui::HoldButton cancel_hold_;
    ui::ProgressBar bar_;
    PromptFor prompt_for_ = PromptFor::none;
    std::string rename_target_;
    bool confirm_delete_ = false;
    int delete_pane_ = 0;
    std::vector<std::string> delete_names_;
    std::string delete_summary_;
    bool job_active_ = false;
    int job_source_ = 0;
    bool conflict_answered_ = false;
    CopyProgress snap_; // the job's progress as of this frame: only the UI thread touches it
    JobShared job_;
    hui::explorer::JobWorker worker_; // last: joined first, before what its jobs use
};

} // namespace

void set_explorer_services(hui::viewer::TextureHost *textures, hui::audio::Mixer *mixer)
{
    g_textures = textures;
    g_mixer = mixer;
}

void set_explorer_registry(const hui::viewer::HandlerRegistry *registry)
{
    g_registry = registry;
}

void set_explorer_start(std::string left, std::string right)
{
    g_start_left = std::move(left);
    g_start_right = std::move(right);
}

std::unique_ptr<app::Concept> make_explorer(app::Context &context)
{
    return std::make_unique<ExplorerScreen>(context);
}

} // namespace hui::concepts
