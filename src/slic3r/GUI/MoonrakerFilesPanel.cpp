#include "MoonrakerFilesPanel.hpp"

#include <wx/arrstr.h>
#include <wx/bitmap.h>
#include <wx/colour.h>
#include <wx/datetime.h>
#include <wx/log.h>
#include <wx/mstream.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>

#include <boost/filesystem/path.hpp>

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "DeviceCore/DevManager.h"
#include "DeviceManager.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "format.hpp"
#include "Jobs/BoostThreadWorker.hpp"
#include "Jobs/PlaterWorker.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"

namespace Slic3r { namespace GUI {

static std::shared_ptr<MoonrakerPrinterAgent> moonraker_agent()
{
    NetworkAgent* agent = wxGetApp().getAgent();
    return agent ? std::dynamic_pointer_cast<MoonrakerPrinterAgent>(agent->get_printer_agent()) : nullptr;
}

static wxString file_name(const std::string& path) { return from_u8(boost::filesystem::path(path).filename().string()); }

// A thumbnail stays valid while the file it belongs to is unchanged.
static std::string thumbnail_key(const MoonrakerPrinterAgent::PrinterFile& file)
{
    return file.path + '\n' + std::to_string(static_cast<long long>(file.modified));
}

bool MoonrakerFilesPanel::is_supported() { return moonraker_agent() != nullptr; }

MoonrakerFilesPanel::MoonrakerFilesPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
    , m_worker(std::make_unique<PlaterWorker<BoostThreadWorker>>(this, nullptr, "moonraker_files_worker"))
{
    m_title   = new Label(this, Label::Head_16, _L("Files on the printer"));
    m_refresh = new Button(this, _L("Refresh"));
    m_refresh->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    m_refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { refresh(); });

    auto header = new wxBoxSizer(wxHORIZONTAL);
    header->Add(m_title, 1, wxALIGN_CENTER_VERTICAL);
    header->Add(m_refresh, 0, wxALIGN_CENTER_VERTICAL);

    m_message = new Label(this, Label::Body_14);
    m_message->Hide();

    m_list = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_list->SetScrollRate(0, FromDIP(20));
    m_list_sizer = new wxBoxSizer(wxVERTICAL);
    m_list->SetSizer(m_list_sizer);

