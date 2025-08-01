///////////////////////////////////////////////////////////////////////////////
// Name:        src/aui/framemanager.cpp
// Purpose:     wxaui: wx advanced user interface - docking window manager
// Author:      Benjamin I. Williams
// Created:     2005-05-17
// Copyright:   (C) Copyright 2005-2006, Kirix Corporation, All Rights Reserved
// Licence:     wxWindows Library Licence, Version 3.1
///////////////////////////////////////////////////////////////////////////////

// ============================================================================
// declarations
// ============================================================================

// ----------------------------------------------------------------------------
// headers
// ----------------------------------------------------------------------------

#include "wx/wxprec.h"


#if wxUSE_AUI

#include "wx/aui/framemanager.h"
#include "wx/aui/dockart.h"
#include "wx/aui/floatpane.h"
#include "wx/aui/tabmdi.h"
#include "wx/aui/auibar.h"
#include "wx/aui/auibook.h"
#include "wx/aui/serializer.h"
#include "wx/mdi.h"
#include "wx/wupdlock.h"

#ifndef WX_PRECOMP
    #include "wx/panel.h"
    #include "wx/settings.h"
    #include "wx/app.h"
    #include "wx/dcclient.h"
    #include "wx/dcmemory.h"
    #include "wx/toolbar.h"
    #include "wx/image.h"
    #include "wx/statusbr.h"
#endif

WX_CHECK_BUILD_OPTIONS("wxAUI")

wxAuiPaneInfo wxAuiNullPaneInfo;
wxAuiDockInfo wxAuiNullDockInfo;
wxDEFINE_EVENT( wxEVT_AUI_PANE_BUTTON, wxAuiManagerEvent );
wxDEFINE_EVENT( wxEVT_AUI_PANE_CLOSE, wxAuiManagerEvent );
wxDEFINE_EVENT( wxEVT_AUI_PANE_MAXIMIZE, wxAuiManagerEvent );
wxDEFINE_EVENT( wxEVT_AUI_PANE_RESTORE, wxAuiManagerEvent );
wxDEFINE_EVENT( wxEVT_AUI_PANE_ACTIVATED, wxAuiManagerEvent );
wxDEFINE_EVENT( wxEVT_AUI_RENDER, wxAuiManagerEvent );
wxDEFINE_EVENT( wxEVT_AUI_FIND_MANAGER, wxAuiManagerEvent );

#ifdef __WXMSW__
    #include "wx/msw/wrapwin.h"
    #include "wx/msw/private.h"
    #include "wx/msw/dc.h"
#endif

#include "wx/dcgraph.h"

#include "wx/generic/private/drawresize.h"

#include <map>
#include <memory>

wxIMPLEMENT_DYNAMIC_CLASS(wxAuiManagerEvent, wxEvent);
wxIMPLEMENT_CLASS(wxAuiManager, wxEvtHandler);


// -- local constants and helper functions --
namespace
{

// Index of the outermost layer used for all toolbars.
constexpr int auiToolBarLayer = 10;

// Default proportion which is "infinitely" greater than anything else.
constexpr int maxDockProportion = 100000;


wxBitmap wxCreateVenetianBlindsBitmap(wxByte r, wxByte g, wxByte b, wxByte a)
{
    const unsigned char c = wxSystemSettings::GetAppearance().IsDark() ? 220 : 5;

    unsigned char data[] = { r,g,b, r,g,b, c,c,c, c,c,c };
    unsigned char alpha[] = { a, a, 200, 200 };

    wxImage img(1,4,data,true);
    img.SetAlpha(alpha,true);
    return wxBitmap(img);
}

// CopyDocksAndPanes() - this utility function creates copies of
// the dock and pane info.  wxAuiDockInfo's usually contain pointers
// to wxAuiPaneInfo classes, thus this function is necessary to reliably
// reconstruct that relationship in the new dock info and pane info arrays

void
CopyDocksAndPanes(wxAuiDockInfoArray& dest_docks,
                  wxAuiPaneInfoArray& dest_panes,
                  const wxAuiDockInfoArray& src_docks,
                  const wxAuiPaneInfoArray& src_panes)
{
    dest_docks = src_docks;
    dest_panes = src_panes;
    int j, k, pc1, pc2;
    for ( auto& dock : dest_docks )
    {
        for (j = 0, pc1 = dock.panes.GetCount(); j < pc1; ++j)
            for (k = 0, pc2 = src_panes.GetCount(); k < pc2; ++k)
                if (dock.panes.Item(j) == &src_panes.Item(k))
                    dock.panes.Item(j) = &dest_panes.Item(k);
    }
}

// GetMaxLayer() is an internal function which returns
// the highest layer inside the specified dock
int GetMaxLayer(const wxAuiDockInfoArray& docks, int dock_direction)
{
    int max_layer = 0;
    for ( const auto& dock : docks )
    {
        if (dock.dock_direction == dock_direction &&
            dock.dock_layer > max_layer && !dock.fixed)
                max_layer = dock.dock_layer;
    }
    return max_layer;
}


// GetMaxRow() is an internal function which returns
// the highest layer inside the specified dock
int GetMaxRow(const wxAuiPaneInfoArray& panes, int direction, int layer)
{
    int max_row = 0;
    for ( const auto& pane : panes )
    {
        if (pane.dock_direction == direction &&
            pane.dock_layer == layer &&
            pane.dock_row > max_row)
                max_row = pane.dock_row;
    }
    return max_row;
}



// DoInsertDockLayer() is an internal function that inserts a new dock
// layer by incrementing all existing dock layer values by one
void
DoInsertDockLayer(wxAuiPaneInfoArray& panes,
                  int dock_direction,
                  int dock_layer)
{
    for ( auto& pane : panes )
    {
        if (!pane.IsFloating() &&
            pane.dock_direction == dock_direction &&
            pane.dock_layer >= dock_layer)
                pane.dock_layer++;
    }
}

// DoInsertDockLayer() is an internal function that inserts a new dock
// row by incrementing all existing dock row values by one
void
DoInsertDockRow(wxAuiPaneInfoArray& panes,
                int dock_direction,
                int dock_layer,
                int dock_row)
{
    for ( auto& pane : panes )
    {
        if (!pane.IsFloating() &&
            pane.dock_direction == dock_direction &&
            pane.dock_layer == dock_layer &&
            pane.dock_row >= dock_row)
                pane.dock_row++;
    }
}

// DoInsertDockLayer() is an internal function that inserts a space for
// another dock pane by incrementing all existing dock row values by one
void
DoInsertPane(wxAuiPaneInfoArray& panes,
             int dock_direction,
             int dock_layer,
             int dock_row,
             int dock_pos)
{
    for ( auto& pane : panes )
    {
        if (!pane.IsFloating() &&
            pane.dock_direction == dock_direction &&
            pane.dock_layer == dock_layer &&
            pane.dock_row == dock_row &&
            pane.dock_pos >= dock_pos)
                pane.dock_pos++;
    }
}

// Flags for FindDocks()
enum class FindDocksFlags
{
    // No special flags.
    None = 0,

    // Stop after the first found dock, returned array has 0 or 1 elements.
    OnlyFirst = 1,

    // Reverse the order of the returned docks, useful for right/bottom docks.
    ReverseOrder = 2
};

// FindDocks() is an internal function that returns a list of docks which meet
// the specified conditions in the parameters and returns a sorted array
// (sorted by layer and then row)
wxAuiDockInfoPtrArray
FindDocks(wxAuiDockInfoArray& docks,
          int dock_direction,
          int dock_layer,
          int dock_row,
          FindDocksFlags flags = FindDocksFlags::None)
{
    // discover the maximum dock layer and the max row
    int max_row = 0, max_layer = 0;
    for ( const auto& d : docks )
    {
        max_row = wxMax(max_row, d.dock_row);
        max_layer = wxMax(max_layer, d.dock_layer);
    }

    // if no dock layer was specified, search all dock layers
    int begin_layer, end_layer;
    if (dock_layer == -1)
    {
        begin_layer = 0;
        end_layer = max_layer;
    }
    else
    {
        begin_layer = dock_layer;
        end_layer = dock_layer;
    }

    // if no dock row was specified, search all dock rows
    int begin_row, end_row;
    if (dock_row == -1)
    {
        begin_row = 0;
        end_row = max_row;
    }
    else
    {
        begin_row = dock_row;
        end_row = dock_row;
    }

    wxAuiDockInfoPtrArray arr;

    for (int layer = begin_layer; layer <= end_layer; ++layer)
    {
        for (int row = begin_row; row <= end_row; ++row)
        {
            for ( auto& d : docks )
            {
                if (dock_direction == -1 || dock_direction == d.dock_direction)
                {
                    if (d.dock_layer == layer && d.dock_row == row)
                    {
                        switch ( flags )
                        {
                            case FindDocksFlags::None:
                                arr.Add(&d);
                                break;

                            case FindDocksFlags::OnlyFirst:
                                arr.Add(&d);
                                return arr;

                            case FindDocksFlags::ReverseOrder:
                                // Inserting into an array is inefficient, but
                                // we won't have more than a few docks here, so
                                // it doesn't matter, and it keeps the code
                                // simpler.
                                arr.Insert(&d, 0);
                                break;
                        }
                    }
                }
            }
        }
    }

    return arr;
}

// FindPaneInDock() looks up a specified window pointer inside a dock.
// If found, the corresponding wxAuiPaneInfo pointer is returned, otherwise nullptr.
wxAuiPaneInfo* FindPaneInDock(const wxAuiDockInfo& dock, wxWindow* window)
{
    for ( const auto& p : dock.panes )
    {
        if (p->window == window)
            return p;
    }
    return nullptr;
}

// RemovePaneFromDocks() removes a pane window from all docks
// with a possible exception specified by parameter "ex_cept"
void
RemovePaneFromDocks(wxAuiDockInfoArray& docks,
                    wxAuiPaneInfo& pane,
                    wxAuiDockInfo* ex_cept  = nullptr)
{
    for ( auto& d : docks )
    {
        if (&d == ex_cept)
            continue;
        wxAuiPaneInfo* pi = FindPaneInDock(d, pane.window);
        if (pi)
            d.panes.Remove(pi);
    }
}

/*
// This function works fine, and may be used in the future

// RenumberDockRows() takes a dock and assigns sequential numbers
// to existing rows.  Basically it takes out the gaps; so if a
// dock has rows with numbers 0,2,5, they will become 0,1,2
void RenumberDockRows(wxAuiDockInfoPtrArray& docks)
{
    int i, dock_count;
    for (i = 0, dock_count = docks.GetCount(); i < dock_count; ++i)
    {
        wxAuiDockInfo& dock = *docks.Item(i);
        dock.dock_row = i;

        int j, pane_count;
        for (j = 0, pane_count = dock.panes.GetCount(); j < pane_count; ++j)
            dock.panes.Item(j)->dock_row = i;
    }
}
*/

} // anonymous namespace


// SetActivePane() sets the active pane, as well as cycles through
// every other pane and makes sure that all others' active flags
// are turned off
void wxAuiManager::SetActivePane(wxWindow* active_pane)
{
    wxAuiPaneInfo* active_paneinfo = nullptr;
    for ( auto& pane : m_panes )
    {
        pane.state &= ~wxAuiPaneInfo::optionActive;
        if (pane.window == active_pane)
        {
            pane.state |= wxAuiPaneInfo::optionActive;
            active_paneinfo = &pane;
        }
    }

    // send the 'activated' event after all panes have been updated
    if ( active_paneinfo )
    {
        wxAuiManagerEvent evt(wxEVT_AUI_PANE_ACTIVATED);
        evt.SetManager(this);
        evt.SetPane(active_paneinfo);
        ProcessMgrEvent(evt);
    }
}


// this function is used to sort panes by dock position
static int PaneSortFunc(wxAuiPaneInfo** p1, wxAuiPaneInfo** p2)
{
    return ((*p1)->dock_pos < (*p2)->dock_pos) ? -1 : 1;
}


bool wxAuiPaneInfo::IsValid() const
{
    // Should this RTTI and function call be rewritten as
    // sending a new event type to allow other window types
    // to check the pane settings?
    wxAuiToolBar* toolbar = wxDynamicCast(window, wxAuiToolBar);
    return !toolbar || toolbar->IsPaneValid(*this);
}

// -- wxAuiManager class implementation --


wxAuiManager::wxAuiManager(wxWindow* managed_wnd, unsigned int flags)
{
    m_action = actionNone;
    m_actionWindow = nullptr;
    m_hoverButton = nullptr;
    m_art = new wxAuiDefaultDockArt;
    m_flags = flags;
    m_hasMaximized = false;
    m_frame = nullptr;
    m_dockConstraintX = 0.3;
    m_dockConstraintY = 0.3;
    m_hintFadeMax = 64;

    m_reserved = nullptr;
    m_currentDragItem = -1;

    if (managed_wnd)
    {
        SetManagedWindow(managed_wnd);
    }
}

wxAuiManager::~wxAuiManager()
{
    UnInit();

    // NOTE: It's possible that the windows have already been destroyed by the
    // time this dtor is called, so this loop can result in memory access via
    // invalid pointers, resulting in a crash.  So it will be disabled while
    // waiting for a better solution.
#if 0
    for ( size_t i = 0; i < m_panes.size(); i++ )
    {
        wxAuiPaneInfo& pinfo = m_panes[i];
        if (pinfo.window && !pinfo.window->GetParent())
            delete pinfo.window;
    }
#endif

    delete m_art;
}

int wxAuiManager::GetActionPartIndex() const
{
    int n = 0;
    for ( const auto& uiPart : m_uiParts )
    {
        if ( &uiPart == m_actionPart )
            return n;

        ++n;
    }

    return wxNOT_FOUND;
}

void wxAuiManager::OnSysColourChanged(wxSysColourChangedEvent& event)
{
    m_art->UpdateColoursFromSystem();
    m_frame->Refresh();
    event.Skip(true);
}

// creates a floating frame for the windows
wxAuiFloatingFrame* wxAuiManager::CreateFloatingFrame(wxWindow* parent,
                                                      const wxAuiPaneInfo& paneInfo)
{
    return new wxAuiFloatingFrame(parent, this, paneInfo);
}

bool wxAuiManager::CanDockPanel(const wxAuiPaneInfo & WXUNUSED(p))
{
    // if a key modifier is pressed while dragging the frame,
    // don't dock the window
    return !(wxGetKeyState(WXK_CONTROL) || wxGetKeyState(WXK_ALT));
}

// GetPane() looks up a wxAuiPaneInfo structure based
// on the supplied window pointer.  Upon failure, GetPane()
// returns an empty wxAuiPaneInfo, a condition which can be checked
// by calling wxAuiPaneInfo::IsOk().
//
// The pane info's structure may then be modified.  Once a pane's
// info is modified, wxAuiManager::Update() must be called to
// realize the changes in the UI.

wxAuiPaneInfo& wxAuiManager::GetPane(wxWindow* window)
{
    for ( auto& p : m_panes )
    {
        if (p.window == window)
            return p;
    }
    return wxAuiNullPaneInfo;
}

// this version of GetPane() looks up a pane based on a
// 'pane name', see above comment for more info
wxAuiPaneInfo& wxAuiManager::GetPane(const wxString& name)
{
    for ( auto& p : m_panes )
    {
        if (p.name == name)
            return p;
    }
    return wxAuiNullPaneInfo;
}

// HitTest() is an internal function which determines
// which UI item the specified coordinates are over
// (x,y) specify a position in client coordinates
wxAuiDockUIPart* wxAuiManager::HitTest(int x, int y)
{
    wxAuiDockUIPart* result = nullptr;

    for ( auto& uiPart : m_uiParts )
    {
        wxAuiDockUIPart* item = &uiPart;

        // we are not interested in typeDock, because this space
        // isn't used to draw anything, just for measurements;
        // besides, the entire dock area is covered with other
        // rectangles, which we are interested in.
        if (item->type == wxAuiDockUIPart::typeDock)
            continue;

        // if we already have a hit on a more specific item, we are not
        // interested in a pane hit.  If, however, we don't already have
        // a hit, returning a pane hit is necessary for some operations
        if ((item->type == wxAuiDockUIPart::typePane ||
            item->type == wxAuiDockUIPart::typePaneBorder) && result)
            continue;

        // if the point is inside the rectangle, we have a hit
        if (item->rect.Contains(x,y))
            result = item;
    }

    return result;
}


// SetFlags() and GetFlags() allow the owner to set various
// options which are global to wxAuiManager
void wxAuiManager::SetFlags(unsigned int flags)
{
    // set the new flags
    m_flags = flags;
}

unsigned int wxAuiManager::GetFlags() const
{
    return m_flags;
}

/* static */ bool wxAuiManager::AlwaysUsesLiveResize(const wxWindow* WXUNUSED(window))
{
    return false;
}

bool wxAuiManager::HasLiveResize() const
{
    return (GetFlags() & wxAUI_MGR_LIVE_RESIZE) == wxAUI_MGR_LIVE_RESIZE;
}

// don't use these anymore as they are deprecated
// use Set/GetManagedFrame() instead
void wxAuiManager::SetFrame(wxFrame* frame)
{
    SetManagedWindow((wxWindow*)frame);
}

wxFrame* wxAuiManager::GetFrame() const
{
    return (wxFrame*)m_frame;
}


// this function will return the aui manager for a given
// window.  The |window| parameter should be any child window
// or grand-child window (and so on) of the frame/window
// managed by wxAuiManager.  The |window| parameter does not
// need to be managed by the manager itself.
wxAuiManager* wxAuiManager::GetManager(wxWindow* window)
{
    wxAuiManagerEvent evt(wxEVT_AUI_FIND_MANAGER);
    evt.SetManager(nullptr);
    evt.ResumePropagation(wxEVENT_PROPAGATE_MAX);
    if (!window->GetEventHandler()->ProcessEvent(evt))
        return nullptr;

    return evt.GetManager();
}


