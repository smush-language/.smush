#include "ui_backend.h"

#include <iostream>
#include <stdexcept>
#include <string>

#include "bytecode_vm.h"

#if SMUSH_HAS_WXWIDGETS
#include <wx/app.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/frame.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#endif

namespace smush::ui {

#if SMUSH_HAS_WXWIDGETS

namespace {

class SmushWxApp : public wxApp {
public:
    bool OnInit() override { return true; }
};

wxIMPLEMENT_APP_NO_MAIN(SmushWxApp);

bool g_wxInitialized = false;

struct BuiltNode {
    wxWindow* window = nullptr;
    wxSizer* sizer = nullptr;
};

void reportUiCallbackFailure(const std::exception& ex) {
    std::cerr << "[smush ui] " << ex.what() << '\n';
}

bool ensureWxStarted() {
    if (g_wxInitialized) {
        return true;
    }
    int argc = 0;
    char** argv = nullptr;
    if (!wxEntryStart(argc, argv)) {
        return false;
    }
    wxApp::SetInstance(new SmushWxApp());
    if (!wxTheApp || !wxTheApp->CallOnInit()) {
        return false;
    }
    g_wxInitialized = true;
    return true;
}

wxColour parseHexColor(const std::string& value, const wxColour& fallback = *wxBLACK) {
    if (value.size() == 7 && value[0] == '#') {
        try {
            const int r = std::stoi(value.substr(1, 2), nullptr, 16);
            const int g = std::stoi(value.substr(3, 2), nullptr, 16);
            const int b = std::stoi(value.substr(5, 2), nullptr, 16);
            return wxColour(r, g, b);
        } catch (...) {
        }
    }
    return fallback;
}

void applyThemeToWindow(wxWindow* widget, UiThemeObject* theme) {
    if (!widget || !theme) {
        return;
    }
    auto background = theme->values.find("background");
    auto foreground = theme->values.find("foreground");
    if (background != theme->values.end()) {
        widget->SetBackgroundColour(parseHexColor(background->second, widget->GetBackgroundColour()));
    }
    if (foreground != theme->values.end()) {
        widget->SetForegroundColour(parseHexColor(foreground->second, widget->GetForegroundColour()));
    }
}

BuiltNode buildView(BytecodeVM& vm, UiViewObject& view, wxWindow* parent);

void addBuiltNode(wxSizer* sizer, const BuiltNode& node) {
    if (!sizer) {
        return;
    }
    if (node.window) {
        sizer->Add(node.window, 0, wxALL | wxEXPAND, 6);
    } else if (node.sizer) {
        sizer->Add(node.sizer, 0, wxALL | wxEXPAND, 6);
    }
}

BuiltNode buildLayout(BytecodeVM& vm, UiViewObject& view, wxWindow* parent, wxSizer* sizer) {
    view.nativeHandle = reinterpret_cast<uintptr_t>(sizer);
    for (const Value& childValue : view.children) {
        if (childValue.kind != Value::Kind::Object || !childValue.objectValue ||
            childValue.objectValue->kind != ObjectKind::UiView) {
            continue;
        }
        auto* child = static_cast<UiViewObject*>(childValue.objectValue);
        addBuiltNode(sizer, buildView(vm, *child, parent));
    }
    return BuiltNode{nullptr, sizer};
}

BuiltNode buildView(BytecodeVM& vm, UiViewObject& view, wxWindow* parent) {
    switch (view.viewKind) {
        case UiViewKind::Label: {
            auto* widget = new wxStaticText(parent, wxID_ANY, wxString::FromUTF8(view.text));
            view.nativeHandle = reinterpret_cast<uintptr_t>(widget);
            return BuiltNode{widget, nullptr};
        }
        case UiViewKind::Button: {
            auto* widget = new wxButton(parent, wxID_ANY, wxString::FromUTF8(view.text));
            view.nativeHandle = reinterpret_cast<uintptr_t>(widget);
            widget->Bind(wxEVT_BUTTON, [&vm, &view](wxCommandEvent&) {
                try {
                    vm.invokeUiCallback(view.onClick, {});
                } catch (const std::exception& ex) {
                    reportUiCallbackFailure(ex);
                }
            });
            return BuiltNode{widget, nullptr};
        }
        case UiViewKind::TextField:
        case UiViewKind::TextArea: {
            long style = wxTE_PROCESS_ENTER;
            if (view.viewKind == UiViewKind::TextArea) {
                style |= wxTE_MULTILINE;
            }
            auto* widget = new wxTextCtrl(parent, wxID_ANY, wxString::FromUTF8(view.text), wxDefaultPosition, wxDefaultSize, style);
            if (!view.placeholder.empty()) {
                widget->SetHint(wxString::FromUTF8(view.placeholder));
            }
            view.nativeHandle = reinterpret_cast<uintptr_t>(widget);
            widget->Bind(wxEVT_TEXT, [&vm, &view, widget](wxCommandEvent&) {
                view.text = widget->GetValue().ToStdString();
                try {
                    vm.invokeUiCallback(view.onChange, {Value::string(view.text)});
                } catch (const std::exception& ex) {
                    reportUiCallbackFailure(ex);
                }
            });
            return BuiltNode{widget, nullptr};
        }
        case UiViewKind::CheckBox: {
            auto* widget = new wxCheckBox(parent, wxID_ANY, wxString::FromUTF8(view.text));
            widget->SetValue(view.checked);
            view.nativeHandle = reinterpret_cast<uintptr_t>(widget);
            widget->Bind(wxEVT_CHECKBOX, [&vm, &view, widget](wxCommandEvent&) {
                view.checked = widget->GetValue();
                try {
                    vm.invokeUiCallback(view.onChange, {Value::boolean(view.checked)});
                } catch (const std::exception& ex) {
                    reportUiCallbackFailure(ex);
                }
            });
            return BuiltNode{widget, nullptr};
        }
        case UiViewKind::Row: {
            auto* sizer = new wxBoxSizer(wxHORIZONTAL);
            return buildLayout(vm, view, parent, sizer);
        }
        case UiViewKind::Column: {
            auto* sizer = new wxBoxSizer(wxVERTICAL);
            return buildLayout(vm, view, parent, sizer);
        }
        case UiViewKind::Grid: {
            auto* sizer = new wxGridSizer(std::max(1, view.gridColumns), 6, 6);
            return buildLayout(vm, view, parent, sizer);
        }
    }
    return {};
}

void rebuildWindowContent(BytecodeVM& vm, UiWindowObject& window) {
    auto* frame = reinterpret_cast<wxFrame*>(window.nativeHandle);
    if (!frame) {
        return;
    }

    if (window.nativeAuxHandle != 0) {
        auto* oldPanel = reinterpret_cast<wxPanel*>(window.nativeAuxHandle);
        oldPanel->Destroy();
        window.nativeAuxHandle = 0;
    }

    auto* panel = new wxPanel(frame);
    window.nativeAuxHandle = reinterpret_cast<uintptr_t>(panel);

    if (window.content.kind == Value::Kind::Object && window.content.objectValue &&
        window.content.objectValue->kind == ObjectKind::UiView) {
        auto* rootView = static_cast<UiViewObject*>(window.content.objectValue);
        BuiltNode root = buildView(vm, *rootView, panel);
        wxSizer* rootSizer = root.sizer;
        if (!rootSizer) {
            rootSizer = new wxBoxSizer(wxVERTICAL);
            if (root.window) {
                rootSizer->Add(root.window, 0, wxALL | wxEXPAND, 6);
            }
        }
        panel->SetSizer(rootSizer);
        rootSizer->SetSizeHints(panel);
    }

    auto* frameSizer = new wxBoxSizer(wxVERTICAL);
    frameSizer->Add(panel, 1, wxEXPAND);
    frame->SetSizer(frameSizer);
    frame->Layout();
}

void createWindow(BytecodeVM& vm, UiWindowObject& window) {
    if (window.nativeHandle != 0) {
        return;
    }

    auto* frame = new wxFrame(nullptr, wxID_ANY, wxString::FromUTF8(window.title), wxDefaultPosition, wxSize(window.width, window.height));
    window.nativeHandle = reinterpret_cast<uintptr_t>(frame);
    frame->Bind(wxEVT_CLOSE_WINDOW, [&vm, &window, frame](wxCloseEvent& event) {
        window.visible = false;
        try {
            vm.invokeUiCallback(window.onClose, {});
        } catch (const std::exception& ex) {
            reportUiCallbackFailure(ex);
        }
        frame->Destroy();
        window.nativeHandle = 0;
        window.nativeAuxHandle = 0;
        event.Skip(false);
    });

    rebuildWindowContent(vm, window);
    setWindowTitle(window);
    setWindowSize(window);
    applyTheme(window);
}

}  // namespace

#endif

std::string backendName() {
#if SMUSH_HAS_WXWIDGETS
    return "wxwidgets";
#else
    return "headless";
#endif
}

bool backendAvailable() {
#if SMUSH_HAS_WXWIDGETS
    return true;
#else
    return false;
#endif
}

int runApp(BytecodeVM& vm, UiAppObject& app) {
    app.running = true;
#if SMUSH_HAS_WXWIDGETS
    if (!ensureWxStarted()) {
        app.running = false;
        throw std::runtime_error("failed to initialize wxWidgets backend");
    }

    bool openedAny = false;
    for (const Value& windowValue : app.windows) {
        if (windowValue.kind != Value::Kind::Object || !windowValue.objectValue ||
            windowValue.objectValue->kind != ObjectKind::UiWindow) {
            continue;
        }
        auto* window = static_cast<UiWindowObject*>(windowValue.objectValue);
        if (!window->visible) {
            continue;
        }
        createWindow(vm, *window);
        auto* frame = reinterpret_cast<wxFrame*>(window->nativeHandle);
        if (frame) {
            frame->Show(true);
            openedAny = true;
        }
    }

    int exitCode = 0;
    if (openedAny && wxTheApp) {
        exitCode = wxTheApp->OnRun();
    }
    app.running = false;
    return exitCode;
#else
    app.running = false;
    return 0;
#endif
}

void showWindow(UiWindowObject& window) {
    window.visible = true;
#if SMUSH_HAS_WXWIDGETS
    auto* frame = reinterpret_cast<wxFrame*>(window.nativeHandle);
    if (frame) {
        frame->Show(true);
    }
#endif
}

void closeWindow(BytecodeVM& vm, UiWindowObject& window) {
    window.visible = false;
#if SMUSH_HAS_WXWIDGETS
    auto* frame = reinterpret_cast<wxFrame*>(window.nativeHandle);
    if (frame) {
        frame->Close();
        return;
    }
#endif
    vm.invokeUiCallback(window.onClose, {});
}

void setWindowTitle(UiWindowObject& window) {
#if SMUSH_HAS_WXWIDGETS
    auto* frame = reinterpret_cast<wxFrame*>(window.nativeHandle);
    if (frame) {
        frame->SetTitle(wxString::FromUTF8(window.title));
    }
#endif
}

void setWindowSize(UiWindowObject& window) {
#if SMUSH_HAS_WXWIDGETS
    auto* frame = reinterpret_cast<wxFrame*>(window.nativeHandle);
    if (frame) {
        frame->SetClientSize(window.width, window.height);
    }
#endif
}

void setWindowContent(BytecodeVM& vm, UiWindowObject& window) {
#if SMUSH_HAS_WXWIDGETS
    if (window.nativeHandle != 0) {
        rebuildWindowContent(vm, window);
        applyTheme(window);
    }
#else
    (void)vm;
    (void)window;
#endif
}

void applyTheme(UiWindowObject& window) {
#if SMUSH_HAS_WXWIDGETS
    auto* frame = reinterpret_cast<wxFrame*>(window.nativeHandle);
    UiThemeObject* theme = nullptr;
    if (window.theme.kind == Value::Kind::Object && window.theme.objectValue &&
        window.theme.objectValue->kind == ObjectKind::UiTheme) {
        theme = static_cast<UiThemeObject*>(window.theme.objectValue);
    }
    if (!frame || !theme) {
        return;
    }
    applyThemeToWindow(frame, theme);
    auto* panel = reinterpret_cast<wxPanel*>(window.nativeAuxHandle);
    applyThemeToWindow(panel, theme);
    if (panel) {
        for (wxWindowList::compatibility_iterator node = panel->GetChildren().GetFirst(); node; node = node->GetNext()) {
            applyThemeToWindow(node->GetData(), theme);
        }
        panel->Refresh();
    }
    frame->Refresh();
#else
    (void)window;
#endif
}

void setViewText(BytecodeVM& vm, UiViewObject& view) {
#if SMUSH_HAS_WXWIDGETS
    switch (view.viewKind) {
        case UiViewKind::Label: {
            auto* widget = reinterpret_cast<wxStaticText*>(view.nativeHandle);
            if (widget) {
                widget->SetLabel(wxString::FromUTF8(view.text));
            }
            break;
        }
        case UiViewKind::Button: {
            auto* widget = reinterpret_cast<wxButton*>(view.nativeHandle);
            if (widget) {
                widget->SetLabel(wxString::FromUTF8(view.text));
            }
            break;
        }
        case UiViewKind::TextField:
        case UiViewKind::TextArea: {
            auto* widget = reinterpret_cast<wxTextCtrl*>(view.nativeHandle);
            if (widget && widget->GetValue().ToStdString() != view.text) {
                widget->ChangeValue(wxString::FromUTF8(view.text));
            }
            break;
        }
        case UiViewKind::CheckBox: {
            auto* widget = reinterpret_cast<wxCheckBox*>(view.nativeHandle);
            if (widget) {
                widget->SetLabel(wxString::FromUTF8(view.text));
            }
            break;
        }
        default:
            break;
    }
#endif
    if (view.onChange.kind != Value::Kind::Null) {
        vm.invokeUiCallback(view.onChange, {Value::string(view.text)});
    }
}

void setViewPlaceholder(UiViewObject& view) {
#if SMUSH_HAS_WXWIDGETS
    auto* widget = reinterpret_cast<wxTextCtrl*>(view.nativeHandle);
    if (widget) {
        widget->SetHint(wxString::FromUTF8(view.placeholder));
    }
#else
    (void)view;
#endif
}

void setViewChecked(BytecodeVM& vm, UiViewObject& view) {
#if SMUSH_HAS_WXWIDGETS
    auto* widget = reinterpret_cast<wxCheckBox*>(view.nativeHandle);
    if (widget) {
        widget->SetValue(view.checked);
    }
#endif
    if (view.onChange.kind != Value::Kind::Null) {
        vm.invokeUiCallback(view.onChange, {Value::boolean(view.checked)});
    }
}

void clickView(BytecodeVM& vm, UiViewObject& view) {
#if SMUSH_HAS_WXWIDGETS
    if (view.viewKind == UiViewKind::Button) {
        auto* widget = reinterpret_cast<wxButton*>(view.nativeHandle);
        if (widget) {
            wxCommandEvent event(wxEVT_BUTTON, widget->GetId());
            widget->ProcessWindowEvent(event);
            return;
        }
    }
#endif
    vm.invokeUiCallback(view.onClick, {});
}

void infoDialog(const std::string& title, const std::string& message) {
#if SMUSH_HAS_WXWIDGETS
    if (g_wxInitialized) {
        wxMessageBox(wxString::FromUTF8(message), wxString::FromUTF8(title), wxOK | wxICON_INFORMATION);
    }
#else
    (void)title;
    (void)message;
#endif
}

bool confirmDialog(const std::string& title, const std::string& message) {
#if SMUSH_HAS_WXWIDGETS
    if (g_wxInitialized) {
        return wxMessageBox(wxString::FromUTF8(message), wxString::FromUTF8(title), wxYES_NO | wxICON_QUESTION) == wxYES;
    }
#else
    (void)title;
    (void)message;
#endif
    return false;
}

}  // namespace smush::ui
