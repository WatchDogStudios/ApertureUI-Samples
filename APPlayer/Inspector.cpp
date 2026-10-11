// Copyright (c) 2026 WD Studios LLC.
// SPDX-License-Identifier: MIT

#include "Inspector.h"

#include <APHTML/API/APUIView.h>
#include <APHTML/DOM/DOMDocument.h>
#include <APHTML/DOM/DOMElement.h>
#include <APHTML/Paint/V2/DebugOverlay.h>

#include <cstdio>
#include <imgui.h>
#include <iterator>

namespace apui::samples::player
{
  namespace
  {
    using aperture::dom::DOMElement;
    using aperture::dom::DOMNode;

    // The computed properties the Styles tab lists, in order.
    const char* const kStyleProperties[] = {
      "width",
      "height",
      "opacity",
      "font-size",
      "color",
      "background-color",
      "padding-top",
      "padding-right",
      "padding-bottom",
      "padding-left",
      "margin-top",
      "margin-right",
      "margin-bottom",
      "margin-left",
    };

    /// s as a single-quoted JavaScript string literal.
    std::string JsString(const std::string& s)
    {
      std::string out = "'";
      out.reserve(s.size() + 2);
      for (const char ch : s)
      {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '\\' || c == '\'')
        {
          out += '\\';
          out += ch;
        }
        else if (c < 0x20)
        {
          char escaped[8];
          std::snprintf(escaped, sizeof(escaped), "\\x%02x", c);
          out += escaped;
        }
        else
        {
          out += ch;
        }
      }
      out += '\'';
      return out;
    }

