// EliVier Explorer - Screen "Files": the file explorer, two panes side by side.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Each pane is a hui::explorer::PaneModel (folder, sorted rows, selection, focus) shown in a
// ui::Table; a ui::SplitView holds the two and moves the focus across. The table only shows:
// Cross is taken from its input (it would mark the row) and sorting is the model's, so folders
// stay on top and a capped folder sorts the whole folder before it is cut. At a table's left or
// right edge the focus crosses to the other pane.

#include "concepts/concepts.hpp"

#include "concepts/explorer/look.hpp"
#include "concepts/explorer/row_format.hpp"
#include "core/tween.hpp"
#include "explorer/local_source.hpp"
#include "explorer/pane_model.hpp"
#include "explorer/trace.hpp"
#include "ui/components/component.hpp"
#include "ui/components/input_prompt.hpp"
#include "ui/components/menu.hpp"
#include "ui/components/split_view.hpp"
#include "ui/components/table.hpp"
#include "ui/components/toast.hpp"

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
using hui::explorer::Entry;
using hui::explorer::EntryKind;
using hui::explorer::LocalSource;
using hui::explorer::PaneModel;
using hui::explorer::PaneState;
using hui::explorer::SortKey;

constexpr const char *kTechniques[] = {
    "Two panes, each a pane model (folder, sorted rows, selection, focus) over a Source",
    "Folders over 2000 entries show the first 2000, so the table stays quick",
    "The table only shows: sorting and the selection marks are the model's",
    "At a table's left or right edge the focus crosses to the other pane (SplitView)",
    "Options opens a menu at the focused row; rename and new folder ask with the on-screen keyboard",
};

constexpr app::TourStep kTour[] = {
    {1.2f, 0, Direction::none, "files"},
    {0.4f, 0, Direction::down, nullptr},
    {0.4f, 0, Direction::down, "files-moved"},
    {0.4f, 0, Direction::right, nullptr},
    {0.8f, 0, Direction::down, "files-right"},
    {0.3f, action_bit(Action::menu), Direction::none, nullptr},
    {0.7f, 0, Direction::none, "files-menu"},
    {0.2f, action_bit(Action::back), Direction::none, nullptr},
    {0.5f, 0, Direction::none, nullptr},
};

constexpr float kHeaderHeight = 100.0f; // a pane's path and facts, above its table

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
    case EACCES:
    case EPERM:
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
    kTagRename = 1,
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
    void handle(const InputFrame &input, app::Feedback &feedback)
    {
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
            std::snprintf(line, sizeof(line), "%d of %d items", static_cast<int>(state.entries.size()),
                          state.total_entries);
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
            paint.body(sorted, x, area_.y + 80.0f, 18.0f, paint.page_text_muted());
        }
        table.draw(canvas);
        if (state.entries.empty())
            paint.body(state.list_error != 0 ? "This folder cannot be opened" : "Empty folder",
                       area_.x + area_.w * 0.5f, area_.y + kHeaderHeight + 150.0f, 24.0f,
                       paint.page_text_muted(), gfx::Align::center);
    }

    PaneModel model;
    ui::Table table;

  private:
    Rect area_{};
};

