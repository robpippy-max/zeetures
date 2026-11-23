#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Openc2eImgui {

struct WorldEntry {
	std::string name;
	std::filesystem::path dir;
};

std::filesystem::path GetWorldsDirectory();
std::filesystem::path WorldFile(const std::filesystem::path& worlddir);
std::vector<WorldEntry> ListWorlds();
std::optional<WorldEntry> FindWorld(const std::string& name);
std::vector<std::filesystem::path> ListWorldTemplates();
bool CreateWorld(const std::string& name, const std::filesystem::path& tmpl, std::string& error);

// Copies what a world keeps beside its save (its photos, the genes of its
// departed, its own Additions) into another world's folder: Save World As.
bool CopyWorldExtras(const std::filesystem::path& from, const std::filesystem::path& to, std::string& error);

// Restarts openc2e with --world <name> (C1/C2 worlds load once, at start-up).
void RelaunchIntoWorld(const std::string& name);

// The new-world form; returns the created world's name once created.
// reset = true only clears the form (draws nothing).
std::string DrawNewWorldForm(bool reset);

// Start-up chooser, run before the world loads. Returns "" if the user quits.
// A world the player chose to open at every launch is returned at once.
std::string RunWorldChooser();
// the world opened at launch without asking ("": ask)
std::string DefaultWorld();
void SetDefaultWorld(const std::string& name);
// once the world has loaded
void DefaultWorldOpened();

} // namespace Openc2eImgui