// SetManagedWindow() is usually called once when the frame
// manager class is being initialized.  "frame" specifies
// the frame which should be managed by the frame manager
void wxAuiManager::SetManagedWindow(wxWindow* wnd)
{
    wxASSERT_MSG(wnd, wxT("specified window must be non-null"));

    UnInit();

    m_frame = wnd;
    m_frame->Bind(wxEVT_AUI_PANE_BUTTON, &wxAuiManager::OnPaneButton, this);
    m_frame->Bind(wxEVT_AUI_RENDER, &wxAuiManager::OnRender, this);
    m_frame->Bind(wxEVT_DESTROY, &wxAuiManager::OnDestroy, this);
    m_frame->Bind(wxEVT_PAINT, &wxAuiManager::OnPaint, this);
    m_frame->Bind(wxEVT_ERASE_BACKGROUND, &wxAuiManager::OnEraseBackground, this);
    m_frame->Bind(wxEVT_SIZE, &wxAuiManager::OnSize, this);
    m_frame->Bind(wxEVT_SET_CURSOR, &wxAuiManager::OnSetCursor, this);
    m_frame->Bind(wxEVT_LEFT_DOWN, &wxAuiManager::OnLeftDown, this);
    m_frame->Bind(wxEVT_LEFT_UP, &wxAuiManager::OnLeftUp, this);
    m_frame->Bind(wxEVT_MOTION, &wxAuiManager::OnMotion, this);
    m_frame->Bind(wxEVT_LEAVE_WINDOW, &wxAuiManager::OnLeaveWindow, this);
    m_frame->Bind(wxEVT_MOUSE_CAPTURE_LOST, &wxAuiManager::OnCaptureLost, this);
    m_frame->Bind(wxEVT_CHILD_FOCUS, &wxAuiManager::OnChildFocus, this);
    m_frame->Bind(wxEVT_AUI_FIND_MANAGER, &wxAuiManager::OnFindManager, this);
    m_frame->Bind(wxEVT_SYS_COLOUR_CHANGED, &wxAuiManager::OnSysColourChanged, this);

#if wxUSE_MDI
    // if the owner is going to manage an MDI parent frame,
    // we need to add the MDI client window as the default
    // center pane

    if (wxDynamicCast(m_frame, wxMDIParentFrame))
    {
        wxMDIParentFrame* mdi_frame = (wxMDIParentFrame*)m_frame;
        wxWindow* client_window = mdi_frame->GetClientWindow();

        wxASSERT_MSG(client_window, wxT("Client window is null!"));

        AddPane(client_window,
                wxAuiPaneInfo().Name(wxT("mdiclient")).
                CenterPane().PaneBorder(false));
    }
    else if (wxDynamicCast(m_frame, wxAuiMDIParentFrame))
    {
        wxAuiMDIParentFrame* mdi_frame = (wxAuiMDIParentFrame*)m_frame;
        wxAuiMDIClientWindow* client_window = mdi_frame->GetClientWindow();
        wxASSERT_MSG(client_window, wxT("Client window is null!"));

        AddPane(client_window,
                wxAuiPaneInfo().Name(wxT("mdiclient")).
                CenterPane().PaneBorder(false));
    }

#endif
}


// UnInit() is called automatically by wxAuiManager itself when either the
// manager itself or its associated frame is destroyed, but can also be called
// explicitly, so make it safe to call it multiple times and just do nothing
// during any calls but the first one.
void wxAuiManager::UnInit()
{
    if (m_frame)
    {
        m_frame->Unbind(wxEVT_AUI_PANE_BUTTON, &wxAuiManager::OnPaneButton, this);
        m_frame->Unbind(wxEVT_AUI_RENDER, &wxAuiManager::OnRender, this);
        m_frame->Unbind(wxEVT_DESTROY, &wxAuiManager::OnDestroy, this);
        m_frame->Unbind(wxEVT_PAINT, &wxAuiManager::OnPaint, this);
        m_frame->Unbind(wxEVT_ERASE_BACKGROUND, &wxAuiManager::OnEraseBackground, this);
        m_frame->Unbind(wxEVT_SIZE, &wxAuiManager::OnSize, this);
        m_frame->Unbind(wxEVT_SET_CURSOR, &wxAuiManager::OnSetCursor, this);
        m_frame->Unbind(wxEVT_LEFT_DOWN, &wxAuiManager::OnLeftDown, this);
        m_frame->Unbind(wxEVT_LEFT_UP, &wxAuiManager::OnLeftUp, this);
        m_frame->Unbind(wxEVT_MOTION, &wxAuiManager::OnMotion, this);
        m_frame->Unbind(wxEVT_LEAVE_WINDOW, &wxAuiManager::OnLeaveWindow, this);
        m_frame->Unbind(wxEVT_MOUSE_CAPTURE_LOST, &wxAuiManager::OnCaptureLost, this);
        m_frame->Unbind(wxEVT_CHILD_FOCUS, &wxAuiManager::OnChildFocus, this);
        m_frame->Unbind(wxEVT_AUI_FIND_MANAGER, &wxAuiManager::OnFindManager, this);
        m_frame->Unbind(wxEVT_SYS_COLOUR_CHANGED, &wxAuiManager::OnSysColourChanged, this);
        m_frame = nullptr;
    }
}

// GetManagedWindow() returns the window pointer being managed
wxWindow* wxAuiManager::GetManagedWindow() const
{
    return m_frame;
}

wxAuiDockArt* wxAuiManager::GetArtProvider() const
{
    return m_art;
}

void wxAuiManager::ProcessMgrEvent(wxAuiManagerEvent& event)
{
    // first, give the owner frame a chance to override
    if (m_frame)
    {
        if (m_frame->GetEventHandler()->ProcessEvent(event))
            return;
    }

    ProcessEvent(event);
}

// SetArtProvider() instructs wxAuiManager to use the
// specified art provider for all drawing calls.  This allows
// plugable look-and-feel features.  The pointer that is
// passed to this method subsequently belongs to wxAuiManager,
// and is deleted in the frame manager destructor
void wxAuiManager::SetArtProvider(wxAuiDockArt* art_provider)
{
    // delete the last art provider, if any
    delete m_art;

    // assign the new art provider
    m_art = art_provider;
}


bool wxAuiManager::AddPane(wxWindow* window, const wxAuiPaneInfo& paneInfo)
{
    wxASSERT_MSG(window, wxT("null window ptrs are not allowed"));

    // check if the pane has a valid window
    if (!window)
        return false;

    // check if the window is already managed by us
    if (GetPane(paneInfo.window).IsOk())
        return false;

    // check if the pane name already exists, this could reveal a
    // bug in the library user's application
    bool already_exists = false;
    if (!paneInfo.name.empty() && GetPane(paneInfo.name).IsOk())
    {
        wxFAIL_MSG(wxT("A pane with that name already exists in the manager!"));
        already_exists = true;
    }

    // if the new pane is docked then we should undo maximize
    if (paneInfo.IsDocked())
        RestoreMaximizedPane();

    // special case:  wxAuiToolBar style interacts with docking flags
    wxAuiPaneInfo test(paneInfo);
    wxAuiToolBar* toolbar = wxDynamicCast(window, wxAuiToolBar);
    if (toolbar)
    {
        // if pane has default docking flags
        const unsigned int dockMask = wxAuiPaneInfo::optionLeftDockable |
                                        wxAuiPaneInfo::optionRightDockable |
                                        wxAuiPaneInfo::optionTopDockable |
                                        wxAuiPaneInfo::optionBottomDockable;
        const unsigned int defaultDock = wxAuiPaneInfo().
                                            DefaultPane().state & dockMask;
        if ((test.state & dockMask) == defaultDock)
        {
            // set docking flags based on toolbar style
            if (toolbar->GetWindowStyleFlag() & wxAUI_TB_VERTICAL)
            {
                test.TopDockable(false).BottomDockable(false);
            }
            else if (toolbar->GetWindowStyleFlag() & wxAUI_TB_HORIZONTAL)
            {
                test.LeftDockable(false).RightDockable(false);
            }
        }
        else
        {
            // see whether non-default docking flags are valid
            test.window = window;
            wxCHECK_MSG(test.IsValid(), false,
                        "toolbar style and pane docking flags are incompatible");
        }
    }

    m_panes.Add(test);

    wxAuiPaneInfo& pinfo = m_panes.Last();

    // set the pane window
    pinfo.window = window;


    // if the pane's name identifier is blank, create a random string
    if (pinfo.name.empty() || already_exists)
    {
        pinfo.name.Printf(wxT("%08lx%08x%08x%08lx"),
             (unsigned long)(wxPtrToUInt(pinfo.window) & 0xffffffff),
             (unsigned int)time(nullptr),
             (unsigned int)clock(),
             (unsigned long)m_panes.GetCount());
    }

    // set initial proportion (if not already set)
    if (pinfo.dock_proportion == 0)
        pinfo.dock_proportion = maxDockProportion;

    if (pinfo.HasGripper())
    {
        if (wxDynamicCast(pinfo.window, wxAuiToolBar))
        {
            // prevent duplicate gripper -- both wxAuiManager and wxAuiToolBar
            // have a gripper control.  The toolbar's built-in gripper
            // meshes better with the look and feel of the control than ours,
            // so turn wxAuiManager's gripper off, and the toolbar's on.

            wxAuiToolBar* tb = static_cast<wxAuiToolBar*>(pinfo.window);
            pinfo.SetFlag(wxAuiPaneInfo::optionGripper, false);
            tb->SetGripperVisible(true);
        }
    }


    if (pinfo.best_size == wxDefaultSize &&
        pinfo.window)
    {
        // It's important to use the current window size and not the best size
        // when adding a pane corresponding to a previously docked window: it
        // shouldn't change its size if it's dragged and docked in a different
        // place.
        pinfo.best_size = pinfo.window->GetSize();

        // But we still shouldn't make it too small.
        pinfo.best_size.IncTo(pinfo.window->GetBestSize());
        pinfo.best_size.IncTo(pinfo.min_size);
    }



    return true;
}

bool wxAuiManager::AddPane(wxWindow* window,
                           int direction,
                           const wxString& caption)
{
    wxAuiPaneInfo pinfo;
    pinfo.Caption(caption);
    switch (direction)
    {
        case wxTOP:    pinfo.Top(); break;
        case wxBOTTOM: pinfo.Bottom(); break;
        case wxLEFT:   pinfo.Left(); break;
        case wxRIGHT:  pinfo.Right(); break;
        case wxCENTER: pinfo.CenterPane(); break;
    }
    return AddPane(window, pinfo);
}

bool wxAuiManager::AddPane(wxWindow* window,
                           const wxAuiPaneInfo& paneInfo,
                           const wxPoint& drop_pos)
{
    if (!AddPane(window, paneInfo))
        return false;

    wxAuiPaneInfo& pane = GetPane(window);

    DoDrop(m_docks, m_panes, pane, drop_pos, wxPoint(0,0));

    return true;
}

bool wxAuiManager::InsertPane(wxWindow* window, const wxAuiPaneInfo& paneInfo,
                                int insert_level)
{
    wxASSERT_MSG(window, wxT("null window ptrs are not allowed"));

    // shift the panes around, depending on the insert level
    switch (insert_level)
    {
        case wxAUI_INSERT_PANE:
            DoInsertPane(m_panes,
                 paneInfo.dock_direction,
                 paneInfo.dock_layer,
                 paneInfo.dock_row,
                 paneInfo.dock_pos);
            break;
        case wxAUI_INSERT_ROW:
            DoInsertDockRow(m_panes,
                 paneInfo.dock_direction,
                 paneInfo.dock_layer,
                 paneInfo.dock_row);
            break;
        case wxAUI_INSERT_DOCK:
            DoInsertDockLayer(m_panes,
                 paneInfo.dock_direction,
                 paneInfo.dock_layer);
            break;
    }

    // if the window already exists, we are basically just moving/inserting the
    // existing window.  If it doesn't exist, we need to add it and insert it
    wxAuiPaneInfo& existing_pane = GetPane(window);
    if (!existing_pane.IsOk())
    {
        return AddPane(window, paneInfo);
    }
    else
    {
        if (paneInfo.IsFloating())
        {
            existing_pane.Float();
            if (paneInfo.floating_pos != wxDefaultPosition)
                existing_pane.FloatingPosition(paneInfo.floating_pos);
            if (paneInfo.floating_size != wxDefaultSize)
                existing_pane.FloatingSize(paneInfo.floating_size);
            if (paneInfo.floating_client_size != wxDefaultSize)
                existing_pane.FloatingClientSize(paneInfo.floating_client_size);
        }
        else
        {
            // if the new pane is docked then we should undo maximize
            RestoreMaximizedPane();

            existing_pane.Direction(paneInfo.dock_direction);
            existing_pane.Layer(paneInfo.dock_layer);
            existing_pane.Row(paneInfo.dock_row);
            existing_pane.Position(paneInfo.dock_pos);
        }
    }

    return true;
}


// DetachPane() removes a pane from the frame manager.  This
// method will not destroy the window that is removed.
bool wxAuiManager::DetachPane(wxWindow* window)
{
    wxASSERT_MSG(window, wxT("null window ptrs are not allowed"));

    int i, count;
    for (i = 0, count = m_panes.GetCount(); i < count; ++i)
    {
        wxAuiPaneInfo& p = m_panes.Item(i);
        if (p.window == window)
        {
            if (p.frame)
            {
                // we have a floating frame which is being detached. We need to
                // reparent it to m_frame and destroy the floating frame

                // reduce flicker
                p.window->SetSize(1,1);

                if (p.frame->IsShown())
                    p.frame->Show(false);

                // reparent to m_frame and destroy the pane
                if (m_actionWindow == p.frame)
                {
                    m_actionWindow = nullptr;
                }

                p.window->Reparent(m_frame);
                p.frame->SetSizer(nullptr);
                p.frame->Destroy();
                p.frame = nullptr;
            }

            // make sure there are no references to this pane in our uiparts,
            // just in case the caller doesn't call Update() immediately after
            // the DetachPane() call.  This prevets obscure crashes which would
            // happen at window repaint if the caller forgets to call Update()
            int actionPartIndex = GetActionPartIndex();
            int pi, part_count;
            for (pi = 0, part_count = (int)m_uiParts.GetCount(); pi < part_count; ++pi)
            {
                wxAuiDockUIPart& part = m_uiParts.Item(pi);
                if (part.pane == &p)
                {
                    if (actionPartIndex != wxNOT_FOUND)
                    {
                        if (pi == actionPartIndex)
                        {
                            // We're removing the action part, so invalidate it.
                            actionPartIndex = wxNOT_FOUND;
                        }
                        else if (pi < actionPartIndex)
                        {
                            // Just adjust the action part index.
                            actionPartIndex--;
                        }
                    }

                    m_uiParts.RemoveAt(pi);
                    part_count--;
                    pi--;
                    continue;
                }
            }

            // Update m_actionPart pointer too to ensure that it remains valid.
            m_actionPart = actionPartIndex == wxNOT_FOUND
                            ? nullptr
                            : &m_uiParts.Item(actionPartIndex);

            m_panes.RemoveAt(i);
            return true;
        }
    }
    return false;
}

// ClosePane() destroys or hides the pane depending on its flags
void wxAuiManager::ClosePane(wxAuiPaneInfo& paneInfo)
{
    // if we were maximized, restore
    if (paneInfo.IsMaximized())
    {
        RestorePane(paneInfo);
    }

    // first, hide the window
    if (paneInfo.window && paneInfo.window->IsShown())
    {
        paneInfo.window->Show(false);
    }

    // make sure that we are the parent of this window
    if (paneInfo.window && paneInfo.window->GetParent() != m_frame)
    {
        paneInfo.window->Reparent(m_frame);
    }

    // if we have a frame, destroy it
    if (paneInfo.frame)
    {
        paneInfo.frame->Destroy();
        paneInfo.frame = nullptr;
    }

    // now we need to either destroy or hide the pane
    if (paneInfo.IsDestroyOnClose())
    {
        wxWindow * window = paneInfo.window;
        DetachPane(window);
        if (window)
        {
            window->Destroy();
        }
    }
    else
    {
        paneInfo.Hide();
    }
}

void wxAuiManager::MaximizePane(wxAuiPaneInfo& paneInfo)
{
    // un-maximize and hide all other panes
    for ( auto& p : m_panes )
    {
        if (!p.IsToolbar() && !p.IsFloating())
        {
            p.Restore();

            // save hidden state
            p.SetFlag(wxAuiPaneInfo::savedHiddenState,
                      p.HasFlag(wxAuiPaneInfo::optionHidden));

            // hide the pane, because only the newly
            // maximized pane should show
            p.Hide();
        }
    }

    // mark ourselves maximized
    paneInfo.Maximize();
    paneInfo.Show();
    m_hasMaximized = true;

    // last, show the window
    if (paneInfo.window && !paneInfo.window->IsShown())
    {
        paneInfo.window->Show(true);
    }
}

void wxAuiManager::RestorePane(wxAuiPaneInfo& paneInfo)
{
    // restore all the panes
    for ( auto& p : m_panes )
    {
        if (!p.IsToolbar() && !p.IsFloating())
        {
            p.SetFlag(wxAuiPaneInfo::optionHidden,
                      p.HasFlag(wxAuiPaneInfo::savedHiddenState));
        }
    }

    // mark ourselves non-maximized
    paneInfo.Restore();
    m_hasMaximized = false;

    // last, show the window
    if (paneInfo.window && !paneInfo.window->IsShown())
    {
        paneInfo.window->Show(true);
    }
}

void wxAuiManager::RestoreMaximizedPane()
{
    // restore all the panes
    for ( auto& p : m_panes )
    {
        if (p.IsMaximized())
        {
            RestorePane(p);
            break;
        }
    }
}

// EscapeDelimiters() changes ";" into "\;" and "|" into "\|"
// in the input string.  This is an internal functions which is
// used for saving perspectives
static wxString EscapeDelimiters(const wxString& s)
{
    wxString result;
    result.Alloc(s.length());
    const wxChar* ch = s.c_str();
    while (*ch)
    {
        if (*ch == wxT(';') || *ch == wxT('|'))
            result += wxT('\\');
        result += *ch;
        ++ch;
    }
    return result;
}

wxString wxAuiManager::SavePaneInfo(const wxAuiPaneInfo& pane)
{
    wxString result = wxT("name=");
    result += EscapeDelimiters(pane.name);
    result += wxT(";");

    result += wxT("caption=");
    result += EscapeDelimiters(pane.caption);
    result += wxT(";");

    result += wxString::Format(wxT("state=%u;"), pane.state);
    result += wxString::Format(wxT("dir=%d;"), pane.dock_direction);
    result += wxString::Format(wxT("layer=%d;"), pane.dock_layer);
    result += wxString::Format(wxT("row=%d;"), pane.dock_row);
    result += wxString::Format(wxT("pos=%d;"), pane.dock_pos);
    result += wxString::Format(wxT("prop=%d;"), pane.dock_proportion);
    result += wxString::Format(wxT("bestw=%d;"), pane.best_size.x);
    result += wxString::Format(wxT("besth=%d;"), pane.best_size.y);
    result += wxString::Format(wxT("minw=%d;"), pane.min_size.x);
    result += wxString::Format(wxT("minh=%d;"), pane.min_size.y);
    result += wxString::Format(wxT("maxw=%d;"), pane.max_size.x);
    result += wxString::Format(wxT("maxh=%d;"), pane.max_size.y);
    result += wxString::Format(wxT("floatx=%d;"), pane.floating_pos.x);
    result += wxString::Format(wxT("floaty=%d;"), pane.floating_pos.y);
    result += wxString::Format(wxT("floatw=%d;"), pane.floating_size.x);
    result += wxString::Format(wxT("floath=%d;"), pane.floating_size.y);
    result += wxString::Format(wxT("floatw_cli=%d;"), pane.floating_client_size.x);
    result += wxString::Format(wxT("floath_cli=%d"), pane.floating_client_size.y);

    return result;
}

// Load a "pane" with the pane information settings in pane_part
void wxAuiManager::LoadPaneInfo(wxString pane_part, wxAuiPaneInfo &pane)
{
    // For backward compatibility, this function needs to handle
    // both layout2 and layout3.  However, layout3 is a superset
    // of layout2, so we don't need to actually check both
    LoadPaneInfoVersioned("layout3", pane_part, pane);
}