    /// A JavaScript expression that finds elem in the page: its index among the element
    /// children of each ancestor, from the document element down. Built from the live
    /// DOM when the element is selected; false when elem is no longer in the document.
    bool ElementScript(
      const std::shared_ptr<DOMElement>& elem, const DOMElement* root, std::string& out)
    {
      std::vector<int> path;
      std::shared_ptr<DOMElement> cur = elem;
      while (cur && cur.get() != root)
      {
        std::shared_ptr<DOMElement> parent = cur->getParentElement();
        if (!parent)
          return false;
        int index = 0;
        bool found = false;
        for (const auto& child : parent->getChildNodes())
        {
          if (!child || child->getNodeType() != DOMNode::Type::ELEMENT_NODE)
            continue;
          if (child.get() == cur.get())
          {
            found = true;
            break;
          }
          ++index;
        }
        if (!found)
          return false;
        path.push_back(index);
        cur = std::move(parent);
      }
      if (!cur)
        return false;
      out = "document.documentElement";
      for (auto it = path.rbegin(); it != path.rend(); ++it)
        out += ".children[" + std::to_string(*it) + "]";
      return true;
    }
  } // namespace

  void Inspector::OnDocumentLoaded()
  {
    m_selected.reset();
    m_selectedScript.clear();
    m_styles.clear();
    m_selectionProblem.clear();
    m_hasBox = false;
    m_hasSnapshot = false;
    m_domRoot = { };
#if NS_ENABLED(NS_COMPILE_FOR_DEBUG) || NS_ENABLED(NS_COMPILE_FOR_DEVELOPMENT)
    if (auto* dbg = aperture::paint::DebugOverlay::Active())
      dbg->PinElement(nullptr); // the pinned element belonged to the previous document
#endif

    if (!m_view)
      return;
    const auto doc = m_view->getDomDocument();
    const auto root = doc ? doc->getDocumentElement() : nullptr;
    if (!root)
      return;
    BuildSnapshot(root, m_domRoot);
    m_hasSnapshot = true;
  }

  void Inspector::BuildSnapshot(
    const std::shared_ptr<DOMElement>& elem, DomNodeSnapshot& out)
  {
    const std::string& tag = elem->getTagName();
    const std::string id = elem->getId();
    const std::string cls = elem->getClassName();
    if (!id.empty())
      out.label = "<" + tag + " id=\"" + id + "\">";
    else if (!cls.empty())
      out.label = "<" + tag + " class=\"" + cls + "\">";
    else
      out.label = "<" + tag + ">";
    out.elem = elem;

    for (const auto& child : elem->getChildNodes())
    {
      if (child && child->getNodeType() == DOMNode::Type::ELEMENT_NODE)
      {
        out.children.emplace_back();
        BuildSnapshot(std::static_pointer_cast<DOMElement>(child), out.children.back());
      }
    }
  }

  void Inspector::Render()
  {
    ImGui::Begin("Inspector");
    if (ImGui::BeginTabBar("##insp_tabs"))
    {
      if (ImGui::BeginTabItem("DOM"))
      {
        RenderDomTab();
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Styles"))
      {
        RenderStylesTab();
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Live Edit"))
      {
        RenderLiveEditTab();
        ImGui::EndTabItem();
      }
#if NS_ENABLED(NS_COMPILE_FOR_DEBUG) || NS_ENABLED(NS_COMPILE_FOR_DEVELOPMENT)
      if (ImGui::BeginTabItem("Debug"))
      {
        RenderDebugTab();
        ImGui::EndTabItem();
      }
#endif
      ImGui::EndTabBar();
    }
    ImGui::End();
  }

  void Inspector::RenderDomTab()
  {
    if (ImGui::Button("Refresh##dom"))
      OnDocumentLoaded();

    if (!m_hasSnapshot)
    {
      ImGui::TextDisabled("(no document; load a page and click Refresh)");
      return;
    }

    ImGui::BeginChild("##dom_tree", { 0, 0 });
    RenderSnapshotNode(m_domRoot, m_selected.lock().get());
    ImGui::EndChild();
  }

  void Inspector::RenderSnapshotNode(
    const DomNodeSnapshot& node, const DOMElement* selected)
  {
    ImGuiTreeNodeFlags flags
      = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow;
    if (node.children.empty())
      flags |= ImGuiTreeNodeFlags_Leaf;
    if (selected == node.elem.get())
      flags |= ImGuiTreeNodeFlags_Selected;

    ImGui::PushID(node.elem.get());
    const bool open = ImGui::TreeNodeEx(node.label.c_str(), flags);
    if (ImGui::IsItemClicked())
      SelectElement(node.elem);
    if (open)
    {
      for (const auto& child : node.children)
        RenderSnapshotNode(child, selected);
      ImGui::TreePop();
    }
    ImGui::PopID();
  }

  void Inspector::SelectElement(const std::shared_ptr<DOMElement>& elem)
  {
    m_selected = elem;
    m_selectedScript.clear();
    if (m_view)
    {
      const auto doc = m_view->getDomDocument();
      const auto root = doc ? doc->getDocumentElement() : nullptr;
      if (root && !ElementScript(elem, root.get(), m_selectedScript))
        m_selectedScript.clear();
    }
    RefreshSelection();
  }

  void Inspector::RefreshSelection()
  {
    m_styles.clear();
    m_hasBox = false;
    m_selectionProblem.clear();
    if (!m_view || m_selected.expired())
      return;
    if (m_selectedScript.empty())
    {
      m_selectionProblem
        = "The element is no longer in the document. Refresh the DOM tab.";
      return;
    }

    // One round trip: the border box on the first line, then one computed value per line.
    std::string names;
    for (const char* name : kStyleProperties)
      names += (names.empty() ? "" : ",") + JsString(name);
    const std::string script = "(function(){var e=" + m_selectedScript
      + ";if(!e)return '';var r=e.getBoundingClientRect();var s=getComputedStyle(e);"
        "return [r.left,r.top,r.width,r.height].join(',')+'\\n'+["
      + names + "].map(function(n){return s.getPropertyValue(n);}).join('\\n');})()";
    std::string result;
    if (!m_view->evaluateJavaScript(script.c_str(), result) || result.empty())
    {
      m_selectionProblem
        = "The page did not answer; the element may have moved. Refresh the DOM tab.";
      return;
    }

    size_t lineStart = 0;
    for (int line = -1; line < static_cast<int>(std::size(kStyleProperties)); ++line)
    {
      size_t lineEnd = result.find('\n', lineStart);
      if (lineEnd == std::string::npos)
        lineEnd = result.size();
      const std::string text = result.substr(lineStart, lineEnd - lineStart);
      if (line < 0)
        m_hasBox = std::sscanf(text.c_str(), "%f,%f,%f,%f", &m_box[0], &m_box[1],
                     &m_box[2], &m_box[3])
          == 4;
      else
        m_styles.push_back({ kStyleProperties[line], text });
      if (lineEnd == result.size())
        break;
      lineStart = lineEnd + 1;
    }
  }

  void Inspector::RenderStylesTab()
  {
    if (m_selected.expired())
    {
      ImGui::TextDisabled("Select an element in the DOM tab");
      return;
    }

    if (ImGui::Button("Refresh"))
      RefreshSelection();
    ImGui::Separator();
    if (!m_selectionProblem.empty())
    {
      ImGui::TextWrapped("%s", m_selectionProblem.c_str());
      return;
    }
    if (m_hasBox)
      ImGui::TextDisabled(
        "box %.1f, %.1f  %.1f x %.1f", m_box[0], m_box[1], m_box[2], m_box[3]);

    if (ImGui::BeginTable(
          "##styles", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV))
    {
      ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 130.0f);
      ImGui::TableSetupColumn("Value");
      ImGui::TableHeadersRow();
      for (const auto& prop : m_styles)
      {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(prop.name);
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(prop.value.c_str());
      }
      ImGui::EndTable();
    }
  }

  void Inspector::Evaluate(const std::string& script)
  {
    std::string out;
    if (!m_view || !m_view->evaluateJavaScript(script.c_str(), out))
      m_result = "(the script threw, or no page is loaded)";
    else
      m_result = out.empty() ? "(undefined)" : out;
    if (!m_selected.expired())
      RefreshSelection(); // the script may have changed the selected element
  }

  void Inspector::RenderLiveEditTab()
  {
    ImGui::TextDisabled("Set a style on the first element a selector matches.");
    ImGui::Separator();

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint(
      "##sel", "selector, e.g. #title", m_editSelector, sizeof(m_editSelector));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.4f);
    ImGui::InputTextWithHint("##prop", "property", m_editProp, sizeof(m_editProp));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##val", "value", m_editValue, sizeof(m_editValue));

    if (ImGui::Button("Apply"))
      Evaluate("(function(){var el=document.querySelector(" + JsString(m_editSelector)
        + ");if(!el)return 'element not found';el.style.setProperty("
        + JsString(m_editProp) + "," + JsString(m_editValue) + ");return 'OK';})()");

    ImGui::Spacing();
    ImGui::TextDisabled("Or run any JavaScript in the page:");
    ImGui::SetNextItemWidth(-1.0f);
    const bool enter = ImGui::InputText(
      "##expr", m_expression, sizeof(m_expression), ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::Button("Eval") || enter)
      Evaluate(m_expression);

    if (!m_result.empty())
      ImGui::TextWrapped("%s", m_result.c_str());
  }

  void Inspector::DrawHighlight(ImDrawList* drawList, const ImVec2& origin) const
  {
    if (!m_hasBox || m_selected.expired())
      return;
    const ImVec2 min = { origin.x + m_box[0], origin.y + m_box[1] };
    const ImVec2 max = { min.x + m_box[2], min.y + m_box[3] };
    drawList->AddRectFilled(min, max, IM_COL32(79, 184, 255, 48));
    drawList->AddRect(min, max, IM_COL32(79, 184, 255, 220), 0.0f, 0, 1.5f);
  }