    auto sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(header, 0, wxEXPAND | wxALL, FromDIP(16));
    sizer->Add(m_message, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    sizer->Add(m_list, 1, wxEXPAND);
    SetSizer(sizer);

    apply_colors();
}

MoonrakerFilesPanel::~MoonrakerFilesPanel()
{
    // The jobs report back to this panel, so none may outlive it.
    m_worker->cancel_all();
    m_worker.reset();
}

void MoonrakerFilesPanel::UpdateByObj(MachineObject* obj)
{
    const std::string dev_id = obj ? obj->get_dev_id() : std::string();
    if (dev_id == m_dev_id)
        return;

    m_dev_id = dev_id;
    m_files.clear();
    m_thumbnails.clear();
    rebuild_rows();
    if (!m_dev_id.empty())
        refresh();
}

void MoonrakerFilesPanel::refresh()
{
    auto agent = moonraker_agent();
    if (!agent) {
        show_message(_L("This printer does not share its files."));
        return;
    }

    show_message(_L("Loading files…"));
    std::set<std::string> cached;
    for (const auto& [key, image] : m_thumbnails)
        cached.insert(key);

    // A newer listing replaces this one: its results are dropped when they arrive late.
    const unsigned generation = ++m_generation;
    replace_job(*m_worker, [this, agent, generation, cached = std::move(cached)](Job::Ctl& ctl) {
        const auto  cancelled = [&ctl] { return ctl.was_canceled(); };
        std::vector<PrinterFile> files;
        std::string error;
        const bool  ok = agent->list_files(files, error, cancelled);
        if (ctl.was_canceled())
            return;
        ctl.call_on_main_thread([this, generation, files, error = ok ? std::string() : error]() mutable {
            if (generation == m_generation)
                show_files(std::move(files), error);
        });
        if (!ok)
            return;

        for (PrinterFile& file : files) {
            if (ctl.was_canceled() || !agent->load_file_metadata(file, cancelled))
                return;
            std::string data, thumbnail_error;
            if (!file.thumbnail.empty() && cached.count(thumbnail_key(file)) == 0)
                agent->download_file(file.thumbnail, data, thumbnail_error);
            ctl.call_on_main_thread([this, generation, file, data = std::move(data)] {
                if (generation == m_generation)
                    update_file(file, data);
            });
        }
    });
}

void MoonrakerFilesPanel::show_files(std::vector<PrinterFile> files, const std::string& error)
{
    m_files = std::move(files);
    rebuild_rows();
    if (!error.empty())
        // TRN %1% is the reason the printer gave, in English.
        show_message(format_wxstr(_L("Could not list the printer's files: %1%"), error));
    else if (m_files.empty())
        show_message(_L("There are no G-code files on the printer."));
    else
        show_message(wxEmptyString);
}

void MoonrakerFilesPanel::show_message(const wxString& message)
{
    m_message->SetLabel(message);
    m_message->Show(!message.empty());
    Layout();
}

void MoonrakerFilesPanel::rebuild_rows()
{
    m_list->Freeze();
    m_list_sizer->Clear(true);
    m_rows.clear();
    for (const PrinterFile& file : m_files)
        add_row(file);
    m_list->FitInside();
    m_list->Layout();
    m_list->Thaw();
}

void MoonrakerFilesPanel::add_row(const PrinterFile& file)
{
    auto row = new wxPanel(m_list);
    row->SetBackgroundColour(m_list->GetBackgroundColour());

    auto thumbnail = new wxStaticBitmap(row, wxID_ANY, thumbnail_bitmap(file));
    auto name      = new Label(row, Label::Head_14, file_name(file.path), wxST_ELLIPSIZE_MIDDLE);
    name->SetToolTip(from_u8(file.path));
    auto details = new Label(row, Label::Body_12, details_text(file), wxST_ELLIPSIZE_END);
    details->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#6B6B6B")));

    auto print = new Button(row, _L("Print"));
    print->SetStyle(ButtonStyle::Confirm, ButtonType::Window);
    print->Bind(wxEVT_BUTTON, [this, path = file.path](wxCommandEvent&) { print_file(path); });
    auto remove = new Button(row, _L("Delete"));
    remove->SetStyle(ButtonStyle::Alert, ButtonType::Window);
    remove->Bind(wxEVT_BUTTON, [this, path = file.path](wxCommandEvent&) { delete_file(path); });

    auto text = new wxBoxSizer(wxVERTICAL);
    text->Add(name, 0, wxEXPAND);
    text->AddSpacer(FromDIP(4));
    text->Add(details, 0, wxEXPAND);

    auto sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add(thumbnail, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(8));
    sizer->Add(text, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(8));
    sizer->Add(print, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    sizer->Add(remove, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    row->SetSizer(sizer);

    auto separator = new wxPanel(m_list, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(1)));
    separator->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#EEEEEE")));

    m_list_sizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));
    m_list_sizer->Add(separator, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
    m_rows[file.path] = {details, thumbnail};
}

void MoonrakerFilesPanel::update_file(const PrinterFile& file, const std::string& thumbnail_data)
{
    auto it = std::find_if(m_files.begin(), m_files.end(), [&file](const PrinterFile& f) { return f.path == file.path; });
    if (it == m_files.end())
        return;
    *it = file;

    if (!thumbnail_data.empty()) {
        wxLogNull            no_decoder_errors;
        wxMemoryInputStream  stream(thumbnail_data.data(), thumbnail_data.size());
        wxImage              image;
        if (image.LoadFile(stream, wxBITMAP_TYPE_ANY) && image.IsOk())
            m_thumbnails[thumbnail_key(file)] = std::move(image);
    }

    if (auto row = m_rows.find(file.path); row != m_rows.end()) {
        row->second.details->SetLabel(details_text(file));
        row->second.thumbnail->SetBitmap(thumbnail_bitmap(file));
    }
}

wxString MoonrakerFilesPanel::details_text(const PrinterFile& file) const
{
    wxArrayString parts;
    if (file.modified > 0)
        parts.Add(wxDateTime(static_cast<time_t>(file.modified)).Format("%Y-%m-%d %H:%M"));
    if (file.estimated_time > 0)
        parts.Add(from_u8(short_time(get_time_dhms(float(file.estimated_time)))));

    // Moonraker's job states: https://moonraker.readthedocs.io/en/latest/web_api/#print-history
    const std::string& status = file.last_status;
    if (status == "completed")
        parts.Add(_L("Last print finished"));
    else if (status == "cancelled")
        parts.Add(_L("Last print cancelled"));
    else if (status == "in_progress")
        parts.Add(_L("Printing"));
    else if (!status.empty())
        parts.Add(_L("Last print failed"));

    wxString text;
    for (const wxString& part : parts) {
        if (!text.empty())
            text += wxString::FromUTF8(" \xC2\xB7 ");
        text += part;
    }
    return text;
}

wxBitmap MoonrakerFilesPanel::thumbnail_bitmap(const PrinterFile& file) const
{
    const int size = FromDIP(64);
    auto      it   = m_thumbnails.find(thumbnail_key(file));
    if (it == m_thumbnails.end()) {
        // A plain square keeps every row the same height until the picture arrives.
        const wxColour fill = StateColor::darkModeColorFor(wxColour("#EEEEEE"));
        wxImage        placeholder(size, size);
        placeholder.SetRGB(wxRect(0, 0, size, size), fill.Red(), fill.Green(), fill.Blue());
        return wxBitmap(placeholder);
    }

    wxImage    image = it->second;
    const double scale = std::min(double(size) / image.GetWidth(), double(size) / image.GetHeight());
    const int    width = std::max(1, int(image.GetWidth() * scale)), height = std::max(1, int(image.GetHeight() * scale));
    image.Rescale(width, height, wxIMAGE_QUALITY_HIGH);
    image.Resize(wxSize(size, size), wxPoint((size - width) / 2, (size - height) / 2));
    return wxBitmap(image);
}

void MoonrakerFilesPanel::print_file(const std::string& path)
{
    DeviceManager* dev = wxGetApp().getDeviceManager();
    MachineObject* obj = dev ? dev->get_selected_machine() : nullptr;
    if (obj && obj->is_in_printing()) {
        MessageDialog(this, _L("The printer is busy with another print. Try again when it has finished."), _L("Print"),
                      wxOK | wxICON_WARNING)
            .ShowModal();
        return;
    }
    // TRN %1% is a file name.
    MessageDialog confirm(this, format_wxstr(_L("Start printing \"%1%\"?"), file_name(path)), _L("Print"), wxYES_NO | wxICON_QUESTION);
    if (confirm.ShowModal() != wxID_YES)
        return;

    auto agent = moonraker_agent();
    if (!agent)
        return;
    // Stops the thumbnail download, which would hold the request up; the listing after it resumes the rest.
    auto error = std::make_shared<std::string>();
    replace_job(
        *m_worker, [agent, path, error](Job::Ctl&) { agent->print_file(path, *error); },
        [this, error](bool, std::exception_ptr&) {
            if (!error->empty())
                // TRN %1% is the reason the printer gave, in English.
                CallAfter([this, message = format_wxstr(_L("Could not start the print: %1%"), *error)] {
                    MessageDialog(this, message, _L("Print"), wxOK | wxICON_ERROR).ShowModal();
                });
            refresh();
        });
}

void MoonrakerFilesPanel::delete_file(const std::string& path)
{
    // TRN %1% is a file name.
    MessageDialog confirm(this, format_wxstr(_L("Delete \"%1%\" from the printer?"), file_name(path)), _L("Delete"),
                          wxYES_NO | wxICON_WARNING);
    if (confirm.ShowModal() != wxID_YES)
        return;

    auto agent = moonraker_agent();
    if (!agent)
        return;
    auto error = std::make_shared<std::string>();
    replace_job(
        *m_worker, [agent, path, error](Job::Ctl&) { agent->delete_file(path, *error); },
        [this, error](bool, std::exception_ptr&) {
            if (!error->empty())
                // TRN %1% is the reason the printer gave, in English.
                CallAfter([this, message = format_wxstr(_L("Could not delete the file: %1%"), *error)] {
                    MessageDialog(this, message, _L("Delete"), wxOK | wxICON_ERROR).ShowModal();
                });
            refresh();
        });
}

void MoonrakerFilesPanel::apply_colors()
{
    const wxColour background = StateColor::darkModeColorFor(wxColour("#FFFFFF"));
    SetBackgroundColour(background);
    m_list->SetBackgroundColour(background);
    m_title->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#262E30")));
    m_message->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#6B6B6B")));
}

void MoonrakerFilesPanel::Rescale()
{
    m_refresh->Rescale();
    rebuild_rows();
    Layout();
}

void MoonrakerFilesPanel::on_sys_color_changed()
{
    apply_colors();
    rebuild_rows();
    Refresh();
}

}} // namespace Slic3r::GUI