bool wxAuiManager::LoadPaneInfoVersioned(wxString layoutVersion, wxString pane_part, wxAuiPaneInfo& destination)
{
    // don't overwrite destination unless pane_part is valid
    wxAuiPaneInfo pane(destination);
    // replace escaped characters so we can
    // split up the string easily
    pane_part.Replace(wxT("\\|"), wxT("\a"));
    pane_part.Replace(wxT("\\;"), wxT("\b"));

    while(1)
    {
        wxString val_part = pane_part.BeforeFirst(wxT(';'));
        pane_part = pane_part.AfterFirst(wxT(';'));
        wxString val_name = val_part.BeforeFirst(wxT('='));
        wxString value = val_part.AfterFirst(wxT('='));
        val_name.MakeLower();
        val_name.Trim(true);
        val_name.Trim(false);
        value.Trim(true);
        value.Trim(false);

        if (val_name.empty())
            break;

        if (val_name == wxT("name"))
            pane.name = value;
        else if (val_name == wxT("caption"))
            pane.caption = value;
        else if (val_name == wxT("state"))
            pane.state = (unsigned int)wxAtoi(value.c_str());
        else if (val_name == wxT("dir"))
            pane.dock_direction = wxAtoi(value.c_str());
        else if (val_name == wxT("layer"))
            pane.dock_layer = wxAtoi(value.c_str());
        else if (val_name == wxT("row"))
            pane.dock_row = wxAtoi(value.c_str());
        else if (val_name == wxT("pos"))
            pane.dock_pos = wxAtoi(value.c_str());
        else if (val_name == wxT("prop"))
            pane.dock_proportion = wxAtoi(value.c_str());
        else if (val_name == wxT("bestw"))
            pane.best_size.x = wxAtoi(value.c_str());
        else if (val_name == wxT("besth"))
            pane.best_size.y = wxAtoi(value.c_str());
        else if (val_name == wxT("minw"))
            pane.min_size.x = wxAtoi(value.c_str());
        else if (val_name == wxT("minh"))
            pane.min_size.y = wxAtoi(value.c_str());
        else if (val_name == wxT("maxw"))
            pane.max_size.x = wxAtoi(value.c_str());
        else if (val_name == wxT("maxh"))
            pane.max_size.y = wxAtoi(value.c_str());
        else if (val_name == wxT("floatx"))
            pane.floating_pos.x = wxAtoi(value.c_str());
        else if (val_name == wxT("floaty"))
            pane.floating_pos.y = wxAtoi(value.c_str());
        else if (val_name == wxT("floatw"))
            pane.floating_size.x = wxAtoi(value.c_str());
        else if (val_name == wxT("floath"))
            pane.floating_size.y = wxAtoi(value.c_str());
        else if (val_name == wxT("floatw_cli") && layoutVersion == "layout3")
            pane.floating_client_size.x = wxAtoi(value.c_str());
        else if (val_name == wxT("floath_cli") && layoutVersion == "layout3")
            pane.floating_client_size.y = wxAtoi(value.c_str());
        else {
            return false;
        }
    }

    // replace escaped characters so we can
    // split up the string easily
    pane.name.Replace(wxT("\a"), wxT("|"));
    pane.name.Replace(wxT("\b"), wxT(";"));
    pane.caption.Replace(wxT("\a"), wxT("|"));
    pane.caption.Replace(wxT("\b"), wxT(";"));
    pane_part.Replace(wxT("\a"), wxT("|"));
    pane_part.Replace(wxT("\b"), wxT(";"));

    destination = pane;
    return true;
}


// SavePerspective() saves all pane information as a single string.
// This string may later be fed into LoadPerspective() to restore
// all pane settings.  This save and load mechanism allows an
// exact pane configuration to be saved and restored at a later time

wxString wxAuiManager::SavePerspective()
{
    wxString result;
    result.Alloc(500);
    result = wxT("layout3|");

    for ( const auto& pane : m_panes )
    {
        result += SavePaneInfo(pane)+wxT("|");
    }

    for ( const auto& dock : m_docks )
    {
        result += wxString::Format(wxT("dock_size(%d,%d,%d)=%d|"),
                                   dock.dock_direction, dock.dock_layer,
                                   dock.dock_row, dock.size);
    }

    return result;
}

// LoadPerspective() loads a layout which was saved with SavePerspective()
// If the "update" flag parameter is true, the GUI will immediately be updated

bool wxAuiManager::LoadPerspective(const wxString& layout, bool update)
{
    wxString input = layout;
    wxString layoutVersion;

    // check layout string version
    //    'layout1' = wxAUI 0.9.0 - wxAUI 0.9.2
    //    'layout2' = wxAUI 0.9.2 (wxWidgets 2.8)
    //    'layout3' = wxWidgets 3.3.1
    layoutVersion = input.BeforeFirst(wxT('|'));
    input = input.AfterFirst(wxT('|'));
    layoutVersion.Trim(true);
    layoutVersion.Trim(false);
    if (layoutVersion != wxT("layout2") &&
        layoutVersion != wxT("layout3"))
        return false;

    // Mark all panes currently managed as hidden. Also, dock all panes that are dockable.
    for ( auto& p : m_panes )
    {
        if(p.IsDockable())
            p.Dock();
        p.Hide();
    }

    // clear out the dock array; this will be reconstructed
    m_docks.Clear();

    // replace escaped characters so we can
    // split up the string easily
    input.Replace(wxT("\\|"), wxT("\a"));
    input.Replace(wxT("\\;"), wxT("\b"));

    m_hasMaximized = false;
    while (1)
    {
        wxAuiPaneInfo pane;

        wxString pane_part = input.BeforeFirst(wxT('|'));
        input = input.AfterFirst(wxT('|'));
        pane_part.Trim(true);

        // if the string is empty, we're done parsing
        if (pane_part.empty())
            break;

        if (pane_part.Left(9) == wxT("dock_size"))
        {
            wxString val_name = pane_part.BeforeFirst(wxT('='));
            wxString value = pane_part.AfterFirst(wxT('='));

            long dir, layer, row, size;
            wxString piece = val_name.AfterFirst(wxT('('));
            piece = piece.BeforeLast(wxT(')'));
            piece.BeforeFirst(wxT(',')).ToLong(&dir);
            piece = piece.AfterFirst(wxT(','));
            piece.BeforeFirst(wxT(',')).ToLong(&layer);
            piece.AfterFirst(wxT(',')).ToLong(&row);
            value.ToLong(&size);

            wxAuiDockInfo dock;
            dock.dock_direction = dir;
            dock.dock_layer = layer;
            dock.dock_row = row;
            dock.size = size;
            m_docks.Add(dock);
            continue;
        }

        // Undo our escaping as LoadPaneInfo needs to take an unescaped
        // name so it can be called by external callers
        pane_part.Replace(wxT("\a"), wxT("|"));
        pane_part.Replace(wxT("\b"), wxT(";"));

        if (!LoadPaneInfoVersioned(layoutVersion, pane_part, pane))
        {
            return false;
        }

        if ( pane.IsMaximized() )
            m_hasMaximized = true;

        wxAuiPaneInfo& p = GetPane(pane.name);
        if (!p.IsOk())
        {
            // the pane window couldn't be found
            // in the existing layout -- skip it
            continue;
        }

        p.SafeSet(pane);
    }

    if (update)
        Update();

    return true;
}

// These helper functions are used by SaveLayout() and LoadLayout() below, as
// we save the panes and docks geometries using DIPs on all platforms in order
// to ensure that they're restored correctly if the display DPI changes between
// saving and restoring the layout even on the platforms not using DIPs.
namespace
{

void MakeDIP(wxWindow* w, wxPoint& pos)
{
    pos = w->ToDIP(pos);
}

void MakeDIP(wxWindow* w, wxSize& size)
{
    size = w->ToDIP(size);
}

void MakeLogical(wxWindow* w, wxPoint& pos)
{
    pos = w->FromDIP(pos);
}

void MakeLogical(wxWindow* w, wxSize& size)
{
    size = w->FromDIP(size);
}

} // anonymous namespace

// Copy pane layout information between structs used when (de)serializing the
// layout and wxAuiPaneInfo itself.

void
wxAuiManager::CopyDockLayoutFrom(wxAuiDockLayoutInfo& dockInfo,
                                 const wxAuiPaneInfo& paneInfo) const
{
    dockInfo.dock_direction = paneInfo.dock_direction;
    dockInfo.dock_layer = paneInfo.dock_layer;
    dockInfo.dock_row = paneInfo.dock_row;
    dockInfo.dock_pos = paneInfo.dock_pos;
    dockInfo.dock_proportion = paneInfo.dock_proportion;

    // Storing the default proportion is not really useful and it looks weird
    // as it's an arbitrary huge number, so replace it with 0 in serialized
    // representation, it will be mapped back to maxDockProportion after load.
    if ( dockInfo.dock_proportion == maxDockProportion )
        dockInfo.dock_proportion = 0;

    // The dock size is typically not set in the pane itself, but set in its
    // containing dock, so find it and copy it from there, as we do need to
    // save it when serializing.
    dockInfo.dock_size = 0;
    for ( const auto& d : m_docks )
    {
        if ( FindPaneInDock(d, paneInfo.window) )
        {
            dockInfo.dock_size = d.size;
            break;
        }
    }
}

void
wxAuiManager::CopyDockLayoutTo(const wxAuiDockLayoutInfo& dockInfo,
                               wxAuiPaneInfo& paneInfo) const
{
    paneInfo.dock_direction = dockInfo.dock_direction;
    paneInfo.dock_layer = dockInfo.dock_layer;
    paneInfo.dock_row = dockInfo.dock_row;
    paneInfo.dock_pos = dockInfo.dock_pos;
    paneInfo.dock_proportion = dockInfo.dock_proportion;
    paneInfo.dock_size = dockInfo.dock_size;

    // Undo the transformation done in CopyDockLayoutFrom() above.
    if ( dockInfo.dock_proportion == 0 )
        paneInfo.dock_proportion = maxDockProportion;
}

void
wxAuiManager::CopyLayoutFrom(wxAuiPaneLayoutInfo& layoutInfo,
                             const wxAuiPaneInfo& pane) const
{
    CopyDockLayoutFrom(layoutInfo, pane);

    layoutInfo.floating_pos = pane.floating_pos;
    layoutInfo.floating_size = pane.floating_size;
    layoutInfo.floating_client_size = pane.floating_client_size;

    layoutInfo.is_maximized = pane.HasFlag(wxAuiPaneInfo::optionMaximized);
    layoutInfo.is_hidden = pane.HasFlag(wxAuiPaneInfo::optionHidden);
}

void
wxAuiManager::CopyLayoutTo(const wxAuiPaneLayoutInfo& layoutInfo,
                           wxAuiPaneInfo& pane) const
{
    CopyDockLayoutTo(layoutInfo, pane);

    pane.floating_pos = layoutInfo.floating_pos;
    pane.floating_size = layoutInfo.floating_size;
    pane.floating_client_size = layoutInfo.floating_client_size;

    pane.SetFlag(wxAuiPaneInfo::optionMaximized, layoutInfo.is_maximized);
    pane.SetFlag(wxAuiPaneInfo::optionHidden, layoutInfo.is_hidden);
}

void wxAuiManager::SaveLayout(wxAuiSerializer& serializer) const
{
    serializer.BeforeSave();

    if ( !m_panes.empty() )
    {
        serializer.BeforeSavePanes();

        // Collect information about all the notebooks we may have while saving
        // the panes layout.
        std::map<wxString, wxAuiNotebook*> notebooks;

        for ( const auto& pane : m_panes )
        {
            wxAuiPaneLayoutInfo layoutInfo{pane.name};
            CopyLayoutFrom(layoutInfo, pane);

            MakeDIP(m_frame, layoutInfo.floating_pos);
            MakeDIP(m_frame, layoutInfo.floating_size);
            MakeDIP(m_frame, layoutInfo.floating_client_size);

            serializer.SavePane(layoutInfo);

            if ( auto* const nb = wxDynamicCast(pane.window, wxAuiNotebook) )
            {
                notebooks[pane.name] = nb;
            }
        }

        serializer.AfterSavePanes();

        if ( !notebooks.empty() )
        {
            serializer.BeforeSaveNotebooks();

            for ( const auto& kv : notebooks )
            {
                kv.second->SaveLayout(kv.first, serializer);
            }

            serializer.AfterSaveNotebooks();
        }
    }

    serializer.AfterSave();
}

void wxAuiManager::LoadLayout(wxAuiDeserializer& deserializer)
{
    deserializer.BeforeLoad();

    // This will be non-empty only if we have a maximized pane.
    wxString maximizedPaneName;

    // Also keep local variables for the existing (and possibly updated) panes
    // and the new ones for the same reason.
    wxAuiPaneInfoArray panes = m_panes;

    struct NewPane
    {
        // In C++11 this ctor is required.
        NewPane(wxWindow* window_, const wxAuiPaneInfo& info_)
            : window(window_), info(info_)
        {
        }

        wxWindow* window = nullptr;
        wxAuiPaneInfo info;
    };
    std::vector<NewPane> newPanes;

    auto layoutInfos = deserializer.LoadPanes();
    for ( auto& layoutInfo : layoutInfos )
    {
        MakeLogical(m_frame, layoutInfo.floating_pos);
        MakeLogical(m_frame, layoutInfo.floating_size);
        MakeLogical(m_frame, layoutInfo.floating_client_size);

        // Find the pane with the same name in the existing layout.
        wxWindow* window = nullptr;
        for ( auto& existingPane : panes )
        {
            if ( existingPane.name == layoutInfo.name )
            {
                // Update the existing pane with the restored layout.
                CopyLayoutTo(layoutInfo, existingPane);

                if ( layoutInfo.is_maximized )
                    maximizedPaneName = existingPane.name;

                window = existingPane.window;
                break;
            }
        }

        // This pane couldn't be found in the existing layout, let deserializer
        // create a new window for it if desired, otherwise just ignore it.
        if ( !window )
        {
            wxAuiPaneInfo pane;
            pane.name = layoutInfo.name;
            CopyLayoutTo(layoutInfo, pane);

            window = deserializer.CreatePaneWindow(pane);
            if ( !window )
                continue;

            newPanes.emplace_back(window, pane);

            if ( layoutInfo.is_maximized )
                maximizedPaneName = pane.name;
        }

        if ( auto* const nb = wxDynamicCast(window, wxAuiNotebook) )
        {
            nb->LoadLayout(layoutInfo.name, deserializer);
        }
    }

    // After loading everything successfully, do update the internal variables.
    m_panes.swap(panes);

    for ( const auto& newPane : newPanes )
        AddPane(newPane.window, newPane.info);

    if ( !maximizedPaneName.empty() )
        MaximizePane(GetPane(maximizedPaneName));

    // Force recreating the docks using the new sizes from the panes.
    m_docks.clear();

    deserializer.AfterLoad();
}

void wxAuiManager::GetPanePositionsAndSizes(wxAuiDockInfo& dock,
                                            wxArrayInt& positions,
                                            wxArrayInt& sizes)
{
    positions.Empty();
    sizes.Empty();

    int action_pane = -1;
    int pane_i, pane_count = dock.panes.GetCount();

    // find the pane marked as our action pane
    for (pane_i = 0; pane_i < pane_count; ++pane_i)
    {
        wxAuiPaneInfo& pane = *(dock.panes.Item(pane_i));

        if (pane.HasFlag(wxAuiPaneInfo::actionPane))
        {
            wxASSERT_MSG(action_pane==-1, wxT("Too many fixed action panes"));
            action_pane = pane_i;
        }
    }

    // set up each panes default position, and
    // determine the size (width or height, depending
    // on the dock's orientation) of each pane
    for ( auto* p : dock.panes )
    {
        wxAuiPaneInfo& pane = *p;
        int caption_size = m_art->GetMetricForWindow(wxAUI_DOCKART_CAPTION_SIZE, pane.window);
        int pane_borderSize = m_art->GetMetricForWindow(wxAUI_DOCKART_PANE_BORDER_SIZE, pane.window);
        int gripperSize = m_art->GetMetricForWindow(wxAUI_DOCKART_GRIPPER_SIZE, pane.window);

        positions.Add(pane.dock_pos);
        int size = 0;

        if (pane.HasBorder())
            size += (pane_borderSize*2);

        if (dock.IsHorizontal())
        {
            if (pane.HasGripper() && !pane.HasGripperTop())
                size += gripperSize;
            size += pane.best_size.x;
        }
        else
        {
            if (pane.HasGripper() && pane.HasGripperTop())
                size += gripperSize;

            if (pane.HasCaption())
                size += caption_size;
            size += pane.best_size.y;
        }

        sizes.Add(size);
    }

    // if there is no action pane, just return the default
    // positions (as specified in pane.pane_pos)
    if (action_pane == -1)
        return;

    for (pane_i = action_pane-1; pane_i >= 0; --pane_i)
    {
        int amount = positions[pane_i+1] - (positions[pane_i] + sizes[pane_i]);

        if (amount >= 0)
            ;
        else
            positions[pane_i] -= -amount;
    }

    // if the dock mode is fixed, make sure none of the panes
    // overlap; we will bump panes that overlap
    int offset = 0;
    for (pane_i = action_pane; pane_i < pane_count; ++pane_i)
    {
        int amount = positions[pane_i] - offset;
        if (amount >= 0)
            offset += amount;
        else
            positions[pane_i] += -amount;

        offset += sizes[pane_i];
    }
}