#if NS_ENABLED(NS_COMPILE_FOR_DEBUG) || NS_ENABLED(NS_COMPILE_FOR_DEVELOPMENT)
  void Inspector::RenderDebugTab()
  {
    using DC = aperture::paint::DebugCategory;
    auto* dbg = aperture::paint::DebugOverlay::Active();
    if (!dbg)
    {
      ImGui::TextDisabled("This SDK build has no debug overlays.");
      return;
    }

    ImGui::SeparatorText("Overlay Categories");
    auto bit = [&](const char* label, DC cat)
    {
      bool on = (m_debugFlags & static_cast<uint32_t>(cat)) != 0;
      if (ImGui::Checkbox(label, &on))
      {
        if (on)
          m_debugFlags |= static_cast<uint32_t>(cat);
        else
          m_debugFlags &= ~static_cast<uint32_t>(cat);
        dbg->SetFlags(m_debugFlags);
      }
    };
    bit("Hit Test", DC::HitTest);
    bit("Dirty Rect", DC::DirtyRect);
    bit("Stacking Context", DC::StackingContext);
    bit("Paint Nodes", DC::PaintNodes);
    bit("Box Model", DC::BoxModel);

    ImGui::Spacing();
    ImGui::SeparatorText("Element Pin");
    if (ImGui::Button("Pin selected element"))
    {
      // The layout element the view built for this DOM element (null before layout).
      const auto sel = m_selected.lock();
      dbg->PinElement(sel ? sel->getCoreElement() : nullptr);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear pin"))
      dbg->PinElement(nullptr);

    ImGui::Spacing();
    ImGui::Text("Primitives this frame: %d", static_cast<int>(dbg->Primitives().size()));
  }
#endif
} // namespace apui::samples::player
