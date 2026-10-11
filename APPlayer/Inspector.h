// Copyright (c) 2026 WD Studios LLC.
// SPDX-License-Identifier: MIT
//
// Inspector.h - the "Inspector" panel: the DOM tree of the loaded document, the
// computed style and box of the selected element, a field to set a style or run any
// JavaScript in the page, and (Debug and Dev builds) the engine's debug overlays.
//
// Everything here goes through the public view and DOM API: the tree is read from the
// DOM, and styles and boxes are asked of the page itself (getComputedStyle,
// getBoundingClientRect) through UIView::evaluateJavaScript.

#pragma once

#include <Foundation/Basics.h>

#include <memory>
#include <string>
#include <vector>

struct ImDrawList;
struct ImVec2;

namespace aperture
{
  class UIView;
  namespace dom
  {
    class DOMElement;
  }
} // namespace aperture

namespace apui::samples::player
{
  class Inspector
  {
  public:
    void SetView(aperture::UIView* view) { m_view = view; }

    /// Rebuilds the DOM tree snapshot. Call after a document loads; the panel's
    /// Refresh button does the same.
    void OnDocumentLoaded();

    void Render();

    /// Outlines the selected element's box (as measured when it was selected or last
    /// refreshed) in the content area whose top-left corner is at origin.
    void DrawHighlight(ImDrawList* drawList, const ImVec2& origin) const;

  private:
    // The DOM is read once per document, not on every frame: the labels are built
    // here and the frame loop only draws them.
    struct DomNodeSnapshot
    {
      std::string label;
      std::shared_ptr<aperture::dom::DOMElement> elem; // read only when clicked
      std::vector<DomNodeSnapshot> children;
    };

    struct StyleProp
    {
      const char* name;
      std::string value;
    };

    void BuildSnapshot(
      const std::shared_ptr<aperture::dom::DOMElement>& elem, DomNodeSnapshot& out);
    void RenderSnapshotNode(
      const DomNodeSnapshot& node, const aperture::dom::DOMElement* selected);
    void RenderDomTab();
    void RenderStylesTab();
    void RenderLiveEditTab();
    void SelectElement(const std::shared_ptr<aperture::dom::DOMElement>& elem);
    void RefreshSelection();
    void Evaluate(const std::string& script);

    aperture::UIView* m_view = nullptr;

    DomNodeSnapshot m_domRoot;
    bool m_hasSnapshot = false;

    std::weak_ptr<aperture::dom::DOMElement> m_selected;
    std::string m_selectedScript; // JS expression that finds the selected element
    std::vector<StyleProp> m_styles;
    std::string m_selectionProblem;
    bool m_hasBox = false;
    float m_box[4] = { }; // x, y, width, height in viewport pixels

    char m_editSelector[256] = { };
    char m_editProp[128] = { };
    char m_editValue[256] = { };
    char m_expression[1024] = { };
    std::string m_result;

#if NS_ENABLED(NS_COMPILE_FOR_DEBUG) || NS_ENABLED(NS_COMPILE_FOR_DEVELOPMENT)
    void RenderDebugTab();
    uint32_t m_debugFlags = 0;
#endif
  };
} // namespace apui::samples::player