void wxAuiManager::LayoutAddPane(wxSizer* cont,
                                 wxAuiDockInfo& dock,
                                 wxAuiPaneInfo& pane,
                                 wxAuiDockUIPartArray& uiparts,
                                 bool spacer_only)
{
    wxAuiDockUIPart part;
    wxSizerItem* sizer_item;

    int caption_size = m_art->GetMetricForWindow(wxAUI_DOCKART_CAPTION_SIZE, pane.window);
    int gripperSize = m_art->GetMetricForWindow(wxAUI_DOCKART_GRIPPER_SIZE, pane.window);
    int pane_borderSize = m_art->GetMetricForWindow(wxAUI_DOCKART_PANE_BORDER_SIZE, pane.window);
    int pane_button_size = m_art->GetMetricForWindow(wxAUI_DOCKART_PANE_BUTTON_SIZE, pane.window);

    // find out the orientation of the item (orientation for panes
    // is the same as the dock's orientation)
    int orientation;
    if (dock.IsHorizontal())
        orientation = wxHORIZONTAL;
    else
        orientation = wxVERTICAL;

    // this variable will store the proportion
    // value that the pane will receive
    int pane_proportion = pane.dock_proportion;

    wxBoxSizer* horz_pane_sizer = new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer* vert_pane_sizer = new wxBoxSizer(wxVERTICAL);

    if (pane.HasGripper())
    {
        if (pane.HasGripperTop())
            sizer_item = vert_pane_sizer ->Add(1, gripperSize, 0, wxEXPAND);
        else
            sizer_item = horz_pane_sizer ->Add(gripperSize, 1, 0, wxEXPAND);

        part.type = wxAuiDockUIPart::typeGripper;
        part.dock = &dock;
        part.pane = &pane;
        part.button = 0;
        part.orientation = orientation;
        part.cont_sizer = horz_pane_sizer;
        part.sizer_item = sizer_item;
        uiparts.Add(part);
    }

    if (pane.HasCaption())
    {
        // create the caption sizer
        wxBoxSizer* caption_sizer = new wxBoxSizer(wxHORIZONTAL);

        sizer_item = caption_sizer->Add(1, caption_size, 1, wxEXPAND);

        part.type = wxAuiDockUIPart::typeCaption;
        part.dock = &dock;
        part.pane = &pane;
        part.button = 0;
        part.orientation = orientation;
        part.cont_sizer = vert_pane_sizer;
        part.sizer_item = sizer_item;
        int caption_part_idx = uiparts.GetCount();
        uiparts.Add(part);

        // add pane buttons to the caption
        int button_count = 0;
        const int NUM_SUPPORTED_BUTTONS = 3;
        wxAuiButtonId buttons[NUM_SUPPORTED_BUTTONS] = {
            wxAUI_BUTTON_MAXIMIZE_RESTORE,
            wxAUI_BUTTON_PIN,
            wxAUI_BUTTON_CLOSE
        };
        int flags[NUM_SUPPORTED_BUTTONS] = {
            wxAuiPaneInfo::buttonMaximize,
            wxAuiPaneInfo::buttonPin,
            wxAuiPaneInfo::buttonClose
        };

        for (int i = 0; i < NUM_SUPPORTED_BUTTONS; ++i)
        {
            if (pane.HasFlag(flags[i]))
            {
                sizer_item = caption_sizer->Add(pane_button_size,
                    caption_size,
                    0, wxEXPAND);

                part.type = wxAuiDockUIPart::typePaneButton;
                part.dock = &dock;
                part.pane = &pane;
                part.button = buttons[i];
                part.orientation = orientation;
                part.cont_sizer = caption_sizer;
                part.sizer_item = sizer_item;
                uiparts.Add(part);
                button_count++;
            }
        }

        // if we have buttons, add a little space to the right
        // of them to ease visual crowding
        if (button_count >= 1)
        {
            caption_sizer->Add(m_frame->FromDIP(3),1);
        }

        // add the caption sizer
        sizer_item = vert_pane_sizer->Add(caption_sizer, 0, wxEXPAND);

        uiparts.Item(caption_part_idx).sizer_item = sizer_item;
    }

    // add the pane window itself
    if (spacer_only)
    {
        sizer_item = vert_pane_sizer->Add(1, 1, 1, wxEXPAND);
    }
    else
    {
        sizer_item = vert_pane_sizer->Add(pane.window, 1, wxEXPAND);
        // Don't do this because it breaks the pane size in floating windows
        // BIW: Right now commenting this out is causing problems with
        // an mdi client window as the center pane.
        vert_pane_sizer->SetItemMinSize(pane.window, 1, 1);
    }

    part.type = wxAuiDockUIPart::typePane;
    part.dock = &dock;
    part.pane = &pane;
    part.button = 0;
    part.orientation = orientation;
    part.cont_sizer = vert_pane_sizer;
    part.sizer_item = sizer_item;
    uiparts.Add(part);


    // determine if the pane should have a minimum size; if the pane is
    // non-resizable (fixed) then we must set a minimum size. Alternatively,
    // if the pane.min_size is set, we must use that value as well

    wxSize min_size = pane.min_size;
    if (pane.IsFixed())
    {
        if (min_size == wxDefaultSize)
        {
            min_size = pane.best_size;
            pane_proportion = 0;
        }
    }

    if (min_size != wxDefaultSize)
    {
        vert_pane_sizer->SetItemMinSize(
                        vert_pane_sizer->GetChildren().GetCount()-1,
                        min_size.x, min_size.y);
    }


    // add the vertical sizer (caption, pane window) to the
    // horizontal sizer (gripper, vertical sizer)
    horz_pane_sizer->Add(vert_pane_sizer, 1, wxEXPAND);

    // finally, add the pane sizer to the dock sizer

    if (pane.HasBorder())
    {
        // allowing space for the pane's border
        sizer_item = cont->Add(horz_pane_sizer, pane_proportion,
                               wxEXPAND | wxALL, pane_borderSize);

        part.type = wxAuiDockUIPart::typePaneBorder;
        part.dock = &dock;
        part.pane = &pane;
        part.button = 0;
        part.orientation = orientation;
        part.cont_sizer = cont;
        part.sizer_item = sizer_item;
        uiparts.Add(part);
    }
    else
    {
        cont->Add(horz_pane_sizer, pane_proportion, wxEXPAND);
    }
}

void wxAuiManager::LayoutAddDock(wxSizer* cont,
                                 wxAuiDockInfo& dock,
                                 wxAuiDockUIPartArray& uiparts,
                                 bool spacer_only)
{
    wxSizerItem* sizer_item;
    wxAuiDockUIPart part;

    int sashSize = m_art->GetMetricForWindow(wxAUI_DOCKART_SASH_SIZE, m_frame);
    int orientation = dock.IsHorizontal() ? wxHORIZONTAL : wxVERTICAL;

    // resizable bottom and right docks have a sash before them
    if (!m_hasMaximized && !dock.fixed && (dock.dock_direction == wxAUI_DOCK_BOTTOM ||
                        dock.dock_direction == wxAUI_DOCK_RIGHT))
    {
        sizer_item = cont->Add(sashSize, sashSize, 0, wxEXPAND);

        part.type = wxAuiDockUIPart::typeDockSizer;
        part.orientation = orientation;
        part.dock = &dock;
        part.pane = nullptr;
        part.button = 0;
        part.cont_sizer = cont;
        part.sizer_item = sizer_item;
        uiparts.Add(part);
    }

    // create the sizer for the dock
    wxSizer* dock_sizer = new wxBoxSizer(orientation);

    // add each pane to the dock
    bool has_maximized_pane = false;

    if (dock.fixed)
    {
        wxArrayInt pane_positions, pane_sizes;

        // figure out the real pane positions we will
        // use, without modifying the each pane's pane_pos member
        GetPanePositionsAndSizes(dock, pane_positions, pane_sizes);

        int offset = 0;
        const int pane_count = dock.panes.GetCount();
        for (int pane_i = 0; pane_i < pane_count; ++pane_i)
        {
            wxAuiPaneInfo& pane = *(dock.panes.Item(pane_i));
            int pane_pos = pane_positions.Item(pane_i);

            if (pane.IsMaximized())
                has_maximized_pane = true;


            int amount = pane_pos - offset;
            if (amount > 0)
            {
                if (dock.IsVertical())
                    sizer_item = dock_sizer->Add(1, amount, 0, wxEXPAND);
                else
                    sizer_item = dock_sizer->Add(amount, 1, 0, wxEXPAND);

                part.type = wxAuiDockUIPart::typeBackground;
                part.dock = &dock;
                part.pane = nullptr;
                part.button = 0;
                part.orientation = (orientation==wxHORIZONTAL) ? wxVERTICAL:wxHORIZONTAL;
                part.cont_sizer = dock_sizer;
                part.sizer_item = sizer_item;
                uiparts.Add(part);

                offset += amount;
            }

            LayoutAddPane(dock_sizer, dock, pane, uiparts, spacer_only);

            offset += pane_sizes.Item(pane_i);
        }

        // at the end add a very small stretchable background area
        sizer_item = dock_sizer->Add(0,0, 1, wxEXPAND);

        part.type = wxAuiDockUIPart::typeBackground;
        part.dock = &dock;
        part.pane = nullptr;
        part.button = 0;
        part.orientation = orientation;
        part.cont_sizer = dock_sizer;
        part.sizer_item = sizer_item;
        uiparts.Add(part);
    }
    else
    {
        wxAuiPaneInfo* last_pane = nullptr;
        for ( auto* p : dock.panes )
        {
            wxAuiPaneInfo& pane = *p;

            if (pane.IsMaximized())
                has_maximized_pane = true;

            // if this is not the first pane being added,
            // we need to add a pane sizer
            if (!m_hasMaximized && last_pane)
            {
                sizer_item = dock_sizer->Add(sashSize, sashSize, 0, wxEXPAND);

                part.type = wxAuiDockUIPart::typePaneSizer;
                part.dock = &dock;
                part.pane = last_pane;
                part.button = 0;
                part.orientation = (orientation==wxHORIZONTAL) ? wxVERTICAL:wxHORIZONTAL;
                part.cont_sizer = dock_sizer;
                part.sizer_item = sizer_item;
                uiparts.Add(part);
            }

            LayoutAddPane(dock_sizer, dock, pane, uiparts, spacer_only);

            last_pane = p;
        }
    }

    if (dock.dock_direction == wxAUI_DOCK_CENTER || has_maximized_pane)
        sizer_item = cont->Add(dock_sizer, 1, wxEXPAND);
    else
        sizer_item = cont->Add(dock_sizer, 0, wxEXPAND);

    part.type = wxAuiDockUIPart::typeDock;
    part.dock = &dock;
    part.pane = nullptr;
    part.button = 0;
    part.orientation = orientation;
    part.cont_sizer = cont;
    part.sizer_item = sizer_item;
    uiparts.Add(part);

    if (dock.IsHorizontal())
        cont->SetItemMinSize(dock_sizer, 0, dock.size);
    else
        cont->SetItemMinSize(dock_sizer, dock.size, 0);

    //  top and left docks have a sash after them
    if (!m_hasMaximized &&
        !dock.fixed &&
          (dock.dock_direction == wxAUI_DOCK_TOP ||
           dock.dock_direction == wxAUI_DOCK_LEFT))
    {
        sizer_item = cont->Add(sashSize, sashSize, 0, wxEXPAND);

        part.type = wxAuiDockUIPart::typeDockSizer;
        part.dock = &dock;
        part.pane = nullptr;
        part.button = 0;
        part.orientation = orientation;
        part.cont_sizer = cont;
        part.sizer_item = sizer_item;
        uiparts.Add(part);
    }
}

wxSizer* wxAuiManager::LayoutAll(wxAuiPaneInfoArray& panes,
                                 wxAuiDockInfoArray& docks,
                                 wxAuiDockUIPartArray& uiparts,
                                 bool spacer_only)
{
    int pane_borderSize = m_art->GetMetricForWindow(wxAUI_DOCKART_PANE_BORDER_SIZE, m_frame);
    int caption_size = m_art->GetMetricForWindow(wxAUI_DOCKART_CAPTION_SIZE, m_frame);
    wxSize cli_size = m_frame->GetClientSize();


    // empty all docks out
    for ( auto& dock : docks )
    {
        // empty out all panes, as they will be readded below
        dock.panes.Empty();

        if (dock.fixed)
        {
            // always reset fixed docks' sizes, because
            // the contained windows may have been resized
            dock.size = 0;
        }
    }


    // iterate through all known panes, filing each
    // of them into the appropriate dock. If the
    // pane does not exist in the dock, add it
    for ( auto& p : panes )
    {
        // find any docks with the same dock direction, dock layer, and
        // dock row as the pane we are working on
        wxAuiDockInfo* dock = nullptr;
        for ( auto dockInfo : FindDocks(docks,
                                        p.dock_direction,
                                        p.dock_layer,
                                        p.dock_row,
                                        FindDocksFlags::OnlyFirst) )
        {
            // found the right dock
            dock = dockInfo;

            // if we've just recreated it, apply the dock size possibly saved
            // in the pane to it
            if ( dock->size == 0 )
                dock->size = p.dock_size;
        }
        if ( !dock )
        {
            // dock was not found, so we need to create a new one
            wxAuiDockInfo d;
            d.dock_direction = p.dock_direction;
            d.dock_layer = p.dock_layer;
            d.dock_row = p.dock_row;
            d.size = p.dock_size;
            docks.Add(d);
            dock = &docks.Last();
        }


        if (p.IsDocked() && p.IsShown())
        {
            // remove the pane from any existing docks except this one
            RemovePaneFromDocks(docks, p, dock);

            // pane needs to be added to the dock,
            // if it doesn't already exist
            if (!FindPaneInDock(*dock, p.window))
                dock->panes.Add(&p);
        }
        else
        {
            // remove the pane from any existing docks
            RemovePaneFromDocks(docks, p);
        }

    }

    // remove any empty docks
    for (int i = docks.GetCount()-1; i >= 0; --i)
    {
        if (docks.Item(i).panes.GetCount() == 0)
            docks.RemoveAt(i);
    }

    // configure the docks further
    for ( auto& dock : docks )
    {
        int j, dock_pane_count = dock.panes.GetCount();
        // sort the dock pane array by the pane's
        // dock position (dock_pos), in ascending order
        dock.panes.Sort(PaneSortFunc);

        // for newly created docks, set up their initial size
        if (dock.size == 0)
        {
            int size = 0;

            for ( const auto* p : dock.panes )
            {
                const wxAuiPaneInfo& pane = *p;
                wxSize pane_size = pane.best_size;
                if (pane_size == wxDefaultSize)
                    pane_size = pane.min_size;
                if (pane_size == wxDefaultSize)
                    pane_size = pane.window->GetSize();

                if (dock.IsHorizontal())
                    size = wxMax(pane_size.y, size);
                else
                    size = wxMax(pane_size.x, size);
            }

            // add space for the border (two times), but only
            // if at least one pane inside the dock has a pane border
            for ( const auto* p : dock.panes )
            {
                if (p->HasBorder())
                {
                    size += (pane_borderSize*2);
                    break;
                }
            }

            // if pane is on the top or bottom, add the caption height,
            // but only if at least one pane inside the dock has a caption
            if (dock.IsHorizontal())
            {
                for ( const auto* p : dock.panes )
                {
                    if (p->HasCaption())
                    {
                        size += caption_size;
                        break;
                    }
                }
            }


            // new dock's size may not be more than the dock constraint
            // parameter specifies.  See SetDockSizeConstraint()

            int max_dock_x_size = (int)(m_dockConstraintX * ((double)cli_size.x));
            int max_dock_y_size = (int)(m_dockConstraintY * ((double)cli_size.y));

            if (dock.IsHorizontal())
                size = wxMin(size, max_dock_y_size);
            else
                size = wxMin(size, max_dock_x_size);

            // absolute minimum size for a dock is 10 pixels
            size = wxMax(size, m_frame->FromDIP(10));

            dock.size = size;
        }


        // determine the dock's minimum size
        bool plus_border = false;
        bool plus_caption = false;
        int dock_min_size = 0;
        for ( const auto* p : dock.panes )
        {
            const wxAuiPaneInfo& pane = *p;
            if (pane.min_size != wxDefaultSize)
            {
                if (pane.HasBorder())
                    plus_border = true;
                if (pane.HasCaption())
                    plus_caption = true;
                if (dock.IsHorizontal())
                {
                    if (pane.min_size.y > dock_min_size)
                        dock_min_size = pane.min_size.y;
                }
                else
                {
                    if (pane.min_size.x > dock_min_size)
                        dock_min_size = pane.min_size.x;
                }
            }
        }

        if (plus_border)
            dock_min_size += (pane_borderSize*2);
        if (plus_caption && dock.IsHorizontal())
            dock_min_size += (caption_size);

        dock.min_size = dock_min_size;


        // if the pane's current size is less than its
        // minimum, increase the dock's size to its minimum
        if (dock.size < dock.min_size)
            dock.size = dock.min_size;


        // determine the dock's mode (fixed or proportional);
        // determine whether the dock has only toolbars
        bool action_pane_marked = false;
        dock.fixed = true;
        dock.toolbar = true;
        for ( const auto* p : dock.panes )
        {
            const wxAuiPaneInfo& pane = *p;
            if (!pane.IsFixed())
                dock.fixed = false;
            if (!pane.IsToolbar())
                dock.toolbar = false;
            if (pane.HasFlag(wxAuiPaneInfo::optionDockFixed))
                dock.fixed = true;
            if (pane.HasFlag(wxAuiPaneInfo::actionPane))
                action_pane_marked = true;
        }


        // if the dock mode is proportional and not fixed-pixel,
        // reassign the dock_pos to the sequential 0, 1, 2, 3;
        // e.g. remove gaps like 1, 2, 30, 500
        if (!dock.fixed)
        {
            for (j = 0; j < dock_pane_count; ++j)
            {
                wxAuiPaneInfo& pane = *dock.panes.Item(j);
                pane.dock_pos = j;
            }
        }

        // if the dock mode is fixed, and none of the panes
        // are being moved right now, make sure the panes
        // do not overlap each other.  If they do, we will
        // adjust the positions of the panes
        if (dock.fixed && !action_pane_marked)
        {
            wxArrayInt pane_positions, pane_sizes;
            GetPanePositionsAndSizes(dock, pane_positions, pane_sizes);

            int offset = 0;
            for (j = 0; j < dock_pane_count; ++j)
            {
                wxAuiPaneInfo& pane = *(dock.panes.Item(j));
                pane.dock_pos = pane_positions[j];

                int amount = pane.dock_pos - offset;
                if (amount >= 0)
                    offset += amount;
                else
                    pane.dock_pos += -amount;

                offset += pane_sizes[j];
            }
        }
    }

    // discover the maximum dock layer
    int max_layer = 0;
    for ( const auto& dock : docks )
        max_layer = wxMax(max_layer, dock.dock_layer);


    // clear out uiparts
    uiparts.Empty();

    // create a bunch of box sizers,
    // from the innermost level outwards.
    wxSizer* cont = nullptr;
    wxSizer* middle = nullptr;

    for (int layer = 0; layer <= max_layer; ++layer)
    {
        // find any docks in this layer
        // if there aren't any, skip to the next layer
        if ( FindDocks(docks, -1, layer, -1, FindDocksFlags::OnlyFirst).IsEmpty() )
            continue;

        wxSizer* old_cont = cont;

        // create a container which will hold this layer's
        // docks (top, bottom, left, right)
        cont = new wxBoxSizer(wxVERTICAL);


        // find any top docks in this layer
        for ( auto dockInfo : FindDocks(docks, wxAUI_DOCK_TOP, layer, -1) )
        {
            LayoutAddDock(cont, *dockInfo, uiparts, spacer_only);
        }


        // fill out the middle layer (which consists
        // of left docks, content area and right docks)

        middle = new wxBoxSizer(wxHORIZONTAL);

        // find any left docks in this layer
        for ( auto dockInfo : FindDocks(docks, wxAUI_DOCK_LEFT, layer, -1) )
        {
            LayoutAddDock(middle, *dockInfo, uiparts, spacer_only);
        }

        // add content dock (or previous layer's sizer
        // to the middle
        if (!old_cont)
        {
            // find any center docks
            bool hasCenter = false;
            for ( auto dockInfo : FindDocks(docks, wxAUI_DOCK_CENTER, -1, -1) )
            {
                LayoutAddDock(middle, *dockInfo, uiparts, spacer_only);
                hasCenter = true;
            }
            if (!hasCenter && !m_hasMaximized)
            {
                // there are no center docks, add a background area
                wxSizerItem* sizer_item = middle->Add(1,1, 1, wxEXPAND);
                wxAuiDockUIPart part;
                part.type = wxAuiDockUIPart::typeBackground;
                part.pane = nullptr;
                part.dock = nullptr;
                part.button = 0;
                part.cont_sizer = middle;
                part.sizer_item = sizer_item;
                uiparts.Add(part);
            }
        }
        else
        {
            middle->Add(old_cont, 1, wxEXPAND);
        }

        // find any right docks in this layer
        for ( auto dockInfo : FindDocks(docks, wxAUI_DOCK_RIGHT, layer, -1,
                                        FindDocksFlags::ReverseOrder) )
        {
            LayoutAddDock(middle, *dockInfo, uiparts, spacer_only);
        }

        if (middle->GetChildren().GetCount() > 0)
            cont->Add(middle, 1, wxEXPAND);
        else
            delete middle;



        // find any bottom docks in this layer
        for ( auto dockInfo : FindDocks(docks, wxAUI_DOCK_BOTTOM, layer, -1,
                                        FindDocksFlags::ReverseOrder) )
        {
            LayoutAddDock(cont, *dockInfo, uiparts, spacer_only);
        }

    }

    if (!cont)
    {
        // no sizer available, because there are no docks,
        // therefore we will create a simple background area
        cont = new wxBoxSizer(wxVERTICAL);
        wxSizerItem* sizer_item = cont->Add(1,1, 1, wxEXPAND);
        wxAuiDockUIPart part;
        part.type = wxAuiDockUIPart::typeBackground;
        part.pane = nullptr;
        part.dock = nullptr;
        part.button = 0;
        part.cont_sizer = middle;
        part.sizer_item = sizer_item;
        uiparts.Add(part);
    }

    return cont;
}


