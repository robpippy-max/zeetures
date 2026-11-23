#pragma once

#include <string>

namespace Openc2eImgui {

// The Help window (openc2e addition, owner 2026-11-11; in depth, with topics
// and a search, 2026-11-22): the toolbar's Help button, Help > Help Topics
// and F1.
void SetHelpOpen(bool open);
// opens it at a topic ("Health Kit"), as it is named in data/help/help-topics.md
void OpenHelpTopic(const std::string& topic);
// F1: the Help, at the topic of the kit in front, if one is
void OpenHelpHere();
void DrawHelpWindow();

} // namespace Openc2eImgui
