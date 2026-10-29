#pragma once

#include <wx/image.h>
#include <wx/panel.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "slic3r/Utils/MoonrakerPrinterAgent.hpp"

class wxBoxSizer;
class wxScrolledWindow;
class wxStaticBitmap;
class Button;
class Label;

namespace Slic3r {

class MachineObject;

namespace GUI {

class Worker;

// The Device tab's Storage page for printers driven through Moonraker. The Bambu page reads the
// printer's storage over Bambu's own file tunnel, which these printers do not have, so this one
// lists the G-code files Moonraker holds, newest first, and prints or deletes them.
class MoonrakerFilesPanel : public wxPanel
{
public:
    explicit MoonrakerFilesPanel(wxWindow* parent);
    ~MoonrakerFilesPanel() override;

    // Whether the selected printer's agent speaks Moonraker, which this page needs.
    static bool is_supported();

    // Lists the files the first time a printer's page is shown, and again when the printer changes.
    void UpdateByObj(MachineObject* obj);
    void Rescale();
    void on_sys_color_changed();

private:
    using PrinterFile = MoonrakerPrinterAgent::PrinterFile;

    struct Row
    {
        Label*          details   = nullptr;
        wxStaticBitmap* thumbnail = nullptr;
    };

    void     refresh();
    void     show_files(std::vector<PrinterFile> files, const std::string& error);
    void     show_message(const wxString& message);
    void     rebuild_rows();
    void     add_row(const PrinterFile& file);
    void     update_file(const PrinterFile& file, const std::string& thumbnail_data);
    wxString details_text(const PrinterFile& file) const;
    wxBitmap thumbnail_bitmap(const PrinterFile& file) const;
    void     print_file(const std::string& path);
    void     delete_file(const std::string& path);
    void     apply_colors();

    std::unique_ptr<Worker>        m_worker;
    std::string                    m_dev_id;
    unsigned                       m_generation = 0;
    std::vector<PrinterFile>       m_files;
    std::map<std::string, wxImage> m_thumbnails; // keyed by the file path

    Label*                                  m_title     = nullptr;
    Button*                                 m_refresh   = nullptr;
    Label*                                  m_message   = nullptr;
    wxScrolledWindow*                       m_list      = nullptr;
    wxBoxSizer*                             m_list_sizer = nullptr;
    std::map<std::string, Row>              m_rows; // keyed by the file path
};

}} // namespace Slic3r::GUI
