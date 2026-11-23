#pragma once

// Updating Zeetures from inside the game (owner, 2026-11-22: "update from the
// app ... check for updates periodically and then prompt to update ... a
// setting to turn that update check off, and to turn auto-updates ... off and
// on"). See Updater.cpp.

namespace Openc2eImgui {

// each frame: the periodic check, downloads, and the update window
void UpdaterTick();
// the Help menu's items
void UpdaterMenuItems();
// looks for a new version now and says what it found (Help > Check for
// Updates, and About Zeetures)
void CheckForUpdatesNow();
// as the program ends, after the world is saved: installs a downloaded
// update if it should be, and starts the new version if asked
void UpdaterAtExit();

} // namespace Openc2eImgui