// SetDockSizeConstraint() allows the dock constraints to be set.  For example,
// specifying values of 0.5, 0.5 will mean that upon dock creation, a dock may
// not be larger than half of the window's size

void wxAuiManager::SetDockSizeConstraint(double width_pct, double height_pct)
{
    m_dockConstraintX = wxMax(0.0, wxMin(1.0, width_pct));
    m_dockConstraintY = wxMax(0.0, wxMin(1.0, height_pct));
}

void wxAuiManager::GetDockSizeConstraint(double* width_pct, double* height_pct) const
{
    if (width_pct)
        *width_pct = m_dockConstraintX;

    if (height_pct)
        *height_pct = m_dockConstraintY;
}



// Update() updates the layout.  Whenever changes are made to
// one or more panes, this function should be called.  It is the
// external entry point for running the layout engine.

void wxAuiManager::Update()
{
    wxTopLevelWindow * const
        tlw = wxDynamicCast(wxGetTopLevelParent(m_frame), wxTopLevelWindow);
    if ( tlw && tlw->IsIconized() )
    {
        // We can't compute the layout correctly when the frame is minimized
        // because at least under MSW its client size is (0,0) in this case
        // but, luckily, we don't need to do it right now anyhow.
        m_updateOnRestore = true;
        return;
    }

    m_hoverButton = nullptr;
    m_actionPart = nullptr;

    wxSizer* sizer;
    int i, pane_count = m_panes.GetCount();


    // destroy floating panes which have been
    // redocked or are becoming non-floating
    for ( auto& p : m_panes )
    {
        if (!p.IsFloating() && p.frame)
        {
            // because the pane is no longer in a floating, we need to
            // reparent it to m_frame and destroy the floating frame

            // reduce flicker
            p.window->SetSize(1,1);


            // the following block is a workaround for bug #1531361
            // (see wxWidgets sourceforge page).  On wxGTK (only), when
            // a frame is shown/hidden, a move event unfortunately
            // also gets fired.  Because we may be dragging around
            // a pane, we need to cancel that action here to prevent
            // a spurious crash.
            if (m_actionWindow == p.frame)
            {
                if (wxWindow::GetCapture() == m_frame)
                    m_frame->ReleaseMouse();
                m_action = actionNone;
                m_actionWindow = nullptr;
            }

            // hide the frame
            if (p.frame->IsShown())
                p.frame->Show(false);

            // reparent to m_frame and destroy the pane
            p.window->Reparent(m_frame);
            p.frame->SetSizer(nullptr);
            p.frame->Destroy();
            p.frame = nullptr;
        }
    }

    // Disable all updates until everything can be repainted at once at the end
    // when not using live resizing.
    //
    // Note that:
    //  - This is useless under Mac, where HasLiveResize() always returns false.
    //  - This is harmful under GTK, where it results in extra flicker (sic).
    //  - This results in display artefacts when using live resizing under MSW.
    //
    // So we only do this under MSW and only when not using live resizing.
#ifdef __WXMSW__
    wxWindowUpdateLocker noUpdates;
    if (!HasLiveResize())
        noUpdates.Lock(m_frame);
#endif // __WXMSW__

    // delete old sizer first
    m_frame->SetSizer(nullptr);

    // create a layout for all of the panes
    sizer = LayoutAll(m_panes, m_docks, m_uiParts, false);

    // hide or show panes as necessary,
    // and float panes as necessary
    for ( auto& p : m_panes )
    {
        if (p.IsFloating())
        {
            if (p.frame == nullptr)
            {
                // we need to create a frame for this
                // pane, which has recently been floated
                wxAuiFloatingFrame* frame = CreateFloatingFrame(m_frame, p);

                // on MSW and Mac, if the owner desires transparent dragging, and
                // the dragging is happening right now, then the floating
                // window should have this style by default
                if (m_action == actionDragFloatingPane &&
                    (m_flags & wxAUI_MGR_TRANSPARENT_DRAG))
                        frame->SetTransparent(150);

                frame->SetPaneWindow(p);
                p.frame = frame;

                if (p.IsShown() && !frame->IsShown())
                    frame->Show();
            }
            else
            {
                // frame already exists, make sure its position
                // and size reflect the information in wxAuiPaneInfo
                // give floating_client_size precedence over floating_size
                if ((p.frame->GetPosition() != p.floating_pos) ||
                    ((p.floating_size != wxDefaultSize) && (p.frame->GetSize() != p.floating_size)) ||
                    ((p.floating_client_size != wxDefaultSize) && (p.frame->GetClientSize() != p.floating_client_size)))
                {
                    if (p.floating_client_size != wxDefaultSize)
                    {
                        p.frame->SetPosition(p.floating_pos);
                        p.frame->SetClientSize(p.floating_client_size);
                    }
                    else
                    {
                        p.frame->SetSize(p.floating_pos.x, p.floating_pos.y,
                                         p.floating_size.x, p.floating_size.y,
                                         wxSIZE_USE_EXISTING);
                    }
                /*
                    p.frame->SetSize(p.floating_pos.x, p.floating_pos.y,
                                     wxDefaultCoord, wxDefaultCoord,
                                     wxSIZE_USE_EXISTING);
                    //p.frame->Move(p.floating_pos.x, p.floating_pos.y);
                */
                }

                // update whether the pane is resizable or not
                long style = p.frame->GetWindowStyleFlag();
                if (p.IsFixed())
                    style &= ~wxRESIZE_BORDER;
                else
                    style |= wxRESIZE_BORDER;
                p.frame->SetWindowStyleFlag(style);

                if (p.frame->GetLabel() != p.caption)
                    p.frame->SetLabel(p.caption);

                if (p.frame->IsShown() != p.IsShown())
                    p.frame->Show(p.IsShown());
            }
        }
        else
        {
            if (p.window->IsShown() != p.IsShown())
                p.window->Show(p.IsShown());
        }

        // if "active panes" are no longer allowed, clear
        // any optionActive values from the pane states
        if ((m_flags & wxAUI_MGR_ALLOW_ACTIVE_PANE) == 0)
        {
            p.state &= ~wxAuiPaneInfo::optionActive;
        }
    }


    // keep track of the old window rectangles so we can
    // refresh those windows whose rect has changed
    std::vector<wxRect> old_pane_rects;
    for ( const auto& p : m_panes )
    {
        wxRect r;
        if (p.window && p.IsShown() && p.IsDocked())
            r = p.rect;

        old_pane_rects.push_back(r);
    }




    // apply the new sizer
    m_frame->SetSizer(sizer);
    m_frame->SetAutoLayout(false);
    DoFrameLayout();



    // now that the frame layout is done, we need to check
    // the new pane rectangles against the old rectangles that
    // we saved a few lines above here.  If the rectangles have
    // changed, the corresponding panes must also be updated
    for (i = 0; i < pane_count; ++i)
    {
        wxAuiPaneInfo& p = m_panes.Item(i);
        if (p.window && p.window->IsShown() && p.IsDocked())
        {
            if (p.rect != old_pane_rects[i])
            {
                p.window->Refresh();
                p.window->Update();
            }
        }
    }


    Repaint();

    // set frame's minimum size

/*
    // N.B. More work needs to be done on frame minimum sizes;
    // this is some interesting code that imposes the minimum size,
    // but we may want to include a more flexible mechanism or
    // options for multiple minimum-size modes, e.g. strict or lax
    wxSize min_size = sizer->GetMinSize();
    wxSize frame_size = m_frame->GetSize();
    wxSize client_size = m_frame->GetClientSize();

    wxSize minframe_size(min_size.x+frame_size.x-client_size.x,
                         min_size.y+frame_size.y-client_size.y );

    m_frame->SetMinSize(minframe_size);

    if (frame_size.x < minframe_size.x ||
        frame_size.y < minframe_size.y)
            sizer->Fit(m_frame);
*/
}


// DoFrameLayout() is an internal function which invokes wxSizer::Layout
// on the frame's main sizer, then measures all the various UI items
// and updates their internal rectangles.  This should always be called
// instead of calling m_frame->Layout() directly

void wxAuiManager::DoFrameLayout()
{
    m_frame->Layout();

    for ( auto& part : m_uiParts )
    {
        // get the rectangle of the UI part
        // originally, this code looked like this:
        //    part.rect = wxRect(part.sizer_item->GetPosition(),
        //                       part.sizer_item->GetSize());
        // this worked quite well, with one exception: the mdi
        // client window had a "deferred" size variable
        // that returned the wrong size.  It looks like
        // a bug in wx, because the former size of the window
        // was being returned.  So, we will retrieve the part's
        // rectangle via other means


        part.rect = part.sizer_item->GetRect();
        int flag = part.sizer_item->GetFlag();
        int border = part.sizer_item->GetBorder();
        if (flag & wxTOP)
        {
            part.rect.y -= border;
            part.rect.height += border;
        }
        if (flag & wxLEFT)
        {
            part.rect.x -= border;
            part.rect.width += border;
        }
        if (flag & wxBOTTOM)
            part.rect.height += border;
        if (flag & wxRIGHT)
            part.rect.width += border;


        if (part.type == wxAuiDockUIPart::typeDock)
            part.dock->rect = part.rect;
        if (part.type == wxAuiDockUIPart::typePane)
            part.pane->rect = part.rect;
    }
}

// GetPanePart() looks up the pane the pane border UI part (or the regular
// pane part if there is no border). This allows the caller to get the exact
// rectangle of the pane in question, including decorations like
// caption and border (if any).

wxAuiDockUIPart* wxAuiManager::GetPanePart(wxWindow* wnd)
{
    for ( auto& part : m_uiParts )
    {
        if ( (part.type == wxAuiDockUIPart::typePaneBorder ||
              part.type == wxAuiDockUIPart::typePane) &&
                part.pane && part.pane->window == wnd)
        {
            return &part;
        }
    }

    return nullptr;
}



// GetDockPixelOffset() is an internal function which returns
// a dock's offset in pixels from the left side of the window
// (for horizontal docks) or from the top of the window (for
// vertical docks).  This value is necessary for calculating
// pixel-pane/toolbar offsets when they are dragged.

int wxAuiManager::GetDockPixelOffset(wxAuiPaneInfo& test)
{
    // the only way to accurately calculate the dock's
    // offset is to actually run a theoretical layout

    wxAuiDockInfoArray docks;
    wxAuiPaneInfoArray panes;
    wxAuiDockUIPartArray uiparts;
    CopyDocksAndPanes(docks, panes, m_docks, m_panes);
    panes.Add(test);

    wxSizer* sizer = LayoutAll(panes, docks, uiparts, true);
    wxSize client_size = m_frame->GetClientSize();
    sizer->SetDimension(0, 0, client_size.x, client_size.y);
    sizer->Layout();

    for ( auto& part : uiparts )
    {
        part.rect = wxRect(part.sizer_item->GetPosition(),
                           part.sizer_item->GetSize());
        if (part.type == wxAuiDockUIPart::typeDock)
            part.dock->rect = part.rect;
    }

    delete sizer;

    for ( const auto& dock : docks )
    {
        if (test.dock_direction == dock.dock_direction &&
            test.dock_layer==dock.dock_layer && test.dock_row==dock.dock_row)
        {
            if (dock.IsVertical())
                return dock.rect.y;
            else
                return dock.rect.x;
        }
    }

    return 0;
}



// ProcessDockResult() is a utility function used by DoDrop() - it checks
// if a dock operation is allowed, the new dock position is copied into
// the target info.  If the operation was allowed, the function returns true.

bool wxAuiManager::ProcessDockResult(wxAuiPaneInfo& target,
                                     const wxAuiPaneInfo& new_pos)
{
    bool allowed = false;
    switch (new_pos.dock_direction)
    {
        case wxAUI_DOCK_TOP:    allowed = target.IsTopDockable();    break;
        case wxAUI_DOCK_BOTTOM: allowed = target.IsBottomDockable(); break;
        case wxAUI_DOCK_LEFT:   allowed = target.IsLeftDockable();   break;
        case wxAUI_DOCK_RIGHT:  allowed = target.IsRightDockable();  break;
    }

    if (allowed)
    {
        target = new_pos;
        // Should this RTTI and function call be rewritten as
        // sending a new event type to allow other window types
        // to vary size based on dock location?
        wxAuiToolBar* toolbar = wxDynamicCast(target.window, wxAuiToolBar);
        if (toolbar)
        {
            wxSize hintSize = toolbar->GetHintSize(target.dock_direction);
            if (target.best_size != hintSize)
            {
                target.best_size = hintSize;
                target.floating_size = wxDefaultSize;
                target.floating_client_size = wxDefaultSize;
            }
        }
    }

    return allowed;
}


// DoDrop() is an important function.  It basically takes a mouse position,
// and determines where the pane's new position would be.  If the pane is to be
// dropped, it performs the drop operation using the specified dock and pane
// arrays.  By specifying copied dock and pane arrays when calling, a "what-if"
// scenario can be performed, giving precise coordinates for drop hints.
// If, however, wxAuiManager:m_docks and wxAuiManager::m_panes are specified
// as parameters, the changes will be made to the main state arrays

const int auiInsertRowPixels = 10;
const int auiNewRowPixels = 40;
const int auiLayerInsertPixels = 40;
const int auiLayerInsertOffset = 5;