class ExplorerScreen final : public app::Concept
{
  public:
    explicit ExplorerScreen(app::Context &context)
        : context_(context), panes_{PaneView(source_, g_start_left, true),
                                    PaneView(source_, g_start_right, false)}
    {
        split_.style.theme = theme();
        split_.style.ratio = 0.5f;
        split_.style.gap = 40.0f;
        split_.set_bounds({kMargin, kTop, kRight - kMargin, kBottom - kTop});
        split_.set_focus(0, true);
        for (int i = 0; i < 2; ++i)
            panes_[i].layout(split_.pane_rect(i));
        const Rect screen{0.0f, 0.0f, gfx::kVirtualWidth, gfx::kVirtualHeight};
        menu_.style.theme = theme();
        menu_.set_bounds(screen);
        prompt_.style.theme = theme();
        prompt_.style.max_length = 128;
        prompt_.keyboard.style.bindings = ui::KeyboardBindings::standard();
        prompt_.set_bounds(screen);
        toasts_.style.theme = theme();
        toasts_.set_bounds(screen);
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
        const int active = split_.focus() == 1 ? 1 : 0;
        PaneView &pane = panes_[active];
        if (prompt_.is_open())
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
            else
                pane.handle(input, feedback);
            const ui::Event event = pane.move(input, feedback);
            // Past a table's edge: the focus crosses the split (or is refused where there is no pane).
            if (event == ui::Event::none && pane.table.exit() != Direction::none)
                split_.handle_exit(pane.table.exit(), input, feedback);
        }
        for (int i = 0; i < 2; ++i)
        {
            panes_[i].table.set_active(i == split_.focus());
            panes_[i].table.update(dt);
        }
        split_.update(dt);
        menu_.update(dt);
        prompt_.update(dt);
        toasts_.update(dt, feedback);
    }

    void draw(app::Frame &frame) const override
    {
        const ui::Theme &look = theme();
        begin_frame(frame, look);
        ui::Painter paint(frame.scene, context_.fonts, look);
        frame.scene.push_opacity(tween::cubic_out(age_ / 0.3f));
        draw_header(paint, "Files", "Browse and manage the console's drives");
        ui::Canvas canvas{frame.scene, context_.fonts, frame.glass_texture, clock_};
        split_.draw(canvas);
        for (int i = 0; i < 2; ++i)
        {
            split_.begin_pane(canvas, i);
            panes_[i].draw(paint, canvas);
            split_.end_pane(canvas, i);
        }
        ui::Canvas top{frame.overlay, context_.fonts, frame.glass_texture, clock_};
        menu_.draw(top);
        prompt_.draw(top);
        toasts_.draw(top);
        static constexpr ui::Hint kHints[] = {
            {ui::Button::cross, "Open"},
            {ui::Button::circle, "Up"},
            {ui::Button::triangle, "Mark"},
            {ui::Button::options, "Menu"},
            {ui::Button::l2, "Sort", ui::Button::r2},
        };
        draw_hints(frame.scene, context_.fonts, look, kHints);
        frame.scene.pop_opacity();
    }

    std::span<const app::TourStep> tour() const override
    {
        return kTour;
    }

  private:
    void open_menu(int active, app::Feedback &feedback)
    {
        PaneView &pane = panes_[active];
        const int row = pane.table.focus();
        const bool has_row = pane.entry_of_row(row) != nullptr;
        ui::MenuItem line;
        line.separator = true;
        menu_.set_items({menu_item("Rename", kTagRename, !has_row),
                         menu_item("New folder", kTagNewFolder), line,
                         menu_item("Select all", kTagSelectAll),
                         menu_item("Invert selection", kTagInvert),
                         menu_item("Clear selection", kTagClear,
                                   pane.model.state().selected.empty())});
        const Rect anchor = has_row ? pane.table.row_rect(row) : pane.table.bounds();
        menu_.open(anchor, feedback);
    }

    void menu_chosen(int active, int tag, app::Feedback &feedback)
    {
        PaneView &pane = panes_[active];
        switch (tag)
        {
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
            break;
        }
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
                         what == PromptFor::rename ? "Could not rename" : "Could not make the folder",
                         describe_error(error));
            return;
        }
        pane.refresh(false);
        // The other pane may show the same folder.
        PaneView &other = panes_[1 - active];
        if (other.model.state().path == pane.model.state().path)
        {
            other.model.reload();
            other.refresh(false);
        }
        toasts_.push(ui::StatusKind::success, what == PromptFor::rename ? "Renamed" : "Folder made",
                     text);
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
    PromptFor prompt_for_ = PromptFor::none;
    std::string rename_target_;
};

} // namespace

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