bool wxAuiManager::DoDrop(wxAuiDockInfoArray& docks,
                          wxAuiPaneInfoArray& panes,
                          wxAuiPaneInfo& target,
                          const wxPoint& pt,
                          const wxPoint& offset)
{
    wxSize cli_size = m_frame->GetClientSize();

    wxAuiPaneInfo drop = target;


    // The result should always be shown
    drop.Show();


    // Check to see if the pane has been dragged outside of the window
    // (or near to the outside of the window), if so, dock it along the edge


    wxSize layer_insert_offset;
    if (!drop.IsToolbar())
        layer_insert_offset = m_frame->FromDIP(wxSize(auiLayerInsertOffset, auiLayerInsertOffset));

    wxSize layer_insert_pixels = m_frame->FromDIP(wxSize(auiLayerInsertPixels, auiLayerInsertPixels));

    if (pt.x < layer_insert_offset.x &&
        pt.x > layer_insert_offset.x-layer_insert_pixels.x &&
        pt.y > 0 &&
        pt.y < cli_size.y)
    {
        int new_layer = wxMax(wxMax(GetMaxLayer(docks, wxAUI_DOCK_LEFT),
                                GetMaxLayer(docks, wxAUI_DOCK_BOTTOM)),
                                GetMaxLayer(docks, wxAUI_DOCK_TOP)) + 1;

        if (drop.IsToolbar())
            new_layer = auiToolBarLayer;

        drop.Dock().Left().
             Layer(new_layer).
             Row(0).
             Position(pt.y - GetDockPixelOffset(drop) - offset.y);
        return ProcessDockResult(target, drop);
    }
    else if (pt.y < layer_insert_offset.y &&
             pt.y > layer_insert_offset.y-layer_insert_pixels.y &&
             pt.x > 0 &&
             pt.x < cli_size.x)
    {
        int new_layer = wxMax(wxMax(GetMaxLayer(docks, wxAUI_DOCK_TOP),
                                GetMaxLayer(docks, wxAUI_DOCK_LEFT)),
                                GetMaxLayer(docks, wxAUI_DOCK_RIGHT)) + 1;

        if (drop.IsToolbar())
            new_layer = auiToolBarLayer;

        drop.Dock().Top().
             Layer(new_layer).
             Row(0).
             Position(pt.x - GetDockPixelOffset(drop) - offset.x);
        return ProcessDockResult(target, drop);
    }
    else if (pt.x >= cli_size.x - layer_insert_offset.x &&
             pt.x < cli_size.x - layer_insert_offset.x + layer_insert_pixels.x &&
             pt.y > 0 &&
             pt.y < cli_size.y)
    {
        int new_layer = wxMax(wxMax(GetMaxLayer(docks, wxAUI_DOCK_RIGHT),
                                GetMaxLayer(docks, wxAUI_DOCK_TOP)),
                                GetMaxLayer(docks, wxAUI_DOCK_BOTTOM)) + 1;

        if (drop.IsToolbar())
            new_layer = auiToolBarLayer;

        drop.Dock().Right().
             Layer(new_layer).
             Row(0).
             Position(pt.y - GetDockPixelOffset(drop) - offset.y);
        return ProcessDockResult(target, drop);
    }
    else if (pt.y >= cli_size.y - layer_insert_offset.y &&
             pt.y < cli_size.y - layer_insert_offset.y + layer_insert_pixels.y &&
             pt.x > 0 &&
             pt.x < cli_size.x)
    {
        int new_layer = wxMax( wxMax( GetMaxLayer(docks, wxAUI_DOCK_BOTTOM),
                                      GetMaxLayer(docks, wxAUI_DOCK_LEFT)),
                                      GetMaxLayer(docks, wxAUI_DOCK_RIGHT)) + 1;

        if (drop.IsToolbar())
            new_layer = auiToolBarLayer;

        drop.Dock().Bottom().
             Layer(new_layer).
             Row(0).
             Position(pt.x - GetDockPixelOffset(drop) - offset.x);
        return ProcessDockResult(target, drop);
    }


    wxAuiDockUIPart* part = HitTest(pt.x, pt.y);


    if (drop.IsToolbar())
    {
        if (!part || !part->dock)
            return false;

        // calculate the offset from where the dock begins
        // to the point where the user dropped the pane
        int dock_drop_offset = 0;
        if (part->dock->IsHorizontal())
            dock_drop_offset = pt.x - part->dock->rect.x - offset.x;
        else
            dock_drop_offset = pt.y - part->dock->rect.y - offset.y;


        // toolbars may only be moved in and to fixed-pane docks,
        // otherwise we will try to float the pane.  Also, the pane
        // should float if being dragged over center pane windows
        if (!part->dock->fixed || part->dock->dock_direction == wxAUI_DOCK_CENTER ||
            pt.x >= cli_size.x || pt.x <= 0 || pt.y >= cli_size.y || pt.y <= 0)
        {
            if ((m_flags & wxAUI_MGR_ALLOW_FLOATING) && drop.IsFloatable())
            {
                drop.Float();
            }
            else
            {
                drop.Position(pt.x - GetDockPixelOffset(drop) - offset.x);
            }
            return ProcessDockResult(target, drop);
        }

        m_lastRect = part->dock->rect;
        m_lastRect.Inflate( m_frame->FromDIP(wxSize(15, 15)) );

        drop.Dock().
             Direction(part->dock->dock_direction).
             Layer(part->dock->dock_layer).
             Row(part->dock->dock_row).
             Position(dock_drop_offset);

        if ((
            ((pt.y < part->dock->rect.y + 1) && part->dock->IsHorizontal()) ||
            ((pt.x < part->dock->rect.x + 1) && part->dock->IsVertical())
            ) && part->dock->panes.GetCount() > 1)
        {
            if ((part->dock->dock_direction == wxAUI_DOCK_TOP) ||
                (part->dock->dock_direction == wxAUI_DOCK_LEFT))
            {
                int row = drop.dock_row;
                DoInsertDockRow(panes, part->dock->dock_direction,
                                part->dock->dock_layer,
                                part->dock->dock_row);
                drop.dock_row = row;
            }
            else
            {
                DoInsertDockRow(panes, part->dock->dock_direction,
                                part->dock->dock_layer,
                                part->dock->dock_row+1);
                drop.dock_row = part->dock->dock_row+1;
            }
        }

        if ((
            ((pt.y > part->dock->rect.y + part->dock->rect.height - 2 ) && part->dock->IsHorizontal()) ||
            ((pt.x > part->dock->rect.x + part->dock->rect.width - 2 ) && part->dock->IsVertical())
            ) && part->dock->panes.GetCount() > 1)
        {
            if ((part->dock->dock_direction == wxAUI_DOCK_TOP) ||
                (part->dock->dock_direction == wxAUI_DOCK_LEFT))
            {
                DoInsertDockRow(panes, part->dock->dock_direction,
                                part->dock->dock_layer,
                                part->dock->dock_row+1);
                drop.dock_row = part->dock->dock_row+1;
            }
            else
            {
                int row = drop.dock_row;
                DoInsertDockRow(panes, part->dock->dock_direction,
                                part->dock->dock_layer,
                                part->dock->dock_row);
                drop.dock_row = row;
            }
        }

        return ProcessDockResult(target, drop);
    }




    if (!part)
        return false;

    if (part->type == wxAuiDockUIPart::typePaneBorder ||
        part->type == wxAuiDockUIPart::typeCaption ||
        part->type == wxAuiDockUIPart::typeGripper ||
        part->type == wxAuiDockUIPart::typePaneButton ||
        part->type == wxAuiDockUIPart::typePane ||
        part->type == wxAuiDockUIPart::typePaneSizer ||
        part->type == wxAuiDockUIPart::typeDockSizer ||
        part->type == wxAuiDockUIPart::typeBackground)
    {
        if (part->type == wxAuiDockUIPart::typeDockSizer)
        {
            if (part->dock->panes.GetCount() != 1)
                return false;
            part = GetPanePart(part->dock->panes.Item(0)->window);
            if (!part)
                return false;
        }



        // If a normal frame is being dragged over a toolbar, insert it
        // along the edge under the toolbar, but over all other panes.
        // (this could be done much better, but somehow factoring this
        // calculation with the one at the beginning of this function)
        if (part->dock && part->dock->toolbar)
        {
            int layer = 0;

            switch (part->dock->dock_direction)
            {
                case wxAUI_DOCK_LEFT:
                    layer = wxMax(wxMax(GetMaxLayer(docks, wxAUI_DOCK_LEFT),
                                      GetMaxLayer(docks, wxAUI_DOCK_BOTTOM)),
                                      GetMaxLayer(docks, wxAUI_DOCK_TOP));
                    break;
                case wxAUI_DOCK_TOP:
                    layer = wxMax(wxMax(GetMaxLayer(docks, wxAUI_DOCK_TOP),
                                      GetMaxLayer(docks, wxAUI_DOCK_LEFT)),
                                      GetMaxLayer(docks, wxAUI_DOCK_RIGHT));
                    break;
                case wxAUI_DOCK_RIGHT:
                    layer = wxMax(wxMax(GetMaxLayer(docks, wxAUI_DOCK_RIGHT),
                                      GetMaxLayer(docks, wxAUI_DOCK_TOP)),
                                      GetMaxLayer(docks, wxAUI_DOCK_BOTTOM));
                    break;
                case wxAUI_DOCK_BOTTOM:
                    layer = wxMax(wxMax(GetMaxLayer(docks, wxAUI_DOCK_BOTTOM),
                                      GetMaxLayer(docks, wxAUI_DOCK_LEFT)),
                                      GetMaxLayer(docks, wxAUI_DOCK_RIGHT));
                    break;
            }

            DoInsertDockRow(panes, part->dock->dock_direction,
                            layer, 0);
            drop.Dock().
                 Direction(part->dock->dock_direction).
                 Layer(layer).Row(0).Position(0);
            return ProcessDockResult(target, drop);
        }


        if (!part->pane)
            return false;

        part = GetPanePart(part->pane->window);
        if (!part)
            return false;

        bool insert_dock_row = false;
        int insert_row = part->pane->dock_row;
        int insert_dir = part->pane->dock_direction;
        int insert_layer = part->pane->dock_layer;
        wxSize insert_row_pixels = m_frame->FromDIP(wxSize(auiInsertRowPixels, auiInsertRowPixels));

        switch (part->pane->dock_direction)
        {
            case wxAUI_DOCK_TOP:
                if (pt.y >= part->rect.y &&
                    pt.y < part->rect.y+insert_row_pixels.y)
                        insert_dock_row = true;
                break;
            case wxAUI_DOCK_BOTTOM:
                if (pt.y > part->rect.y+part->rect.height-insert_row_pixels.y &&
                    pt.y <= part->rect.y + part->rect.height)
                        insert_dock_row = true;
                break;
            case wxAUI_DOCK_LEFT:
                if (pt.x >= part->rect.x &&
                    pt.x < part->rect.x+insert_row_pixels.x)
                        insert_dock_row = true;
                break;
            case wxAUI_DOCK_RIGHT:
                if (pt.x > part->rect.x+part->rect.width-insert_row_pixels.x &&
                    pt.x <= part->rect.x+part->rect.width)
                        insert_dock_row = true;
                break;
            case wxAUI_DOCK_CENTER:
            {
                // "new row pixels" will be set to the default, but
                // must never exceed 20% of the window size
                wxSize new_row_pixels = m_frame->FromDIP(wxSize(auiNewRowPixels, auiNewRowPixels));
                int new_row_pixels_x = new_row_pixels.x;
                int new_row_pixels_y = new_row_pixels.y;

                if (new_row_pixels_x > (part->rect.width*20)/100)
                    new_row_pixels_x = (part->rect.width*20)/100;

                if (new_row_pixels_y > (part->rect.height*20)/100)
                    new_row_pixels_y = (part->rect.height*20)/100;


                // determine if the mouse pointer is in a location that
                // will cause a new row to be inserted.  The hot spot positions
                // are along the borders of the center pane

                insert_layer = 0;
                insert_dock_row = true;
                const wxRect& pr = part->rect;
                if (pt.x >= pr.x && pt.x < pr.x + new_row_pixels_x)
                    insert_dir = wxAUI_DOCK_LEFT;
                else if (pt.y >= pr.y && pt.y < pr.y + new_row_pixels_y)
                    insert_dir = wxAUI_DOCK_TOP;
                else if (pt.x >= pr.x + pr.width - new_row_pixels_x &&
                         pt.x < pr.x + pr.width)
                    insert_dir = wxAUI_DOCK_RIGHT;
                else if (pt.y >= pr.y+ pr.height - new_row_pixels_y &&
                         pt.y < pr.y + pr.height)
                    insert_dir = wxAUI_DOCK_BOTTOM;
                else
                    return false;

                insert_row = GetMaxRow(panes, insert_dir, insert_layer) + 1;
            }
        }

        if (insert_dock_row)
        {
            DoInsertDockRow(panes, insert_dir, insert_layer, insert_row);
            drop.Dock().Direction(insert_dir).
                        Layer(insert_layer).
                        Row(insert_row).
                        Position(0);
            return ProcessDockResult(target, drop);
        }

        // determine the mouse offset and the pane size, both in the
        // direction of the dock itself, and perpendicular to the dock

        int mouseOffset, size;

        if (part->orientation == wxVERTICAL)
        {
            mouseOffset = pt.y - part->rect.y;
            size = part->rect.GetHeight();
        }
        else
        {
            mouseOffset = pt.x - part->rect.x;
            size = part->rect.GetWidth();
        }

        int drop_position = part->pane->dock_pos;

        // if we are in the top/left part of the pane,
        // insert the pane before the pane being hovered over
        if (mouseOffset <= size/2)
        {
            drop_position = part->pane->dock_pos;
            DoInsertPane(panes,
                         part->pane->dock_direction,
                         part->pane->dock_layer,
                         part->pane->dock_row,
                         part->pane->dock_pos);
        }

        // if we are in the bottom/right part of the pane,
        // insert the pane before the pane being hovered over
        if (mouseOffset > size/2)
        {
            drop_position = part->pane->dock_pos+1;
            DoInsertPane(panes,
                         part->pane->dock_direction,
                         part->pane->dock_layer,
                         part->pane->dock_row,
                         part->pane->dock_pos+1);
        }

        drop.Dock().
             Direction(part->dock->dock_direction).
             Layer(part->dock->dock_layer).
             Row(part->dock->dock_row).
             Position(drop_position);
        return ProcessDockResult(target, drop);
    }

    return false;
}

void wxAuiManager::OnHintFadeTimer(wxTimerEvent& WXUNUSED(event))
{
    if (m_hintFadeAmt >= m_hintFadeMax)
    {
        m_hintFadeTimer.Stop();
        Unbind(wxEVT_TIMER, &wxAuiManager::OnHintFadeTimer, this,
               m_hintFadeTimer.GetId());
        return;
    }

    m_hintFadeAmt++;

    ShowHint(m_lastHint);
}

void wxAuiManager::ShowHint(const wxRect& rectScreen)
{
    wxOverlayDC dc(m_overlay, m_frame);
    dc.Clear();

    wxRect rect = rectScreen;
    m_frame->ScreenToClient(&rect.x, &rect.y);

    wxDCClipper clip(dc, rect);

    if ( m_flags & wxAUI_MGR_RECTANGLE_HINT )
    {
        // Not using a transparent hint window...
        wxBitmap stipple = wxPaneCreateStippleBitmap();
        wxBrush brush(stipple);
        dc.SetBrush(brush);
        dc.SetPen(*wxTRANSPARENT_PEN);

        const int d = m_frame->FromDIP(5);
        const int dd = m_frame->FromDIP(10);

        dc.DrawRectangle(rect.x, rect.y, d, rect.height);
        dc.DrawRectangle(rect.x + d, rect.y, rect.width - dd, d);
        dc.DrawRectangle(rect.x + rect.width - d, rect.y, d, rect.height);
        dc.DrawRectangle(rect.x + d, rect.y + rect.height - d, rect.width - dd, d);

        return;
    }

#ifdef __WXGTK3__
    // The standard DC under wxGTK3 supports alpha drawing, whether the overlay
    // is native (Wayland) or generic (X11).
    const bool canDrawTransparentHint = true;
#else
    const bool canDrawTransparentHint = m_overlay.IsNative();
#endif

    const auto hintCol = wxSystemSettings::GetColour(wxSYS_COLOUR_HOTLIGHT);
    const unsigned char r = hintCol.GetRed();
    const unsigned char g = hintCol.GetGreen();
    const unsigned char b = hintCol.GetBlue();
    const unsigned char a = m_hintFadeAmt;

    const auto makeBrush = [this, r, g, b, a]()
    {
        return (m_flags & wxAUI_MGR_VENETIAN_BLINDS_HINT) != 0
                    ? wxCreateVenetianBlindsBitmap(r, g, b, a)
                    : wxBrush(wxColour(r, g, b, a));
    };

    if ( canDrawTransparentHint )
    {
#ifdef __WXMSW__
        m_overlay.SetOpacity(m_hintFadeAmt);
#endif // __WXMSW__

        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(makeBrush());
        dc.DrawRectangle(rect);
    }
    else
    {
        wxBitmap bmp(rect.GetSize(), 32);
        wxMemoryDC mdc(bmp);
        mdc.Blit(0, 0, rect.width, rect.height, &dc, rect.x, rect.y);
        wxRasterOperationMode logicalFunc = wxINVERT;

#if wxUSE_GRAPHICS_CONTEXT
        {
            wxGCDC gdc(mdc);
            gdc.SetPen(*wxTRANSPARENT_PEN);
            gdc.SetBrush(makeBrush());
            gdc.DrawRectangle(wxPoint(0, 0), rect.GetSize());
        }

        logicalFunc = wxCOPY;
#endif // wxUSE_GRAPHICS_CONTEXT

        dc.Blit(rect.x, rect.y, rect.width, rect.height, &mdc, 0, 0, logicalFunc);
    }

    // if we are dragging a floating pane, set the focus
    // back to that floating pane (otherwise it becomes unfocused)
    if (m_action == actionDragFloatingPane && m_actionWindow)
        m_actionWindow->SetFocus();

    if (m_hintFadeAmt != m_hintFadeMax) //  Only fade if we need to
    {
        // start fade in timer
        m_hintFadeTimer.SetOwner(this);
        m_hintFadeTimer.Start(10);
        Bind(wxEVT_TIMER, &wxAuiManager::OnHintFadeTimer, this,
             m_hintFadeTimer.GetId());
    }
}

void wxAuiManager::HideHint()
{
    m_overlay.Reset();
    m_hintFadeTimer.Stop();
    // In case this is called while a hint fade is going, we need to
    // disconnect the event handler.
    Unbind(wxEVT_TIMER, &wxAuiManager::OnHintFadeTimer, this,
           m_hintFadeTimer.GetId());

    m_lastHint = wxRect();
}


void wxAuiManager::StartPaneDrag(wxWindow* pane_window,
                                 const wxPoint& offset)
{
    wxAuiPaneInfo& pane = GetPane(pane_window);
    if (!pane.IsOk())
        return;

    if (pane.IsToolbar())
    {
        m_action = actionDragToolbarPane;
    }
    else
    {
        m_action = actionDragFloatingPane;
    }

    m_actionWindow = pane_window;
    m_actionOffset = offset;
    m_frame->CaptureMouse();

    if (pane.frame)
    {
        wxRect window_rect = pane.frame->GetRect();
        wxRect client_rect = pane.frame->GetClientRect();
        wxPoint client_pt = pane.frame->ClientToScreen(client_rect.GetTopLeft());
        wxPoint origin_pt = client_pt - window_rect.GetTopLeft();
        m_actionOffset += origin_pt;
    }
}


// CalculateHintRect() calculates the drop hint rectangle.  The method
// first calls DoDrop() to determine the exact position the pane would
// be at were if dropped.  If the pane would indeed become docked at the
// specified drop point, the rectangle hint will be returned in
// screen coordinates.  Otherwise, an empty rectangle is returned.
// |pane_window| is the window pointer of the pane being dragged, |pt| is
// the mouse position, in client coordinates.  |offset| describes the offset
// that the mouse is from the upper-left corner of the item being dragged

wxRect wxAuiManager::CalculateHintRect(wxWindow* pane_window,
                                       const wxPoint& pt,
                                       const wxPoint& offset)
{
    wxRect rect;

    // we need to paint a hint rectangle; to find out the exact hint rectangle,
    // we will create a new temporary layout and then measure the resulting
    // rectangle; we will create a copy of the docking structures (m_dock)
    // so that we don't modify the real thing on screen

    int i, pane_count;
    wxAuiDockInfoArray docks;
    wxAuiPaneInfoArray panes;
    wxAuiDockUIPartArray uiparts;
    wxAuiPaneInfo hint = GetPane(pane_window);
    hint.name = wxT("__HINT__");
    hint.PaneBorder(true);
    hint.Show();

    if (!hint.IsOk())
        return rect;

    CopyDocksAndPanes(docks, panes, m_docks, m_panes);

    // remove any pane already there which bears the same window;
    // this happens when you are moving a pane around in a dock
    for (i = 0, pane_count = panes.GetCount(); i < pane_count; ++i)
    {
        if (panes.Item(i).window == pane_window)
        {
            RemovePaneFromDocks(docks, panes.Item(i));
            panes.RemoveAt(i);
            break;
        }
    }

    // find out where the new pane would be
    if (!DoDrop(docks, panes, hint, pt, offset))
    {
        return rect;
    }

    panes.Add(hint);

    wxSizer* sizer = LayoutAll(panes, docks, uiparts, true);
    wxSize client_size = m_frame->GetClientSize();
    sizer->SetDimension(0, 0, client_size.x, client_size.y);
    sizer->Layout();

    for ( auto& part : uiparts )
    {
        if (part.type == wxAuiDockUIPart::typePaneBorder &&
            part.pane && part.pane->name == wxT("__HINT__"))
        {
            rect = wxRect(part.sizer_item->GetPosition(),
                          part.sizer_item->GetSize());
            break;
        }
    }

    delete sizer;

    if ( rect.IsEmpty() )
        return rect;

    m_frame->ClientToScreen(&rect.x, &rect.y);

    if ( m_frame->GetLayoutDirection() == wxLayout_RightToLeft )
    {
        // Mirror rectangle in RTL mode
        rect.x -= rect.GetWidth();
    }

    return rect;
}

// DrawHintRect() calculates the hint rectangle by calling
// CalculateHintRect().  If there is a rectangle, it shows it
// by calling ShowHint(), otherwise it hides any hint
// rectangle currently shown
void wxAuiManager::DrawHintRect(wxWindow* pane_window,
                                const wxPoint& pt,
                                const wxPoint& offset)
{
    UpdateHint(CalculateHintRect(pane_window, pt, offset));
}

void wxAuiManager::UpdateHint(const wxRect& rect)
{
    if (rect == m_lastHint)
        return;

    m_lastHint = rect;

    if (rect.IsEmpty())
    {
        HideHint();
    }
    else
    {
        // Decide if we want to fade in the hint and set it to the end value if
        // we don't.
        if (m_flags & wxAUI_MGR_HINT_FADE)
            m_hintFadeAmt = 0;
        else
            m_hintFadeAmt = m_hintFadeMax;

         ShowHint(rect);
     }
}

void wxAuiManager::OnFloatingPaneMoveStart(wxWindow* wnd)
{
    // try to find the pane
    wxAuiPaneInfo& pane = GetPane(wnd);
    wxASSERT_MSG(pane.IsOk(), wxT("Pane window not found"));

    if(!pane.frame)
        return;

    if (m_flags & wxAUI_MGR_TRANSPARENT_DRAG)
        pane.frame->SetTransparent(150);
}

void wxAuiManager::OnFloatingPaneMoving(wxWindow* wnd, wxDirection dir)
{
    // try to find the pane
    wxAuiPaneInfo& pane = GetPane(wnd);
    wxASSERT_MSG(pane.IsOk(), wxT("Pane window not found"));

    if(!pane.frame)
        return;

    wxPoint pt = ::wxGetMousePosition();

#if 0
    // Adapt pt to direction
    if (dir == wxNORTH)
    {
        // move to pane's upper border
        wxPoint pos( 0,0 );
        pos = wnd->ClientToScreen( pos );
        pt.y = pos.y;
        // and some more pixels for the title bar
        pt.y -= wnd->FromDIP(5);
    }
    else if (dir == wxWEST)
    {
        // move to pane's left border
        wxPoint pos( 0,0 );
        pos = wnd->ClientToScreen( pos );
        pt.x = pos.x;
    }
    else if (dir == wxEAST)
    {
        // move to pane's right border
        wxPoint pos( wnd->GetSize().x, 0 );
        pos = wnd->ClientToScreen( pos );
        pt.x = pos.x;
    }
    else if (dir == wxSOUTH)
    {
        // move to pane's bottom border
        wxPoint pos( 0, wnd->GetSize().y );
        pos = wnd->ClientToScreen( pos );
        pt.y = pos.y;
    }
#else
    wxUnusedVar(dir);
#endif

    wxPoint client_pt = m_frame->ScreenToClient(pt);

    // calculate the offset from the upper left-hand corner
    // of the frame to the mouse pointer
    wxPoint frame_pos = pane.frame->GetPosition();
    wxPoint action_offset(pt.x-frame_pos.x, pt.y-frame_pos.y);

    // no hint for toolbar floating windows
    if (pane.IsToolbar() && m_action == actionDragFloatingPane)
    {
        wxAuiDockInfoArray docks;
        wxAuiPaneInfoArray panes;
        wxAuiDockUIPartArray uiparts;
        wxAuiPaneInfo hint = pane;

        CopyDocksAndPanes(docks, panes, m_docks, m_panes);

        // find out where the new pane would be
        if (!DoDrop(docks, panes, hint, client_pt))
            return;
        if (hint.IsFloating())
            return;

        pane = hint;
        m_action = actionDragToolbarPane;
        m_actionWindow = pane.window;

        Update();

        return;
    }


    // if a key modifier is pressed while dragging the frame,
    // don't dock the window
    if (!CanDockPanel(pane))
    {
        HideHint();
        return;
    }


    DrawHintRect(wnd, client_pt, action_offset);

#ifdef __WXGTK__
    // this cleans up some screen artifacts that are caused on GTK because
    // we aren't getting the exact size of the window (see comment
    // in DrawHintRect)
    //Refresh();
#endif


    // reduces flicker
    m_frame->Update();
}

void wxAuiManager::OnFloatingPaneMoved(wxWindow* wnd, wxDirection dir)
{
    // try to find the pane
    wxAuiPaneInfo& pane = GetPane(wnd);
    wxASSERT_MSG(pane.IsOk(), wxT("Pane window not found"));

    if(!pane.frame)
        return;

    wxPoint pt = ::wxGetMousePosition();

#if 0
    // Adapt pt to direction
    if (dir == wxNORTH)
    {
        // move to pane's upper border
        wxPoint pos( 0,0 );
        pos = wnd->ClientToScreen( pos );
        pt.y = pos.y;
        // and some more pixels for the title bar
        pt.y -= wnd->FromDIP(10);
    }
    else if (dir == wxWEST)
    {
        // move to pane's left border
        wxPoint pos( 0,0 );
        pos = wnd->ClientToScreen( pos );
        pt.x = pos.x;
    }
    else if (dir == wxEAST)
    {
        // move to pane's right border
        wxPoint pos( wnd->GetSize().x, 0 );
        pos = wnd->ClientToScreen( pos );
        pt.x = pos.x;
    }
    else if (dir == wxSOUTH)
    {
        // move to pane's bottom border
        wxPoint pos( 0, wnd->GetSize().y );
        pos = wnd->ClientToScreen( pos );
        pt.y = pos.y;
    }
#else
    wxUnusedVar(dir);
#endif

    wxPoint client_pt = m_frame->ScreenToClient(pt);

    // calculate the offset from the upper left-hand corner
    // of the frame to the mouse pointer
    wxPoint frame_pos = pane.frame->GetPosition();
    wxPoint action_offset(pt.x-frame_pos.x, pt.y-frame_pos.y);

    // if a key modifier is pressed while dragging the frame,
    // don't dock the window
    if (CanDockPanel(pane))
    {
        // do the drop calculation
        DoDrop(m_docks, m_panes, pane, client_pt, action_offset);
    }

    // if the pane is still floating, update its floating
    // position (that we store)
    if (pane.IsFloating())
    {
        pane.floating_pos = pane.frame->GetPosition();

        if (m_flags & wxAUI_MGR_TRANSPARENT_DRAG)
            pane.frame->SetTransparent(255);
    }
    else if (m_hasMaximized)
    {
        RestoreMaximizedPane();
    }

    Update();

    HideHint();
}

void wxAuiManager::OnFloatingPaneResized(wxWindow* wnd, const wxRect& rect)
{
    // try to find the pane
    wxAuiPaneInfo& pane = GetPane(wnd);
    wxASSERT_MSG(pane.IsOk(), wxT("Pane window not found"));
    // if frame isn't fully set up, don't stomp on pos/size info
    if (!pane.frame)
    {
        return;
    }

    // Setting floating client size is enough, there is no need to set floating
    // size, as it won't be used if the client size is set.
    pane.FloatingClientSize(pane.frame->WindowToClientSize(rect.GetSize()));

    // the top-left position may change as well as the size
    pane.FloatingPosition(rect.x, rect.y);
}


void wxAuiManager::OnFloatingPaneClosed(wxWindow* wnd, wxCloseEvent& evt)
{
    // try to find the pane
    wxAuiPaneInfo& pane = GetPane(wnd);
    wxASSERT_MSG(pane.IsOk(), wxT("Pane window not found"));


    // fire pane close event
    wxAuiManagerEvent e(wxEVT_AUI_PANE_CLOSE);
    e.SetPane(&pane);
    e.SetCanVeto(evt.CanVeto());
    ProcessMgrEvent(e);

    if (e.GetVeto())
    {
        evt.Veto();
        return;
    }
    else
    {
        // close the pane, but check that it
        // still exists in our pane array first
        // (the event handler above might have removed it)

        wxAuiPaneInfo& check = GetPane(wnd);
        if (check.IsOk())
        {
            ClosePane(pane);
        }
    }
}



void wxAuiManager::OnFloatingPaneActivated(wxWindow* wnd)
{
    if ((GetFlags() & wxAUI_MGR_ALLOW_ACTIVE_PANE) && GetPane(wnd).IsOk())
    {
        SetActivePane(wnd);
        Repaint();
    }
}

// OnRender() draws all of the pane captions, sashes,
// backgrounds, captions, grippers, pane borders and buttons.
// It renders the entire user interface.

void wxAuiManager::OnRender(wxAuiManagerEvent& evt)
{
    // if the frame is about to be deleted, don't bother
    if (!m_frame || wxPendingDelete.Member(m_frame))
        return;

    wxDC* dc = evt.GetDC();

#ifdef __WXMAC__
    dc->Clear() ;
#endif
    for ( const auto& part : m_uiParts )
    {
        // don't draw hidden pane items or items that aren't windows
        if (part.sizer_item &&
                ((!part.sizer_item->IsWindow() &&
                  !part.sizer_item->IsSpacer() &&
                  !part.sizer_item->IsSizer()) ||
                  !part.sizer_item->IsShown() ||
                   part.rect.IsEmpty()))
            continue;

        switch (part.type)
        {
            case wxAuiDockUIPart::typeDockSizer:
            case wxAuiDockUIPart::typePaneSizer:
                m_art->DrawSash(*dc, m_frame, part.orientation, part.rect);
                break;
            case wxAuiDockUIPart::typeBackground:
                m_art->DrawBackground(*dc, m_frame, part.orientation, part.rect);
                break;
            case wxAuiDockUIPart::typeCaption:
                m_art->DrawCaption(*dc, m_frame, part.pane->caption, part.rect, *part.pane);
                break;
            case wxAuiDockUIPart::typeGripper:
                m_art->DrawGripper(*dc, m_frame, part.rect, *part.pane);
                break;
            case wxAuiDockUIPart::typePaneBorder:
                m_art->DrawBorder(*dc, m_frame, part.rect, *part.pane);
                break;
            case wxAuiDockUIPart::typePaneButton:
                m_art->DrawPaneButton(*dc, m_frame, part.button,
                        wxAUI_BUTTON_STATE_NORMAL, part.rect, *part.pane);
                break;
        }
    }
}


// Render() fire a render event, which is normally handled by
// wxAuiManager::OnRender().  This allows the render function to
// be overridden via the render event.  This can be useful for paintin
// custom graphics in the main window. Default behaviour can be
// invoked in the overridden function by calling OnRender()

void wxAuiManager::Render(wxDC* dc)
{
    wxAuiManagerEvent e(wxEVT_AUI_RENDER);
    e.SetManager(this);
    e.SetDC(dc);
    ProcessMgrEvent(e);
}

void wxAuiManager::Repaint(wxDC* dc)
{
    std::unique_ptr<wxClientDC> client_dc;

    // figure out which dc to use; if one
    // has been specified, use it, otherwise
    // make a client dc
    if (!dc)
    {
        if ( !wxClientDC::CanBeUsedForDrawing(m_frame) )
        {
            // We can't use wxClientDC in these ports.
            m_frame->Refresh() ;
            m_frame->Update() ;
            return ;
        }

        client_dc.reset(new wxClientDC(m_frame));
        dc = client_dc.get();
    }

    int w, h;
    m_frame->GetClientSize(&w, &h);

    // if the frame has a toolbar, the client area
    // origin will not be (0,0).
    wxPoint pt = m_frame->GetClientAreaOrigin();
    if (pt.x != 0 || pt.y != 0)
        dc->SetDeviceOrigin(pt.x, pt.y);

    // render all the items
    Render(dc);
}

void wxAuiManager::OnDestroy(wxWindowDestroyEvent& event)
{
    if ( event.GetEventObject() == m_frame )
    {
        wxWindow* const frame = m_frame;

        UnInit();

        // Just calling Skip() would be insufficient in this case, as by
        // removing the event handler from the event handlers chain in UnInit()
        // we'd still prevent the frame from getting this event, so we need to
        // forward it to it manually. Note that this must be done after calling
        // UnInit() to prevent infinite recursion.
        if ( frame )
            frame->ProcessWindowEventLocally(event);
    }

    // Make sure this event get's propagated to other handlers.
    event.Skip();
}

void wxAuiManager::OnPaint(wxPaintEvent& WXUNUSED(event))
{
    wxPaintDC dc(m_frame);

    dc.SetBackground(GetArtProvider()->GetColor(wxAUI_DOCKART_BACKGROUND_COLOUR));
    dc.Clear();

    Repaint(&dc);
}

void wxAuiManager::OnEraseBackground(wxEraseEvent& event)
{
#ifdef __WXMAC__
    event.Skip() ;
#else
    wxUnusedVar(event);
#endif
}

void wxAuiManager::OnSize(wxSizeEvent& event)
{
    if (m_frame)
    {
        if ( m_updateOnRestore )
        {
            // If we had postponed updating, do it now: we only receive size
            // events once the window is restored.
            m_updateOnRestore = false;

            Update();
        }
        else // Otherwise just re-layout, without redoing the full update.
        {
            DoFrameLayout();
            Repaint();
        }

#if wxUSE_MDI
        if (wxDynamicCast(m_frame, wxMDIParentFrame))
        {
            // for MDI parent frames, this event must not
            // be "skipped".  In other words, the parent frame
            // must not be allowed to resize the client window
            // after we are finished processing sizing changes
            return;
        }
#endif
    }
    event.Skip();
}

void wxAuiManager::OnFindManager(wxAuiManagerEvent& evt)
{
    // get the window we are managing, if none, return nullptr
    wxWindow* window = GetManagedWindow();
    if (!window)
    {
        evt.SetManager(nullptr);
        return;
    }

    // if we are managing a child frame, get the 'real' manager
    if (wxDynamicCast(window, wxAuiFloatingFrame))
    {
        wxAuiFloatingFrame* float_frame = static_cast<wxAuiFloatingFrame*>(window);
        evt.SetManager(float_frame->GetOwnerManager());
        return;
    }

    // return pointer to ourself
    evt.SetManager(this);
}

void wxAuiManager::OnSetCursor(wxSetCursorEvent& event)
{
    // determine cursor
    wxAuiDockUIPart* part = HitTest(event.GetX(), event.GetY());
    wxCursor cursor;

    if (part)
    {
        if (part->type == wxAuiDockUIPart::typeDockSizer ||
            part->type == wxAuiDockUIPart::typePaneSizer)
        {
            // a dock may not be resized if it has a single
            // pane which is not resizable
            if (part->type == wxAuiDockUIPart::typeDockSizer && part->dock &&
                part->dock->panes.GetCount() == 1 &&
                part->dock->panes.Item(0)->IsFixed())
                    return;

            // panes that may not be resized do not get a sizing cursor
            if (part->pane && part->pane->IsFixed())
                return;

            if (part->orientation == wxVERTICAL)
                cursor = wxCursor(wxCURSOR_SIZEWE);
            else
                cursor = wxCursor(wxCURSOR_SIZENS);
        }
        else if (part->type == wxAuiDockUIPart::typeGripper)
        {
            cursor = wxCursor(wxCURSOR_SIZING);
        }
    }

    event.SetCursor(cursor);
}



void wxAuiManager::UpdateButtonOnScreen(wxAuiDockUIPart* button_ui_part,
                                        const wxMouseEvent& event)
{
    wxAuiDockUIPart* hit_test = HitTest(event.GetX(), event.GetY());
    if (!hit_test || !button_ui_part)
        return;

    int state = wxAUI_BUTTON_STATE_NORMAL;

    if (hit_test == button_ui_part)
    {
        if (event.LeftDown())
            state = wxAUI_BUTTON_STATE_PRESSED;
        else
            state = wxAUI_BUTTON_STATE_HOVER;
    }
    else
    {
        if (event.LeftDown())
            state = wxAUI_BUTTON_STATE_HOVER;
    }

    // now repaint the button with hover state -- or everything if we can't
    // repaint just it
    if ( !wxClientDC::CanBeUsedForDrawing(m_frame) )
    {
        m_frame->Refresh();
        m_frame->Update();
    }

    wxClientDC cdc(m_frame);

    // if the frame has a toolbar, the client area
    // origin will not be (0,0).
    wxPoint pt = m_frame->GetClientAreaOrigin();
    if (pt.x != 0 || pt.y != 0)
        cdc.SetDeviceOrigin(pt.x, pt.y);

    if (hit_test->pane)
    {
        m_art->DrawPaneButton(cdc, m_frame,
                  button_ui_part->button,
                  state,
                  button_ui_part->rect,
                  *hit_test->pane);
    }
}

void wxAuiManager::OnLeftDown(wxMouseEvent& event)
{
    m_currentDragItem = -1;

    wxAuiDockUIPart* part = HitTest(event.GetX(), event.GetY());
    if (part)
    {
        if (part->type == wxAuiDockUIPart::typeDockSizer ||
            part->type == wxAuiDockUIPart::typePaneSizer)
        {
            // Removing this restriction so that a centre pane can be resized
            //if (part->dock && part->dock->dock_direction == wxAUI_DOCK_CENTER)
            //    return;

            // a dock may not be resized if it has a single
            // pane which is not resizable
            if (part->type == wxAuiDockUIPart::typeDockSizer && part->dock &&
                part->dock->panes.GetCount() == 1 &&
                part->dock->panes.Item(0)->IsFixed())
                    return;

            // panes that may not be resized should be ignored here
            if (part->pane && part->pane->IsFixed())
                return;

            m_action = actionResize;
            m_actionPart = part;
            m_actionHintRect = wxRect();
            m_actionStart = wxPoint(event.m_x, event.m_y);
            m_actionOffset = wxPoint(event.m_x - part->rect.x,
                                      event.m_y - part->rect.y);
            m_frame->CaptureMouse();
        }
        else if (part->type == wxAuiDockUIPart::typePaneButton)
        {
            m_action = actionClickButton;
            m_actionPart = part;
            m_actionStart = wxPoint(event.m_x, event.m_y);
            m_frame->CaptureMouse();

            UpdateButtonOnScreen(part, event);
        }
        else if (part->type == wxAuiDockUIPart::typeCaption ||
                  part->type == wxAuiDockUIPart::typeGripper)
        {
            // if we are managing a wxAuiFloatingFrame window, then
            // we are an embedded wxAuiManager inside the wxAuiFloatingFrame.
            // We want to initiate a toolbar drag in our owner manager
            wxWindow* managed_wnd = GetManagedWindow();

            if (part->pane &&
                part->pane->window &&
                managed_wnd &&
                wxDynamicCast(managed_wnd, wxAuiFloatingFrame))
            {
                wxAuiFloatingFrame* floating_frame = (wxAuiFloatingFrame*)managed_wnd;
                wxAuiManager* owner_mgr = floating_frame->GetOwnerManager();
                owner_mgr->StartPaneDrag(part->pane->window,
                                             wxPoint(event.m_x - part->rect.x,
                                                     event.m_y - part->rect.y));
                return;
            }



            if (GetFlags() & wxAUI_MGR_ALLOW_ACTIVE_PANE)
            {
                // set the caption as active
                SetActivePane(part->pane->window);
                Repaint();
            }

            if (part->dock && part->dock->dock_direction == wxAUI_DOCK_CENTER)
                return;

            m_action = actionClickCaption;
            m_actionPart = part;
            m_actionStart = wxPoint(event.m_x, event.m_y);
            m_actionOffset = wxPoint(event.m_x - part->rect.x,
                                      event.m_y - part->rect.y);
            m_frame->CaptureMouse();
        }
        else
        {
            event.Skip();
        }
    }
    else
    {
        event.Skip();
    }
}

/// Ends a resize action, or for live update, resizes the sash
bool wxAuiManager::DoEndResizeAction(wxMouseEvent& event)
{
    // resize the dock or the pane
    if (m_actionPart && m_actionPart->type==wxAuiDockUIPart::typeDockSizer)
    {
        // first, we must calculate the maximum size the dock may be
        int sashSize = m_art->GetMetricForWindow(wxAUI_DOCKART_SASH_SIZE, m_frame);

        int used_width = 0, used_height = 0;

        wxSize client_size = m_frame->GetClientSize();

        for ( const auto& dock : m_docks )
        {
            if (dock.dock_direction == wxAUI_DOCK_TOP ||
                dock.dock_direction == wxAUI_DOCK_BOTTOM)
            {
                used_height += dock.size;
            }
            if (dock.dock_direction == wxAUI_DOCK_LEFT ||
                dock.dock_direction == wxAUI_DOCK_RIGHT)
            {
                used_width += dock.size;
            }
            if (dock.resizable)
                used_width += sashSize;
        }


        int available_width = client_size.GetWidth() - used_width;
        int available_height = client_size.GetHeight() - used_height;


#if wxUSE_STATUSBAR
        // if there's a status control, the available
        // height decreases accordingly
        if (wxDynamicCast(m_frame, wxFrame))
        {
            wxFrame* frame = static_cast<wxFrame*>(m_frame);
            wxStatusBar* status = frame->GetStatusBar();
            if (status)
            {
                wxSize status_client_size = status->GetClientSize();
                available_height -= status_client_size.GetHeight();
            }
        }
#endif

        const wxRect& rect = m_actionPart->dock->rect;

        wxPoint new_pos(event.m_x - m_actionOffset.x,
            event.m_y - m_actionOffset.y);
        int new_size, old_size = m_actionPart->dock->size;

        switch (m_actionPart->dock->dock_direction)
        {
        case wxAUI_DOCK_LEFT:
            new_size = new_pos.x - rect.x;
            if (new_size-old_size > available_width)
                new_size = old_size+available_width;
            m_actionPart->dock->size = new_size;
            break;
        case wxAUI_DOCK_TOP:
            new_size = new_pos.y - rect.y;
            if (new_size-old_size > available_height)
                new_size = old_size+available_height;
            m_actionPart->dock->size = new_size;
            break;
        case wxAUI_DOCK_RIGHT:
            new_size = rect.x + rect.width - new_pos.x -
                       m_actionPart->rect.GetWidth();
            if (new_size-old_size > available_width)
                new_size = old_size+available_width;
            m_actionPart->dock->size = new_size;
            break;
        case wxAUI_DOCK_BOTTOM:
            new_size = rect.y + rect.height -
                new_pos.y - m_actionPart->rect.GetHeight();
            if (new_size-old_size > available_height)
                new_size = old_size+available_height;
            m_actionPart->dock->size = new_size;
            break;
        }

        Update();
    }
    else if (m_actionPart &&
        m_actionPart->type == wxAuiDockUIPart::typePaneSizer)
    {
        wxAuiDockInfo& dock = *m_actionPart->dock;
        wxAuiPaneInfo& pane = *m_actionPart->pane;

        int total_proportion = 0;
        int dock_pixels = 0;
        int new_pixsize = 0;

        int caption_size = m_art->GetMetricForWindow(wxAUI_DOCKART_CAPTION_SIZE, pane.window);
        int pane_borderSize = m_art->GetMetricForWindow(wxAUI_DOCKART_PANE_BORDER_SIZE, pane.window);
        int sashSize = m_art->GetMetricForWindow(wxAUI_DOCKART_SASH_SIZE, pane.window);

        wxPoint new_pos(event.m_x - m_actionOffset.x,
            event.m_y - m_actionOffset.y);

        // determine the pane rectangle by getting the pane part
        wxAuiDockUIPart* pane_part = GetPanePart(pane.window);
        wxASSERT_MSG(pane_part,
            wxT("Pane border part not found -- shouldn't happen"));

        // determine the new pixel size that the user wants;
        // this will help us recalculate the pane's proportion
        if (dock.IsHorizontal())
            new_pixsize = new_pos.x - pane_part->rect.x;
        else
            new_pixsize = new_pos.y - pane_part->rect.y;

        // determine the size of the dock, based on orientation
        if (dock.IsHorizontal())
            dock_pixels = dock.rect.GetWidth();
        else
            dock_pixels = dock.rect.GetHeight();

        // determine the total proportion of all resizable panes,
        // and the total size of the dock minus the size of all
        // the fixed panes
        int i, dock_pane_count = dock.panes.GetCount();
        int pane_position = -1;
        for (i = 0; i < dock_pane_count; ++i)
        {
            wxAuiPaneInfo& p = *dock.panes.Item(i);
            if (p.window == pane.window)
                pane_position = i;

            // while we're at it, subtract the pane sash
            // width from the dock width, because this would
            // skew our proportion calculations
            if (i > 0)
                dock_pixels -= sashSize;

            // also, the whole size (including decorations) of
            // all fixed panes must also be subtracted, because they
            // are not part of the proportion calculation
            if (p.IsFixed())
            {
                if (dock.IsHorizontal())
                    dock_pixels -= p.best_size.x;
                else
                    dock_pixels -= p.best_size.y;
            }
            else
            {
                total_proportion += p.dock_proportion;
            }
        }

        // new size can never be more than the number of dock pixels
        if (new_pixsize > dock_pixels)
            new_pixsize = dock_pixels;


        // find a pane in our dock to 'steal' space from or to 'give'
        // space to -- this is essentially what is done when a pane is
        // resized; the pane should usually be the first non-fixed pane
        // to the right of the action pane
        int borrow_pane = -1;
        for (i = pane_position+1; i < dock_pane_count; ++i)
        {
            wxAuiPaneInfo& p = *dock.panes.Item(i);
            if (!p.IsFixed())
            {
                borrow_pane = i;
                break;
            }
        }


        // demand that the pane being resized is found in this dock
        // (this assert really never should be raised)
        wxASSERT_MSG(pane_position != -1, wxT("Pane not found in dock"));

        // prevent division by zero
        if (dock_pixels == 0 || total_proportion == 0 || borrow_pane == -1)
        {
            m_action = actionNone;
            return false;
        }

        // calculate the new proportion of the pane
        int new_proportion = (new_pixsize*total_proportion)/dock_pixels;

        // default minimum size
        int min_size = 0;

        // check against the pane's minimum size, if specified. please note
        // that this is not enough to ensure that the minimum size will
        // not be violated, because the whole frame might later be shrunk,
        // causing the size of the pane to violate its minimum size
        if (pane.min_size.IsFullySpecified())
        {
            min_size = 0;

            if (pane.HasBorder())
                min_size += (pane_borderSize*2);

            // calculate minimum size with decorations (border,caption)
            if (pane_part->orientation == wxVERTICAL)
            {
                min_size += pane.min_size.y;
                if (pane.HasCaption())
                    min_size += caption_size;
            }
            else
            {
                min_size += pane.min_size.x;
            }
        }


        // for some reason, an arithmetic error somewhere is causing
        // the proportion calculations to always be off by 1 pixel;
        // for now we will add the 1 pixel on, but we really should
        // determine what's causing this.
        min_size++;

        int min_proportion = (min_size*total_proportion)/dock_pixels;

        if (new_proportion < min_proportion)
            new_proportion = min_proportion;



        int prop_diff = new_proportion - pane.dock_proportion;

        // borrow the space from our neighbor pane to the
        // right or bottom (depending on orientation);
        // also make sure we don't make the neighbor too small
        int prop_borrow = dock.panes.Item(borrow_pane)->dock_proportion;

        if (prop_borrow - prop_diff < 0)
        {
            // borrowing from other pane would make it too small,
            // so cancel the resize operation
            prop_borrow = min_proportion;
        }
         else
        {
            prop_borrow -= prop_diff;
        }


        dock.panes.Item(borrow_pane)->dock_proportion = prop_borrow;
        pane.dock_proportion = new_proportion;

        Update();
    }

    return true;
}

void wxAuiManager::OnLeftUp(wxMouseEvent& event)
{
    if (m_action == actionResize)
    {
        m_frame->ReleaseMouse();

        if (!HasLiveResize())
        {
            // get rid of the hint rectangle
            m_overlay.Reset();
        }
        if (m_currentDragItem != -1 && HasLiveResize())
            m_actionPart = & (m_uiParts.Item(m_currentDragItem));

        DoEndResizeAction(event);

        m_currentDragItem = -1;

    }
    else if (m_action == actionClickButton)
    {
        m_hoverButton = nullptr;
        m_frame->ReleaseMouse();

        if (m_actionPart)
        {
            UpdateButtonOnScreen(m_actionPart, event);

            // make sure we're still over the item that was originally clicked
            if (m_actionPart == HitTest(event.GetX(), event.GetY()))
            {
                // fire button-click event
                wxAuiManagerEvent e(wxEVT_AUI_PANE_BUTTON);
                e.SetManager(this);
                e.SetPane(m_actionPart->pane);
                e.SetButton(m_actionPart->button);
                ProcessMgrEvent(e);
            }
        }
    }
    else if (m_action == actionClickCaption)
    {
        m_frame->ReleaseMouse();
    }
    else if (m_action == actionDragFloatingPane)
    {
        m_frame->ReleaseMouse();
    }
    else if (m_action == actionDragToolbarPane)
    {
        m_frame->ReleaseMouse();

        wxAuiPaneInfo& pane = GetPane(m_actionWindow);
        wxASSERT_MSG(pane.IsOk(), wxT("Pane window not found"));

        // save the new positions
        for ( auto dockInfo : FindDocks(m_docks, pane.dock_direction,
                                        pane.dock_layer, pane.dock_row,
                                        FindDocksFlags::OnlyFirst) )
        {
            wxAuiDockInfo& dock = *dockInfo;

            wxArrayInt pane_positions, pane_sizes;
            GetPanePositionsAndSizes(dock, pane_positions, pane_sizes);

            int i, dock_pane_count = dock.panes.GetCount();
            for (i = 0; i < dock_pane_count; ++i)
                dock.panes.Item(i)->dock_pos = pane_positions[i];
        }

        pane.state &= ~wxAuiPaneInfo::actionPane;
        Update();
    }
    else
    {
        event.Skip();
    }

    m_action = actionNone;
    m_lastMouseMove = wxPoint(); // see comment in OnMotion()
}


void wxAuiManager::OnMotion(wxMouseEvent& event)
{
    // sometimes when Update() is called from inside this method,
    // a spurious mouse move event is generated; this check will make
    // sure that only real mouse moves will get anywhere in this method;
    // this appears to be a bug somewhere, and I don't know where the
    // mouse move event is being generated.  only verified on MSW

    wxPoint mouse_pos = event.GetPosition();
    if (m_lastMouseMove == mouse_pos)
        return;
    m_lastMouseMove = mouse_pos;


    if (m_action == actionResize)
    {
        // It's necessary to reset m_actionPart since it destroyed
        // by the Update within DoEndResizeAction.
        if (m_currentDragItem != -1)
            m_actionPart = & (m_uiParts.Item(m_currentDragItem));
        else
            m_currentDragItem = GetActionPartIndex();

        if (m_actionPart)
        {
            wxPoint pos = m_actionPart->rect.GetPosition();
            if (m_actionPart->orientation == wxHORIZONTAL)
                pos.y = wxMax(0, event.m_y - m_actionOffset.y);
            else
                pos.x = wxMax(0, event.m_x - m_actionOffset.x);

            if (HasLiveResize())
            {
                m_frame->ReleaseMouse();
                DoEndResizeAction(event);
                m_frame->CaptureMouse();
            }
            else
            {
                // draw resize hint
                m_actionHintRect = wxRect(pos, m_actionPart->rect.GetSize());
                wxDrawOverlayResizeHint(m_frame, m_overlay, m_actionHintRect);
            }
        }
    }
    else if (m_action == actionClickCaption)
    {
        int drag_x_threshold = wxSystemSettings::GetMetric(wxSYS_DRAG_X, m_frame);
        int drag_y_threshold = wxSystemSettings::GetMetric(wxSYS_DRAG_Y, m_frame);

        // caption has been clicked.  we need to check if the mouse
        // is now being dragged. if it is, we need to change the
        // mouse action to 'drag'
        if (m_actionPart &&
            (abs(event.m_x - m_actionStart.x) > drag_x_threshold ||
             abs(event.m_y - m_actionStart.y) > drag_y_threshold))
        {
            wxAuiPaneInfo* paneInfo = m_actionPart->pane;

            if (!paneInfo->IsToolbar())
            {
                if ((m_flags & wxAUI_MGR_ALLOW_FLOATING) &&
                    paneInfo->IsFloatable())
                {
                    m_action = actionDragFloatingPane;

                    // set initial float position
                    wxPoint pt = m_frame->ClientToScreen(event.GetPosition());
                    paneInfo->floating_pos = wxPoint(pt.x - m_actionOffset.x,
                                                      pt.y - m_actionOffset.y);

                    // float the window
                    if (paneInfo->IsMaximized())
                        RestorePane(*paneInfo);
                    paneInfo->Float();
                    Update();

                    m_actionWindow = paneInfo->frame;

                    // action offset is used here to make it feel "natural" to the user
                    // to drag a docked pane and suddenly have it become a floating frame.
                    // Sometimes, however, the offset where the user clicked on the docked
                    // caption is bigger than the width of the floating frame itself, so
                    // in that case we need to set the action offset to a sensible value
                    wxSize frame_size = m_actionWindow->GetSize();
                    if (frame_size.x <= m_actionOffset.x)
                        m_actionOffset.x = paneInfo->frame->FromDIP(30);
                }
            }
            else
            {
                m_action = actionDragToolbarPane;
                m_actionWindow = paneInfo->window;
            }
        }
    }
    else if (m_action == actionDragFloatingPane)
    {
        if (m_actionWindow)
        {
            // We can't move the child window so we need to get the frame that
            // we want to be really moving. This is probably not the best place
            // to do this but at least it fixes the bug (#13177) for now.
            if (!wxDynamicCast(m_actionWindow, wxAuiFloatingFrame))
            {
                wxAuiPaneInfo& pane = GetPane(m_actionWindow);
                m_actionWindow = pane.frame;
            }

            wxPoint pt = m_frame->ClientToScreen(event.GetPosition());
            m_actionWindow->Move(pt.x - m_actionOffset.x,
                                pt.y - m_actionOffset.y);
        }
    }
    else if (m_action == actionDragToolbarPane)
    {
        wxAuiPaneInfo& pane = GetPane(m_actionWindow);
        wxASSERT_MSG(pane.IsOk(), wxT("Pane window not found"));

        pane.SetFlag(wxAuiPaneInfo::actionPane, true);

        wxPoint point = event.GetPosition();
        DoDrop(m_docks, m_panes, pane, point, m_actionOffset);

        // if DoDrop() decided to float the pane, set up
        // the floating pane's initial position
        if (pane.IsFloating())
        {
            wxPoint pt = m_frame->ClientToScreen(event.GetPosition());
            pane.floating_pos = wxPoint(pt.x - m_actionOffset.x,
                                        pt.y - m_actionOffset.y);
        }

        // this will do the actual move operation;
        // in the case that the pane has been floated,
        // this call will create the floating pane
        // and do the reparenting
        Update();

        // if the pane has been floated, change the mouse
        // action actionDragFloatingPane so that subsequent
        // EVT_MOTION() events will move the floating pane
        if (pane.IsFloating())
        {
            pane.state &= ~wxAuiPaneInfo::actionPane;
            m_action = actionDragFloatingPane;
            m_actionWindow = pane.frame;
        }
    }
    else
    {
        wxAuiDockUIPart* part = HitTest(event.GetX(), event.GetY());
        if (part && part->type == wxAuiDockUIPart::typePaneButton)
        {
            if (part != m_hoverButton)
            {
                // make the old button normal
                if (m_hoverButton)
                {
                    UpdateButtonOnScreen(m_hoverButton, event);
                    Repaint();
                }

                // mouse is over a button, so repaint the
                // button in hover mode
                UpdateButtonOnScreen(part, event);
                m_hoverButton = part;

            }
        }
        else
        {
            if (m_hoverButton)
            {
                m_hoverButton = nullptr;
                Repaint();
            }
            else
            {
                event.Skip();
            }
        }
    }
}

void wxAuiManager::OnLeaveWindow(wxMouseEvent& WXUNUSED(event))
{
    if (m_hoverButton)
    {
        m_hoverButton = nullptr;
        Repaint();
    }
}

void wxAuiManager::OnCaptureLost(wxMouseCaptureLostEvent& WXUNUSED(event))
{
    // cancel the operation in progress, if any
    if ( m_action != actionNone )
    {
        m_action = actionNone;
        HideHint();
    }
}

void wxAuiManager::OnChildFocus(wxChildFocusEvent& event)
{
    // when a child pane has its focus set, we should change the
    // pane's active state to reflect this. (this is only true if
    // active panes are allowed by the owner)
    if (GetFlags() & wxAUI_MGR_ALLOW_ACTIVE_PANE)
    {
        wxAuiPaneInfo& pane = GetPane(event.GetWindow());
        if (pane.IsOk() && (pane.state & wxAuiPaneInfo::optionActive) == 0)
        {
            SetActivePane(event.GetWindow());
            m_frame->Refresh();
        }
    }

    event.Skip();
}


// OnPaneButton() is an event handler that is called
// when a pane button has been pressed.
void wxAuiManager::OnPaneButton(wxAuiManagerEvent& evt)
{
    wxASSERT_MSG(evt.pane, wxT("Pane Info passed to wxAuiManager::OnPaneButton must be non-null"));

    wxAuiPaneInfo& pane = *(evt.pane);

    if (evt.button == wxAUI_BUTTON_CLOSE)
    {
        // fire pane close event
        wxAuiManagerEvent e(wxEVT_AUI_PANE_CLOSE);
        e.SetManager(this);
        e.SetPane(evt.pane);
        ProcessMgrEvent(e);

        if (!e.GetVeto())
        {
            // close the pane, but check that it
            // still exists in our pane array first
            // (the event handler above might have removed it)

            wxAuiPaneInfo& check = GetPane(pane.window);
            if (check.IsOk())
            {
                ClosePane(pane);
            }

            Update();
        }
    }
    else if (evt.button == wxAUI_BUTTON_MAXIMIZE_RESTORE && !pane.IsMaximized())
    {
        // fire pane maximize event
        wxAuiManagerEvent e(wxEVT_AUI_PANE_MAXIMIZE);
        e.SetManager(this);
        e.SetPane(evt.pane);
        ProcessMgrEvent(e);

        if (!e.GetVeto())
        {
            MaximizePane(pane);
            Update();
        }
    }
    else if (evt.button == wxAUI_BUTTON_MAXIMIZE_RESTORE && pane.IsMaximized())
    {
        // fire pane restore event
        wxAuiManagerEvent e(wxEVT_AUI_PANE_RESTORE);
        e.SetManager(this);
        e.SetPane(evt.pane);
        ProcessMgrEvent(e);

        if (!e.GetVeto())
        {
            RestorePane(pane);
            Update();
        }
    }
    else if (evt.button == wxAUI_BUTTON_PIN &&
                (m_flags & wxAUI_MGR_ALLOW_FLOATING) && pane.IsFloatable())
    {
        if (pane.IsMaximized())
        {
            // If the pane is maximized, the original state must be restored
            // before trying to float the pane, otherwise the other panels
            // wouldn't appear correctly when it becomes floating.
            wxAuiManagerEvent e(wxEVT_AUI_PANE_RESTORE);
            e.SetManager(this);
            e.SetPane(evt.pane);
            ProcessMgrEvent(e);

            if (e.GetVeto())
            {
                // If it can't be restored, it can't be floated either.
                return;
            }

            RestorePane(pane);
        }

        pane.Float();
        Update();
    }
}

#endif // wxUSE_AUI
