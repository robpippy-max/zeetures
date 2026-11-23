// The Owner's Kit (Creatures 2): birth registration (a name, the birth
// certificate), photographs, the family and the life log of births, deaths
// and pregnancies (also announced as notices on the status bar). C2's own art from the player's
// Applet Data; our layout. Names, registration, birth times, photos and
// notes are kept with the world (WorldSave, monikerData).

#include "Agent.h"
#include "common/devenv.h"
#include "C2Kits.h"
#include "C2KitsInternal.h"
#include "WorldSave.h"
#include "Camera.h"
#include "Engine.h"
#include "PointerAgent.h"
#include "World.h"
#include "common/backend/Backend.h"
#include "creatures/CreatureAgent.h"
#include "caosValue.h"
#include "creatures/oldCreature.h"
#include "fileformats/ImageUtils.h"
#include "historyManager.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fmt/format.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <climits>
#include <map>
#include <set>

using namespace ImGuiUtils;
namespace fs = std::filesystem;

namespace Openc2eImgui {

static const ImU32 INK = IM_COL32(74, 46, 22, 255);
static const char* s_stages[] = {"baby", "child", "adolescent", "youth", "adult", "old", "senile"};

static const char* stageWord(int st) {
	return st >= 0 && st < 7 ? s_stages[st] : "?";
}

// ---- Registration ------------------------------------------------------------------

static monikerData* recordOf(const std::string& mon);
static std::string recordName(monikerData* m);
static unsigned int photoTick(const std::string& file);
static std::string takePhoto(Agent* a, monikerData& m);

// a seal for a registered birth: our own, drawn (red wax, a ribbon, a tick)
static void drawSeal(ImDrawList* dl, ImVec2 c, float r) {
	dl->AddTriangleFilled(c + ImVec2(-r * 0.6f, r * 0.4f), c + ImVec2(-r * 0.15f, r * 0.5f), c + ImVec2(-r * 0.75f, r * 1.7f), IM_COL32(70, 90, 170, 255));
	dl->AddTriangleFilled(c + ImVec2(r * 0.6f, r * 0.4f), c + ImVec2(r * 0.15f, r * 0.5f), c + ImVec2(r * 0.75f, r * 1.7f), IM_COL32(70, 90, 170, 255));
	dl->AddCircleFilled(c, r, IM_COL32(150, 24, 28, 255), 24);
	dl->AddCircleFilled(c - ImVec2(r * 0.15f, r * 0.15f), r * 0.82f, IM_COL32(196, 42, 44, 255), 24);
	dl->AddCircle(c, r * 0.62f, IM_COL32(120, 16, 20, 255), 24, 1.5f);
	dl->AddPolyline(std::vector<ImVec2>{c + ImVec2(-r * 0.32f, 0), c + ImVec2(-r * 0.08f, r * 0.26f), c + ImVec2(r * 0.36f, -r * 0.28f)}.data(), 3, IM_COL32(255, 220, 200, 255), 0, 2.5f);
}

static void RegistrationPage() {
	if (!KitNeedsCreature())
		return;
	Agent* a = KitCreatureAgent();
	c2Creature* c = KitCreature();
	monikerData* m = KitRecordsOf(a);
	if (!m) {
		ImGui::TextDisabled("This creature has no records.");
		return;
	}
	const float W = 560;
	const ImVec4 gold(0.86f, 0.70f, 0.40f, 1), dim = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
	int st = m->died ? m->diedstage : (int)c->getStage();
	std::string mon = world.history->findMoniker(c->getGenome());

	KitBeginCard("idcard", ImVec2(W, 0));
	ImDrawList* dl = ImGui::GetWindowDrawList();
	// its latest photo (the Photos page takes more)
	const float PW = 168, PH = 126;
	ImVec2 pp = ImGui::GetCursorScreenPos();
	int n = (int)m->photos.size();
	Texture tex = n ? KitPhotoTexture(engine.currentworlddir / m->photos[n - 1]) : Texture();
	if (tex)
		dl->AddImage(tex.as<ImTextureID>(), pp, pp + ImVec2(PW, PH));
	else {
		dl->AddRectFilled(pp, pp + ImVec2(PW, PH), IM_COL32(18, 18, 20, 255));
		const char* none = "no photo yet";
		ImVec2 ns = ImGui::CalcTextSize(none);
		dl->AddText(pp + ImVec2((PW - ns.x) / 2, (PH - ns.y) / 2), IM_COL32(110, 110, 110, 255), none);
	}
	dl->AddRect(pp - ImVec2(1, 1), pp + ImVec2(PW + 1, PH + 1), IM_COL32(205, 170, 95, 255));
	ImGui::Dummy(ImVec2(PW, PH));
	if (n) {
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + PW);
		ImGui::TextDisabled("photo, %s", KitDateText(photoTick(m->photos[n - 1])).c_str());
		ImGui::PopTextWrapPos();
	}
	else if (ImGui::SmallButton("Take a photo"))
		takePhoto(a, *m);
	float leftBottom = ImGui::GetCursorPosY();

	// the details, right of the photo
	ImGui::SetCursorScreenPos(pp + ImVec2(PW + 18, 0));
	ImGui::BeginGroup();
	// the name, which the owner may change
	static char name[64];
	static monikerData* editing = nullptr;
	// (and when it was named elsewhere: a default name given to a blank one)
	if (editing != m || (ImGui::GetActiveID() != ImGui::GetID("##name") && m->name != name)) {
		editing = m;
		std::snprintf(name, sizeof(name), "%s", m->name.c_str());
	}
	ImGui::SetNextItemWidth(W - PW - 60);
	if (ImGui::InputTextWithHint("##name", "type a name", name, sizeof(name)))
		m->name = name;
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Its name (kept with the world)");
	ImGui::TextColored(gold, "%s %s, %s", c->isFemale() ? "female" : "male", KitSpeciesWord(a), stageWord(st));
	ImGui::Spacing();
	auto row = [&](const char* k, const std::string& v) {
		ImGui::TextColored(dim, "%-10s", k);
		ImGui::SameLine(0, 4);
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + W - PW - 160);
		ImGui::TextUnformatted(v.c_str());
		ImGui::PopTextWrapPos();
	};
	row(m->bornknown ? "Born" : "First seen", KitDateText(m->borntick));
	if (m->died)
		row("Died", KitDateText(m->diedtick));
	else
		row("Age", (m->bornknown ? "" : "at least ") + KitAgeText(KitAgeTicks(*m), false));
	auto nameOf = [](const std::string& mm) {
		monikerData* r = recordOf(mm);
		return r ? recordName(r) : std::string("unknown");
	};
	if (m->mother.empty() && m->father.empty())
		row("Parents", "none: it hatched from an egg laid by the Hatchery or the Breeder's Kit");
	else if (m->mother == m->father) // one genome copied (NEW: GENE of one creature: the Metamorphosis device)
		row("Parents", "cloned from " + nameOf(m->mother));
	else if (m->spliced) // the gene splicer's (NEW: GENE of two creatures)
		row("Parents", fmt::format("spliced from {} and {} in the gene lab", nameOf(m->mother), nameOf(m->father)));
	else
		row("Parents", fmt::format("{} and {}", m->mother.empty() ? "unknown" : nameOf(m->mother), m->father.empty() ? "unknown" : nameOf(m->father)));
	std::vector<std::string> kids;
	for (auto& [k, r] : world.history->all())
		if (!mon.empty() && (r.mother == mon || r.father == mon))
			kids.push_back(recordName(&r));
	std::string kidText;
	for (size_t i = 0; i < kids.size(); i++)
		kidText += (i ? (i + 1 == kids.size() ? " and " : ", ") : "") + kids[i];
	row("Children", kids.empty() ? "none yet" : kidText);
	row("Moniker", mon);
	ImGui::EndGroup();
	float rightBottom = ImGui::GetCursorPosY();

	// registration: a button, or the seal
	ImGui::SetCursorPosY(std::max(leftBottom, rightBottom) + 6);
	if (m->registered) {
		ImVec2 sp = ImGui::GetCursorScreenPos();
		drawSeal(dl, sp + ImVec2(18, 16), 15);
		ImGui::SetCursorScreenPos(sp + ImVec2(44, 8));
		ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.5f, 1), "Registered");
		ImGui::SetCursorScreenPos(sp + ImVec2(0, 44));
	} else {
		if (ImGui::Button("Register this birth"))
			m->registered = true;
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Seal its record. The name can still be changed.");
	}
	KitEndCard();

	// its life stage, older or younger: stage by stage, as natural ageing
	// (moved here from the Creatures menu; owner, 2026-11-12)
	if (c->isAlive() && !m->died) {
		static const char* const stages[] = {"Baby", "Child", "Adolescent", "Youth", "Adult", "Old", "Senile"};
		int stage = (int)c->getStage();
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted("Life stage");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(140);
		if (ImGui::BeginCombo("##lifestage", stage >= 0 && stage < 7 ? stages[stage] : "?")) {
			for (int i = 0; i < 7; i++)
				if (ImGui::Selectable(stages[i], i == stage) && i != stage)
					if (auto sp = AgentRef(a).lock())
						WorldSave::RequestLifeStage(sp, i);
			ImGui::EndCombo();
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Make it older or younger: it changes a stage at a time, as natural ageing does,\nkeeping its memories, chemistry and health.");
	}

	if (m->died) {
		std::string age = KitAgeText(KitAgeTicks(*m), false);
		KitNoteColored(ImVec4(0.85f, 0.45f, 0.45f, 1), "Died %s, aged %s%s, a%s %s.", KitDateText(m->diedtick).c_str(), m->bornknown ? "" : "at least ",
			age.c_str(), st == 4 || st == 5 ? "n" : "", stageWord(st));
	}

	// notes
	static char notes[1024];
	static monikerData* noting = nullptr;
	if (noting != m) {
		noting = m;
		std::snprintf(notes, sizeof(notes), "%s", m->notes.c_str());
	}
	ImGui::TextDisabled("Notes");
	if (ImGui::InputTextMultiline("##notes", notes, sizeof(notes), ImVec2(-FLT_MIN, 60)))
		m->notes = notes;
}

// ---- Photographs --------------------------------------------------------------------

static constexpr int PHOTO_W = 240, PHOTO_H = 180;

class PhotoCamera : public Camera {
  public:
	unsigned int getWidth() const override { return PHOTO_W; }
	unsigned int getHeight() const override { return PHOTO_H; }
};

static std::map<std::string, Texture> s_photos; // by full path

Texture KitPhotoTexture(const fs::path& path) {
	auto key = path.string();
	auto it = s_photos.find(key);
	if (it != s_photos.end())
		return it->second;
	Texture t;
	std::error_code ec;
	if (fs::exists(path, ec)) {
		try {
			MultiImage im = ImageUtils::ReadImage(path);
			if (!im.empty())
				t = get_backend()->createTextureFromImage(im[0]);
		} catch (std::exception&) {
		}
	}
	s_photos[key] = t;
	return t;
}

// photo_<world tick>_<n>.bmp: when it was taken
static unsigned int photoTick(const std::string& file) {
	unsigned int t = 0;
	std::sscanf(fs::path(file).filename().string().c_str(), "photo_%u", &t);
	return t;
}

static std::string takePhoto(Agent* a, monikerData& m) {
	if (engine.currentworlddir.empty())
		return "This world has no folder to keep photos in.";
	static std::unique_ptr<PhotoCamera> cam;
	static std::shared_ptr<RenderTarget> target;
	if (!cam)
		cam = std::make_unique<PhotoCamera>();
	if (!target)
		target = get_backend()->newRenderTarget(PHOTO_W, PHOTO_H);
	float cx = a->x + a->getWidth() / 2.0f, cy = a->y + a->getHeight() / 2.0f;
	cam->moveToGlobal((int)cx - PHOTO_W / 2, (int)cy - PHOTO_H / 2 - 10, jump);
	world.drawWorld(cam.get(), target.get(), true, a); // no other creature in the picture
	fs::path dir = engine.currentworlddir / "photos";
	std::error_code ec;
	fs::create_directories(dir, ec);
	std::string file;
	for (int n = 0; n < 1000; n++) {
		file = fmt::format("photo_{}_{}.bmp", world.worldtickcount, n);
		if (!fs::exists(dir / file, ec))
			break;
	}
	if (!target->saveToFile((dir / file).string()))
		return "Couldn't save the photo.";
	m.photos.push_back("photos/" + file);
	world.history->tell(m.getName(), storyEntry::photo);
	return "Photo taken.";
}

static void PhotosPage() {
	if (!KitNeedsCreature())
		return;
	Agent* a = KitCreatureAgent();
	monikerData* m = KitRecordsOf(a);
	if (!m)
		return;
	static std::string message;
	static int big = -1;
	static monikerData* shown = nullptr;
	if (shown != m) {
		shown = m;
		big = -1;
		message.clear();
	}
	// developer aid: OPENC2E_TEST_PHOTO=N takes one photo once the world has
	// run N ticks (1: at once)
	static bool testPhoto = devenv("OPENC2E_TEST_PHOTO") != nullptr;
	static unsigned int testPhotoTick = testPhoto ? (unsigned int)atoi(devenv("OPENC2E_TEST_PHOTO")) : 0;
	if (testPhoto && world.worldtickcount >= testPhotoTick) {
		testPhoto = false;
		message = takePhoto(a, *m);
		big = (int)m->photos.size() - 1;
	}
	if (ImGui::Button("Take a photo"))
		message = takePhoto(a, *m), big = (int)m->photos.size() - 1;
	ImGui::SameLine();
	ImGui::Text("%s: %d photo%s", KitCreatureName(a).c_str(), (int)m->photos.size(), m->photos.size() == 1 ? "" : "s");
	if (!message.empty()) {
		ImGui::SameLine();
		ImGui::TextDisabled("%s", message.c_str());
	}

	// the album: thumbnails in frames, three to a row
	const float cellW = 136, thumbW = 120, thumbH = 90;
	ImVec2 p = ImGui::GetCursorScreenPos();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	int rows = ((int)m->photos.size() + 2) / 3;
	float rowH = thumbH + 22;
	for (size_t i = 0; i < m->photos.size(); i++) {
		ImVec2 cell = p + ImVec2(cellW * (i % 3), rowH * (i / 3));
		dl->AddRectFilled(cell + ImVec2(2, 2), cell + ImVec2(cellW - 2, rowH - 2), IM_COL32(26, 28, 34, 255), 5);
		dl->AddRect(cell + ImVec2(2, 2), cell + ImVec2(cellW - 2, rowH - 2), IM_COL32(70, 72, 80, 255), 5);
		Texture t = KitPhotoTexture(engine.currentworlddir / m->photos[i]);
		ImVec2 at = cell + ImVec2((cellW - thumbW) / 2, 11);
		if (t)
			dl->AddImage(t.as<ImTextureID>(), at, at + ImVec2(thumbW, thumbH));
		else
			dl->AddText(at + ImVec2(10, 36), IM_COL32(160, 160, 160, 255), "(missing)");
		ImGui::SetCursorScreenPos(at);
		ImGui::PushID((int)i);
		if (ImGui::InvisibleButton("photo", ImVec2(thumbW, thumbH)))
			big = (int)i;
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Taken %s", KitDateText(photoTick(m->photos[i])).c_str());
		ImGui::PopID();
		if ((int)i == big)
			dl->AddRect(at - 2, at + ImVec2(thumbW, thumbH) + 2, IM_COL32(255, 220, 120, 255), 0, 0, 2);
	}
	ImGui::SetCursorScreenPos(p + ImVec2(0, std::max(1, rows) * rowH + 4));
	if (m->photos.empty())
		KitNote("No photos yet. The camera frames the creature where it stands, with no other creature in the way.");

	// the chosen photo, full size
	if (big >= 0 && big < (int)m->photos.size()) {
		Texture t = KitPhotoTexture(engine.currentworlddir / m->photos[big]);
		if (t)
			ImGui::Image(t.as<ImTextureID>(), ImVec2(t.width, t.height));
		ImGui::TextDisabled("Taken %s", KitDateText(photoTick(m->photos[big])).c_str());
		ImGui::SameLine();
		if (ImGui::SmallButton("Delete..."))
			ImGui::OpenPopup("delete photo");
		if (ImGui::BeginPopup("delete photo")) {
			ImGui::Text("Delete this photo for good?");
			if (ImGui::Button("Delete it")) {
				fs::path f = engine.currentworlddir / m->photos[big];
				std::error_code ec;
				fs::remove(f, ec);
				s_photos.erase(f.string());
				m->photos.erase(m->photos.begin() + big);
				big = -1;
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("Keep it"))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
	}
}

// ---- Life log, and the notices on the status bar ------------------------------------

static const char* kindWord(int kind) {
	switch (kind) {
		case lifeLogEntry::birth: return "Born";
		case lifeLogEntry::death: return "Died";
		case lifeLogEntry::eggLaid: return "Egg laid";
		case lifeLogEntry::unconscious: return "Unconscious";
		case lifeLogEntry::cameRound: return "Came round";
		case lifeLogEntry::changed: return "Changed";
		case lifeLogEntry::together: return "Together";
		case lifeLogEntry::fellIll: return "Ill";
		case lifeLogEntry::recovered: return "Recovered";
		default: return "Pregnant";
	}
}

static const char* genusWord(int genus) {
	switch (genus) {
		case 2: return "grendel";
		case 3: return "ettin";
		default: return "norn";
	}
}

// its name now (it may have been named since), else then, else "Unnamed norn"
static std::string entryName(const lifeLogEntry& e) {
	if (e.kind == lifeLogEntry::together) // "4 norns"
		return fmt::format("{} {}s", e.stage, genusWord(e.genus));
	if (world.history->hasMoniker(e.moniker) && !world.history->getMoniker(e.moniker).name.empty())
		return world.history->getMoniker(e.moniker).name;
	if (!e.name.empty())
		return e.name;
	return fmt::format("Unnamed {}", genusWord(e.genus));
}

static std::string entryDetail(const lifeLogEntry& e) {
	std::string who = fmt::format("{} {}", e.female ? "female" : "male", genusWord(e.genus));
	switch (e.kind) {
		case lifeLogEntry::birth: return "a " + who;
		case lifeLogEntry::death: {
			std::string age = KitAgeText(e.age);
			age = age.substr(0, age.find(" ("));
			// what it died of, in plain words (owner, 2026-11-11); entries
			// from before causes were kept only know old age
			std::string cause = !e.cause.empty() ? e.cause : e.oldage ? "old age" : "cause not recorded";
			return fmt::format("{}, aged {}{}, {}: {}", who, e.ageknown ? "" : "at least ", age, stageWord(e.stage), cause);
		}
		case lifeLogEntry::unconscious: return fmt::format("a {}, {}: out of energy. Mend the cause soon (out of water, fed, a Health Kit injection)", who, stageWord(e.stage));
		case lifeLogEntry::cameRound: return fmt::format("a {}, {}: {}", who, stageWord(e.stage), e.cause == "revived" ? "brought back to life" : "came round");
		case lifeLogEntry::changed: return fmt::format("a {}, {}: {}", who, stageWord(e.stage), e.cause.empty() ? "changed into something else" : e.cause);
		case lifeLogEntry::together: return fmt::format("{} hatched together, grew old together and reached the end together", e.cause);
		case lifeLogEntry::fellIll: return fmt::format("a {}, {}: caught {}. Keep it warm and fed; the Health Kit shows how it is", who, stageWord(e.stage), e.cause.empty() ? "an illness" : e.cause);
		case lifeLogEntry::recovered: return fmt::format("a {}, {}: got over {}", who, stageWord(e.stage), e.cause.empty() ? "an illness" : e.cause);
		case lifeLogEntry::eggLaid: {
			std::string s = fmt::format("laid by a {}, {}", who, stageWord(e.stage));
			if (auto egg = KitEggOf(e.child))
				s += fmt::format("; the egg is at {:.0f}, {:.0f}, {}", egg->x, egg->y, KitEggState(egg.get()));
			else if (KitHatchedFrom(e.child))
				s += "; it has hatched";
			return s;
		}
		default: return fmt::format("{}, {}", who, stageWord(e.stage));
	}
}

// the creature's body, if it is still in the world
static std::shared_ptr<Agent> entryBody(const lifeLogEntry& e) {
	if (!world.history->hasMoniker(e.moniker))
		return nullptr;
	Agent* a = world.history->getMoniker(e.moniker).owner.get();
	if (!a || a->isDying() || !dynamic_cast<CreatureAgent*>(a))
		return nullptr;
	return AgentRef(a).lock();
}

// the memorial stone (memorial.cos), if this world has it
static std::shared_ptr<Agent> memorialStone() {
	for (auto& a : world.agents)
		if (a && !a->isDying() && a->family == 2 && a->genus == 3 && a->species == 254)
			return a;
	return nullptr;
}

static void followEntry(const lifeLogEntry& e) {
	// grew old together: the memorial stone, its words open. Only creatures
	// press the stone (owner, 2026-11-15), so the notice marks the press as
	// its own (OV09 1; memorial.cos) for the stone to open its window.
	if (e.kind == lifeLogEntry::together) {
		if (auto stone = memorialStone()) {
			KitLookAt(stone);
			stone->var[9] = caosValue(1);
			stone->queueScript(1, world.hand());
		} else
			OpenC2KitAt("chronicles", "History");
		return;
	}
	// a death: its gravestone (the Graveyard Kit)
	if (e.kind == lifeLogEntry::death) {
		OpenGraveyardAt(e.moniker);
		return;
	}
	// an egg: go to the egg wherever it is now, or the baby once it hatched
	if (e.kind == lifeLogEntry::eggLaid) {
		if (auto egg = KitEggOf(e.child)) {
			KitLookAt(egg);
			return;
		}
		if (auto baby = KitHatchedFrom(e.child)) {
			KitGoTo(baby);
			return;
		}
	}
	auto body = entryBody(e);
	if (!body) { // gone: its line in the log is all there is
		OpenC2KitAt("owners", "Life log");
		return;
	}
	KitGoTo(body);
	if (e.kind == lifeLogEntry::pregnancy)
		OpenC2KitAt("breeders", "Reproductive");
	else if (e.kind == lifeLogEntry::birth)
		OpenC2KitAt("owners", "Registration");
	// fell unconscious or came round: going to it is enough (owner,
	// 2026-11-12: the Owner's Kit opening there wasn't needed)
}

// The world's history (Chronicles Kit; was the Owner's Kit's life log)
void WorldHistoryPage() {
	auto& log = world.history->lifelog;
	if (log.empty()) {
		KitNote("No births, deaths or pregnancies yet. They are recorded here as they happen.");
		return;
	}
	ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
	// fixed columns, the kit fitting them (owner, 2026-11-20: the Details
	// column half as wide, its full text on hovering; before, 2026-11-13, the
	// list filled a window the player sized, which made the Statistics page
	// as wide)
	const float colW[5] = {180, 96, 120, 230, 44};
	const ImGuiStyle& st = ImGui::GetStyle();
	float tableW = st.ScrollbarSize + 6;
	for (float w : colW)
		tableW += w + st.CellPadding.x * 2 + 1;
	if (ImGui::BeginTable("log", 5, flags, ImVec2(tableW, 430))) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("When", 0, colW[0]);
		ImGui::TableSetupColumn("What", 0, colW[1]); // room for "Unconscious"
		ImGui::TableSetupColumn("Who", 0, colW[2]);
		ImGui::TableSetupColumn("Details", 0, colW[3]);
		ImGui::TableSetupColumn("", 0, colW[4]);
		ImGui::TableHeadersRow();
		for (int i = (int)log.size() - 1; i >= 0; i--) {
			auto& e = log[i];
			ImGui::PushID(i);
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(KitDateText(e.tick).c_str());
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(kindWord(e.kind));
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(entryName(e).c_str());
			ImGui::TableNextColumn();
			{
				std::string detail = entryDetail(e);
				ImGui::TextUnformatted(detail.c_str());
				// the whole of it, when it does not fit the column
				if (ImGui::IsItemHovered() && ImGui::CalcTextSize(detail.c_str()).x > ImGui::GetColumnWidth()) {
					ImGui::BeginTooltip();
					ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
					ImGui::TextUnformatted(detail.c_str());
					ImGui::PopTextWrapPos();
					ImGui::EndTooltip();
				}
			}
			ImGui::TableNextColumn();
			// (no grave while one brought back to life lives)
			if (e.kind == lifeLogEntry::death && world.history->hasMoniker(e.moniker) && world.history->getMoniker(e.moniker).died) {
				if (ImGui::SmallButton("Grave"))
					followEntry(e);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Its gravestone in the Graveyard Kit");
			} else if (entryBody(e) && ImGui::SmallButton("Go"))
				followEntry(e);
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
}

static int s_notice_menu = -1; // the notice right-clicked for its menu (-1: the "+N" button)
static ImVec2 s_more_at; // where the "+N" list opens (its bottom left)
static int s_clear_older = 0; // "Clear these" in the "+N" list: how many of the oldest to clear

void DrawLifeNotices(float room) {
	if (engine.version != 2 || !world.history)
		return;
	// the notices on the bar: the world's own list, saved with it, so they
	// last through relaunches (owner, 2026-11-15)
	std::vector<lifeLogEntry>& s_notices = world.history->shownNotices;
	// (each kind only if its kit's checkbox is ticked: KitNoticeShown)
	for (auto& e : world.history->notices)
		if (KitNoticeShown(e.kind))
			s_notices.push_back(e);
	world.history->notices.clear();
	// developer aid: OPENC2E_TEST_NOTICES=1 announces one of each for the
	// selected creature (not logged), =N that many; =follow also follows the
	// last one
	static bool test = devenv("OPENC2E_TEST_NOTICES") != nullptr;
	if (test && world.selectedcreature) {
		test = false;
		std::string mon = world.history->findMoniker(AgentRef(world.selectedcreature));
		// (=N, a number over 3: N notices, the kinds in turn)
		int count = std::max(3, atoi(devenv("OPENC2E_TEST_NOTICES")));
		for (int k = 0; k < count; k++) {
			lifeLogEntry e;
			e.kind = k % 7;
			e.tick = world.worldtickcount;
			e.moniker = mon;
			e.genus = world.selectedcreature->genus;
			s_notices.push_back(e);
		}
		if (std::string(devenv("OPENC2E_TEST_NOTICES")) == "follow") // as if the pregnancy notice were clicked
			followEntry(s_notices.back());
	}
	// OPENC2E_TEST_DEATH=1: the selected creature dies (logged and announced)
	static bool testDeath = devenv("OPENC2E_TEST_DEATH") != nullptr;
	if (testDeath && world.selectedcreature) {
		testDeath = false;
		if (auto* ca = dynamic_cast<CreatureAgent*>(world.selectedcreature.get()))
			if (ca->getCreature()->isAlive())
				ca->getCreature()->die();
	}
	// OPENC2E_TEST_TOGETHER=1: four living creatures are given the same
	// birth tick and die of old age at once, for the memorial's notice
	static bool testTogether = devenv("OPENC2E_TEST_TOGETHER") != nullptr;
	if (testTogether && world.tickcount > 30) {
		testTogether = false;
		std::vector<CreatureAgent*> four;
		for (auto& a : world.agents)
			if (auto* ca = dynamic_cast<CreatureAgent*>(a.get()))
				if (!a->isDying() && ca->getCreature()->isAlive() && four.size() < 4)
					four.push_back(ca);
		for (auto* ca : four) {
			std::string mon = world.history->findMoniker(ca->getCreature()->getGenome());
			monikerData& m = world.history->getMoniker(mon);
			m.borntick = world.worldtickcount > 5000 ? world.worldtickcount - 5000 : 0;
			m.bornknown = true;
			ca->getCreature()->dieOfOldAge();
		}
		fmt::print("* TEST TOGETHER: {} died together\n", four.size());
	}
	// OPENC2E_TEST_STONE=1: the first living creature presses the memorial
	// stone (as its own activation would), with its Loneliness Decrease
	// (chemical 42) and loneliness drive printed before and after
	static bool testStone = devenv("OPENC2E_TEST_STONE") != nullptr;
	static int testStoneAt = -1;
	static AgentRef testStoneBy;
	if (testStone && world.tickcount > 40) {
		testStone = false;
		auto stone = memorialStone();
		for (auto& a : world.agents)
			if (auto* ca = dynamic_cast<CreatureAgent*>(a.get()))
				if (stone && !a->isDying() && ca->getCreature()->isAlive()) {
					auto* c = dynamic_cast<oldCreature*>(ca->getCreature());
					if (!c)
						continue;
					fmt::print("* TEST STONE: before chem42 {} loneliness {}\n", (int)c->getChemical(42), (int)c->getDrive(7));
					stone->queueScript(1, a.get());
					testStoneBy = a.get();
					testStoneAt = (int)world.tickcount;
					break;
				}
		if (!stone)
			fmt::print("* TEST STONE: no stone\n");
	}
	if (testStoneAt >= 0 && (int)world.tickcount == testStoneAt + 5 && testStoneBy)
		if (auto* ca = dynamic_cast<CreatureAgent*>(testStoneBy.get()))
			if (auto* c = dynamic_cast<oldCreature*>(ca->getCreature()))
				fmt::print("* TEST STONE: after chem42 {} loneliness {}\n", (int)c->getChemical(42), (int)c->getDrive(7));
	if (testStoneAt >= 0 && (int)world.tickcount == testStoneAt + 100 && testStoneBy)
		if (auto* ca = dynamic_cast<CreatureAgent*>(testStoneBy.get()))
			if (auto* c = dynamic_cast<oldCreature*>(ca->getCreature()))
				fmt::print("* TEST STONE: later chem42 {} loneliness {}\n", (int)c->getChemical(42), (int)c->getDrive(7));
	if (s_notices.empty())
		return;

	// the newest that fit in the room (at most three), the rest behind "+N"
	auto label = [](const lifeLogEntry& e, int i) { return fmt::format("{}: {}##notice{}", kindWord(e.kind), entryName(e), i); };
	const ImGuiStyle& style = ImGui::GetStyle();
	float used = ImGui::CalcTextSize("+99").x + style.FramePadding.x * 2 + style.ItemSpacing.x;
	int first = (int)s_notices.size();
	while (first > 0 && (int)s_notices.size() - first < 3) {
		float w = ImGui::CalcTextSize(label(s_notices[first - 1], first - 1).c_str(), nullptr, true).x + style.FramePadding.x * 2 + style.ItemSpacing.x;
		if (used + w > room && first < (int)s_notices.size())
			break;
		used += w;
		first--;
	}
	if (first > 0) {
		std::string more = fmt::format("+{}##morenotices", first);
		bool moreClicked = ImGui::SmallButton(more.c_str());
		// the list stays just above the button, wherever the bar's layout
		// puts it this frame
		s_more_at = ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y - 4);
		if (moreClicked) {
			// a list of the notices not shown, above the button (owner,
			// 2026-11-15: the click made them all disappear into the life log)
			ImGui::OpenPopup("##morenoticelist");
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%d older notice%s: click to list them; right-click to clear them all", first, first == 1 ? "" : "s");
		// developer aid: OPENC2E_TEST_MORE_NOTICES keeps the list open while
		// five or more notices are hidden, for a snapshot of it
		static bool testMore = devenv("OPENC2E_TEST_MORE_NOTICES") != nullptr;
		if (testMore && first >= 5 && !ImGui::IsPopupOpen("##morenoticelist"))
			ImGui::OpenPopup("##morenoticelist");
		if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
			s_notice_menu = -1;
			ImGui::OpenPopup("##noticemenu");
		}
	}
	int remove = -1;
	// one notice as a coloured button: a click follows it and clears it
	// (a click is enough for every notice, eggs too: owner, 2026-11-13, the
	// Hatchery's Find eggs is for finding eggs later); returns true when
	// clicked. full: as wide as the list it is in
	auto chip = [&](int i, bool full) {
		auto& e = s_notices[i];
		ImVec4 col = e.kind == lifeLogEntry::birth ? ImVec4(0.20f, 0.52f, 0.25f, 1) : e.kind == lifeLogEntry::death ? ImVec4(0.30f, 0.30f, 0.36f, 1)
																: e.kind == lifeLogEntry::eggLaid			? ImVec4(0.62f, 0.50f, 0.18f, 1)
																: e.kind == lifeLogEntry::unconscious		? ImVec4(0.75f, 0.16f, 0.14f, 1)
																: e.kind == lifeLogEntry::cameRound		? ImVec4(0.20f, 0.52f, 0.25f, 1)
															: e.kind == lifeLogEntry::changed			? ImVec4(0.16f, 0.44f, 0.50f, 1)
															: e.kind == lifeLogEntry::together			? ImVec4(0.42f, 0.36f, 0.56f, 1)
																										  : ImVec4(0.70f, 0.30f, 0.50f, 1);
		ImGui::PushStyleColor(ImGuiCol_Button, col);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(col.x + 0.12f, col.y + 0.12f, col.z + 0.12f, 1));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(col.x + 0.2f, col.y + 0.2f, col.z + 0.2f, 1));
		bool clicked = full ? ImGui::Button(label(e, i).c_str(), ImVec2(-FLT_MIN, 0)) : ImGui::SmallButton(label(e, i).c_str());
		ImGui::PopStyleColor(3);
		if (clicked) {
			followEntry(e);
			remove = i;
		}
		if (ImGui::IsItemHovered()) {
			// what a click does (followEntry)
			const char* does = e.kind == lifeLogEntry::together ? "go to the memorial stone and read it"
							   : e.kind == lifeLogEntry::eggLaid ? "go to the egg (or the baby, once it hatches)"
							   : e.kind == lifeLogEntry::death ? "open its page in the Graveyard Kit"
							   : e.kind == lifeLogEntry::pregnancy ? "go to it and open the Breeder's Kit"
							   : e.kind == lifeLogEntry::birth ? "go to it and open the Owner's Kit"
															   : "go to it";
			ImGui::SetTooltip("%s %s\n%s\nClick to %s; right-click to dismiss it.",
				kindWord(e.kind), KitDateText(e.tick).c_str(), entryDetail(e).c_str(), does);
		}
		return clicked;
	};
	for (int i = first; i < (int)s_notices.size(); i++) {
		chip(i, false);
		if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
			s_notice_menu = i;
			ImGui::OpenPopup("##noticemenu");
		}
	}
	// the "+N" list: every notice not shown on the bar, newest first, in a
	// window above the button that scrolls when long; each clickable as on
	// the bar, right-click to dismiss one
	ImGui::SetNextWindowPos(s_more_at, ImGuiCond_Always, ImVec2(0, 1));
	if (ImGui::BeginPopup("##morenoticelist", ImGuiWindowFlags_AlwaysAutoResize)) {
		int hidden = std::min(first, (int)s_notices.size());
		ImGui::TextDisabled("%d older notice%s", hidden, hidden == 1 ? "" : "s");
		float rowH = ImGui::GetFrameHeightWithSpacing();
		// as wide as the longest notice, no wider (owner, 2026-11-15: it was
		// a fixed 420 px, about twice what it needed), with room for the
		// scroll bar when the list scrolls
		float listH = std::min(300.0f, rowH * hidden + 4);
		float w = ImGui::CalcTextSize("Open the life log").x + ImGui::CalcTextSize("Clear these").x + style.FramePadding.x * 4 + style.ItemSpacing.x;
		for (int i = 0; i < hidden; i++)
			w = std::max(w, ImGui::CalcTextSize(label(s_notices[i], i).c_str(), nullptr, true).x + style.FramePadding.x * 2 + 8);
		if (rowH * hidden + 4 > listH)
			w += style.ScrollbarSize;
		ImGui::BeginChild("##morescroll", ImVec2(w, listH), ImGuiChildFlags_None);
		for (int i = hidden - 1; i >= 0; i--) {
			ImGui::PushID(i);
			if (chip(i, true))
				ImGui::CloseCurrentPopup();
			if (ImGui::BeginPopupContextItem("##dismissone")) {
				if (ImGui::MenuItem("Dismiss"))
					remove = i;
				ImGui::EndPopup();
			}
			ImGui::PopID();
		}
		ImGui::EndChild();
		ImGui::Separator();
		if (ImGui::SmallButton("Open the life log")) {
			OpenC2KitAt("owners", "Life log");
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Clear these")) {
			s_clear_older = hidden;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
	// developer aid: OPENC2E_TEST_NOTICE_MENU opens the menu on the newest
	// notice once, for a snapshot of it
	static bool testMenu = devenv("OPENC2E_TEST_NOTICE_MENU") != nullptr;
	if (testMenu && world.tickcount > 30) {
		testMenu = false;
		s_notice_menu = (int)s_notices.size() - 1;
		ImGui::OpenPopup("##noticemenu");
	}
	// right-click on a notice: dismiss it, or clear them all (owner, 2026-11-13)
	bool clearAll = false;
	if (ImGui::BeginPopup("##noticemenu")) {
		if (s_notice_menu >= 0 && s_notice_menu < (int)s_notices.size() && ImGui::MenuItem("Dismiss"))
			remove = s_notice_menu;
		if (ImGui::MenuItem("Clear all"))
			clearAll = true;
		ImGui::EndPopup();
	}
	if (clearAll) {
		s_notices.clear();
		return;
	}
	if (s_clear_older > 0) {
		s_notices.erase(s_notices.begin(), s_notices.begin() + std::min(s_clear_older, (int)s_notices.size()));
		s_clear_older = 0;
		return;
	}
	if (remove >= 0)
		s_notices.erase(s_notices.begin() + remove);
	// egg notices go once the egg has hatched (the life log keeps them);
	// checked once a world tick, when an egg can hatch, not every frame: each
	// check scans every object and every record (performance, 2026-11-16)
	static unsigned int s_eggs_checked_tick = (unsigned int)-1;
	if (s_eggs_checked_tick == world.tickcount)
		return;
	s_eggs_checked_tick = world.tickcount;
	s_notices.erase(std::remove_if(s_notices.begin(), s_notices.end(), [](const lifeLogEntry& e) {
		return e.kind == lifeLogEntry::eggLaid && !e.child.empty() && !KitEggOf(e.child) && KitHatchedFrom(e.child);
	}), s_notices.end());
}

// ---- Family tree, with each member's photos ---------------------------------------

static monikerData* recordOf(const std::string& mon) {
	return !mon.empty() && world.history->hasMoniker(mon) ? &world.history->getMoniker(mon) : nullptr;
}

static std::shared_ptr<Agent> bodyOf(monikerData* m) {
	Agent* a = m ? m->owner.get() : nullptr;
	if (!a || a->isDying() || !dynamic_cast<CreatureAgent*>(a))
		return nullptr;
	return AgentRef(a).lock();
}

static std::string recordName(monikerData* m) {
	return m->name.empty() ? fmt::format("Unnamed {}", genusWord(m->genus)) : m->name;
}

// "female adult" (at death, for the dead), then its age or when it died
static std::pair<std::string, std::string> recordStatus(monikerData* m) {
	auto body = bodyOf(m);
	c2Creature* c = body ? KitC2Of(body.get()) : nullptr;
	bool female = c ? c->isFemale() : m->gender == 2;
	const char* sex = c || m->gender > 0 ? (female ? "female " : "male ") : "";
	if (m->died)
		return {fmt::format("died: {}{}", sex, stageWord(m->diedstage)), KitDateText(m->diedtick)};
	if (c) {
		std::string age = KitAgeText(KitAgeTicks(*m));
		age = age.substr(0, age.find(" ("));
		return {fmt::format("{}{}", sex, stageWord((int)c->getStage())), (m->bornknown ? "age " : "at least ") + age};
	}
	return {"no longer in the world", ""};
}

static std::string fitText(std::string t, float w) {
	if (ImGui::CalcTextSize(t.c_str()).x <= w)
		return t;
	while (!t.empty() && ImGui::CalcTextSize((t + "..").c_str()).x > w)
		t.pop_back();
	return t + "..";
}

// The tree is laid out as cards in rows, one row per generation. A creature
// shows up once however many ways it is related (a mother who is also the
// father's mother): its card says every way, and lines from it go to each
// of its children shown.
static const float CARD_W = 184, CARD_GAP = 14, ROW_GAP = 34, THUMB_W = 56, THUMB_H = 42, LINE_H = 15, PAD = 5;

struct FamilyCard {
	std::string mon; // empty for an unknown parent
	std::vector<std::string> roles; // how it is related, in order
	int gen = 0; // generations above (positive) or below this creature
	float want = 0; // where it would like to be (card middle, in columns)
	int row = 0; // visual row (top first)
	ImVec2 at, size; // relative to the tree's top-left
	bool self = false;
	float mid() const { return at.x + size.x / 2; }
};

// the tree's zoom (mouse wheel, middle-drag to pan, the slider)
static float s_treeScale = 1;
static ImVec2 sc(ImVec2 v, float k) { return ImVec2(v.x * k, v.y * k); }

// the zoomed-in photo, drawn over the tree
static std::string s_zoom; // whose photo; empty when not zoomed
static int s_zoomPhoto = 0;
static double s_zoomStart = 0;
static ImVec2 s_zoomFrom0, s_zoomFrom1; // the thumbnail it grows out of (screen)
static float s_zoomTreeScroll = 0;

static void zoomTo(const std::string& mon, int photo, ImVec2 from0, ImVec2 from1) {
	s_zoom = mon;
	s_zoomPhoto = photo;
	s_zoomStart = ImGui::GetTime();
	s_zoomFrom0 = from0;
	s_zoomFrom1 = from1;
}

static int femaleOf(monikerData* m) { // 1 female, 0 male, -1 not known
	if (!m)
		return -1;
	auto body = bodyOf(m);
	if (c2Creature* c = body ? KitC2Of(body.get()) : nullptr)
		return c->isFemale() ? 1 : 0;
	return m->gender > 0 ? (m->gender == 2 ? 1 : 0) : -1;
}

static std::string roleText(const std::vector<std::string>& roles) {
	if (roles.empty())
		return "";
	auto has = [&](const char* r) { return std::find(roles.begin(), roles.end(), r) != roles.end(); };
	if (roles.size() == 2 && has("Mother") && has("Father"))
		return "Cloned from";
	if (roles.size() == 2 && has("Mother's mother") && has("Father's mother"))
		return "Grandmother on both sides";
	if (roles.size() == 2 && has("Mother's father") && has("Father's father"))
		return "Grandfather on both sides";
	std::string t = roles[0];
	for (size_t i = 1; i < roles.size(); i++) {
		std::string r = roles[i];
		r[0] = (char)std::tolower((unsigned char)r[0]);
		t += (i == 1 ? ", also " : " and ") + r;
	}
	return t;
}

// the card's text: name and relation beside the thumbnail, then its life
struct CardText {
	std::string name, role, status, when;
	float roleH = LINE_H;
};

// (measured at the tree's zoom k, with the font scaled to match)
static CardText cardText(const FamilyCard& c, float k = 1) {
	CardText t;
	t.role = roleText(c.roles);
	t.roleH = std::max(LINE_H * k, ImGui::CalcTextSize(t.role.c_str(), nullptr, false, (CARD_W - THUMB_W - 3 * PAD) * k).y);
	if (monikerData* m = recordOf(c.mon)) {
		t.name = recordName(m);
		auto [status, when] = recordStatus(m);
		t.status = m->died && status.rfind("died: ", 0) == 0 ? status.substr(6) : status; // red says it died
		// "Summer, day 4, year 002" in short: "Summer 4, yr 002"
		std::string d = when;
		if (auto k = d.find(", day "); k != std::string::npos)
			d.replace(k, 6, " ");
		if (auto k = d.find(", year "); k != std::string::npos)
			d.replace(k, 7, ", yr ");
		t.when = m->died ? "died " + d : when;
	}
	return t;
}

static ImVec2 cardSize(const FamilyCard& c) {
	CardText t = cardText(c);
	if (!recordOf(c.mon)) // unknown: the relation, then "unknown"
		return ImVec2(CARD_W, PAD * 2 + t.roleH + LINE_H);
	float top = std::max(THUMB_H, LINE_H + t.roleH);
	return ImVec2(CARD_W, PAD + top + PAD + 2 * LINE_H + PAD);
}

// one card: a small photo (click to zoom in), name, relation, life; drawn
// at the tree's zoom k (the child window's font is scaled to match)
static void drawCard(const FamilyCard& c, ImVec2 o, bool interactive, float k) {
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImU32 gray = IM_COL32(150, 150, 150, 255), gold = IM_COL32(205, 180, 125, 255);
	monikerData* m = recordOf(c.mon);
	ImVec2 at = o + sc(c.at, k), end = at + sc(c.size, k);
	dl->AddRectFilled(at, end, IM_COL32(30, 30, 34, 255), 4 * k);
	dl->AddRect(at, end, c.self ? IM_COL32(255, 220, 120, 255) : IM_COL32(90, 90, 100, 255), 4 * k, 0, std::max(1.0f, (c.self ? 2.0f : 1.0f) * k));
	CardText t = cardText(c, k);
	ImFont* font = ImGui::GetFont();
	float fs = ImGui::GetFontSize();
	const float pad = PAD * k, line = LINE_H * k, tw = THUMB_W * k, th = THUMB_H * k, cw = CARD_W * k;
	if (!m) {
		dl->AddText(font, fs, at + ImVec2(pad + 1, pad), gold, t.role.c_str(), nullptr, cw - 2 * pad);
		dl->AddText(font, fs, at + ImVec2(pad + 1, pad + t.roleH), gray, "unknown");
		return;
	}
	ImGui::PushID(c.mon.c_str());
	int n = (int)m->photos.size();
	ImVec2 tp = at + ImVec2(pad, pad);
	Texture tex = n ? KitPhotoTexture(engine.currentworlddir / m->photos[n - 1]) : Texture();
	if (tex) {
		dl->AddImage(tex.as<ImTextureID>(), tp, tp + ImVec2(tw, th));
		if (n > 1) { // how many there are
			std::string count = std::to_string(n);
			ImVec2 cs = ImGui::CalcTextSize(count.c_str());
			ImVec2 cp = tp + ImVec2(tw - cs.x - 3 * k, th - cs.y - 1 * k);
			dl->AddRectFilled(cp - ImVec2(3 * k, 1 * k), cp + cs + ImVec2(2 * k, 0), IM_COL32(0, 0, 0, 170), 3 * k);
			dl->AddText(font, fs, cp, IM_COL32(235, 235, 235, 255), count.c_str());
		}
	} else {
		dl->AddRectFilled(tp, tp + ImVec2(tw, th), IM_COL32(18, 18, 20, 255));
		const char* none = n ? "missing" : "none";
		ImVec2 ns = ImGui::CalcTextSize(none);
		dl->AddText(font, fs, tp + ImVec2((tw - ns.x) / 2, (th - ns.y) / 2), IM_COL32(110, 110, 110, 255), none);
	}
	dl->AddRect(tp, tp + ImVec2(tw, th), IM_COL32(70, 70, 78, 255));
	if (interactive) {
		ImGui::SetCursorScreenPos(tp);
		if (ImGui::InvisibleButton("photo", ImVec2(tw, th)) && tex)
			zoomTo(c.mon, n - 1, tp, tp + ImVec2(tw, th));
		if (ImGui::IsItemHovered()) {
			if (tex)
				ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
			ImGui::SetTooltip(tex ? "%s: click to zoom in (%d photo%s)" : "%s: no photos yet", t.name.c_str(), n, n == 1 ? "" : "s");
		}
	}
	auto body = bodyOf(m);
	bool go = interactive && body && !c.self && !m->died;
	float tx = pad * 2 + tw;
	float goW = go ? ImGui::CalcTextSize("Go").x + ImGui::GetStyle().FramePadding.x * 2 + 4 * k : 0;
	dl->AddText(font, fs, at + ImVec2(tx, pad), m->died ? IM_COL32(200, 200, 200, 255) : IM_COL32(240, 240, 240, 255),
		fitText(t.name, cw - tx - pad - goW).c_str());
	dl->AddText(font, fs, at + ImVec2(tx, pad + line), gold, t.role.c_str(), nullptr, cw - tx - pad);
	float y = pad + std::max(th, line + t.roleH) + pad;
	ImU32 sub = m->died ? IM_COL32(210, 120, 120, 255) : gray;
	dl->AddText(font, fs, at + ImVec2(pad + 1, y), sub, fitText(t.status, cw - 2 * pad).c_str());
	dl->AddText(font, fs, at + ImVec2(pad + 1, y + line), sub, fitText(t.when, cw - 2 * pad).c_str());
	if (go) {
		ImGui::SetCursorScreenPos(at + ImVec2(cw - pad - goW + 4 * k, pad - 2 * k));
		if (ImGui::SmallButton("Go"))
			KitGoTo(body);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Select %s and go to it", t.name.c_str());
	}
	ImGui::PopID();
}

struct FamilyLayout {
	std::vector<FamilyCard> cards;
	std::vector<std::pair<int, int>> edges; // parent card, child card
	std::vector<float> rowTop, rowBottom;
	float width = 0, height = 0;
};

static FamilyLayout layOutFamily(const std::string& self) {
	FamilyLayout L;
	auto& cards = L.cards;
	std::map<std::string, int> index;
	const float col = CARD_W + CARD_GAP;
	// a known creature gets one card, however many roles; an unknown parent
	// gets its own
	auto add = [&](const std::string& mon, const std::string& role, float want) -> int {
		if (!mon.empty() && index.count(mon)) {
			FamilyCard& c = cards[index[mon]];
			c.roles.push_back(role);
			c.want = (c.want * (c.roles.size() - 1) + want) / c.roles.size(); // between its places
			return index[mon];
		}
		FamilyCard c;
		c.mon = recordOf(mon) ? mon : "";
		c.roles = {role};
		c.want = want;
		cards.push_back(c);
		if (!c.mon.empty())
			index[mon] = (int)cards.size() - 1;
		return (int)cards.size() - 1;
	};
	std::vector<std::pair<int, int>> unknownOf; // an unknown parent's card, the child's
	auto parentsOf = [](const std::string& mon) -> std::pair<std::string, std::string> {
		monikerData* r = recordOf(mon);
		auto known = [](const std::string& p) { return recordOf(p) ? p : std::string(); };
		return r ? std::make_pair(known(r->mother), known(r->father)) : std::make_pair(std::string(), std::string());
	};
	int me = add(self, "This creature", 2.0f);
	cards[me].self = true;
	// parents and grandparents; when only one of a pair is known, the other
	// shows as unknown
	auto [mother, father] = parentsOf(self);
	auto addPair = [&](const std::string& mo, const std::string& fa, const char* mrole, const char* frole, float mw, float fw, int child) {
		if (mo.empty() && fa.empty())
			return;
		int a = add(mo, mrole, mw), b = add(fa, frole, fw);
		if (cards[a].mon.empty())
			unknownOf.push_back({a, child});
		if (cards[b].mon.empty())
			unknownOf.push_back({b, child});
	};
	addPair(mother, father, "Mother", "Father", 1.0f, 3.0f, me);
	if (!mother.empty()) {
		auto [mm, mf] = parentsOf(mother);
		addPair(mm, mf, "Mother's mother", "Mother's father", 0.5f, 1.5f, index[mother]);
	}
	if (!father.empty()) {
		auto [fm, ff] = parentsOf(father);
		addPair(fm, ff, "Father's mother", "Father's father", 2.5f, 3.5f, index[father]);
	}
	// children, oldest first
	std::vector<std::string> children;
	for (auto& [mon, r] : world.history->all())
		if (!mon.empty() && mon != self && (r.mother == self || r.father == self))
			children.push_back(mon);
	std::sort(children.begin(), children.end(), [](auto& x, auto& y) { return recordOf(x)->borntick < recordOf(y)->borntick; });
	for (auto& ch : children) {
		int f = femaleOf(recordOf(ch));
		add(ch, f == 1 ? "Daughter" : f == 0 ? "Son" : "Child", 2.0f);
	}
	for (auto& ch : children) { // a child of a child here is also a grandchild
		monikerData* r = recordOf(ch);
		if (std::find(children.begin(), children.end(), r->mother) != children.end() || std::find(children.begin(), children.end(), r->father) != children.end()) {
			int f = femaleOf(r);
			cards[index[ch]].roles.push_back(f == 1 ? "Granddaughter" : f == 0 ? "Grandson" : "Grandchild");
		}
	}

	// the lines: every parent shown to every child shown
	for (size_t i = 0; i < cards.size(); i++) {
		monikerData* r = recordOf(cards[i].mon);
		if (!r)
			continue;
		for (auto& p : {r->mother, r->father})
			if (!p.empty() && p != cards[i].mon && index.count(p))
				L.edges.push_back({index[p], (int)i});
	}
	for (auto& [u, child] : unknownOf)
		L.edges.push_back({u, child});

	// generations: an ancestor sits just above its lowest child shown, a
	// descendant just below its lowest parent shown
	std::set<int> below; // this creature's descendants
	for (auto& ch : children)
		below.insert(index[ch]);
	const int none = INT_MIN;
	std::vector<int> gen(cards.size(), none);
	gen[me] = 0;
	for (size_t pass = 0; pass <= cards.size(); pass++)
		for (auto& [p, c] : L.edges) {
			if (gen[c] != none && !below.count(c) && !below.count(p))
				gen[p] = std::max(gen[p] == none ? INT_MIN + 1 : gen[p], gen[c] + 1);
			if (gen[p] != none && below.count(c))
				gen[c] = gen[c] == none ? gen[p] - 1 : std::min(gen[c], gen[p] - 1);
		}
	for (size_t i = 0; i < cards.size(); i++)
		cards[i].gen = gen[i] == none ? 0 : gen[i];

	// rows, top first; this creature's own children go four to a row
	int top = 0, bottom = 0;
	for (auto& c : cards) {
		top = std::max(top, c.gen);
		bottom = std::min(bottom, c.gen);
	}
	int row = 0;
	std::vector<std::vector<int>> rows;
	for (int g = top; g >= bottom; g--) {
		std::vector<int> in;
		for (size_t i = 0; i < cards.size(); i++)
			if (cards[i].gen == g)
				in.push_back((int)i);
		if (in.empty())
			continue;
		if (g == -1) { // children: in rows of four, centered
			for (size_t k = 0; k < in.size(); k += 4) {
				std::vector<int> part(in.begin() + k, in.begin() + std::min(in.size(), k + 4));
				for (size_t j = 0; j < part.size(); j++)
					cards[part[j]].want = 2.0f - (part.size() - 1) / 2.0f + j;
				rows.push_back(part);
			}
			continue;
		}
		if (g < -1) // further down: under their parents
			for (int i : in) {
				float sum = 0;
				int n = 0;
				for (auto& [p, c] : L.edges)
					if (c == i) {
						sum += cards[p].want;
						n++;
					}
				if (n)
					cards[i].want = sum / n;
			}
		rows.push_back(in);
	}
	// place each row: sorted by where its cards want to be, never
	// overlapping, shifted as little as possible
	float minX = 0, maxX = 4 * col - CARD_GAP;
	for (auto& r : rows) {
		std::sort(r.begin(), r.end(), [&](int a, int b) { return cards[a].want < cards[b].want; });
		std::vector<float> x(r.size());
		for (size_t k = 0; k < r.size(); k++) {
			x[k] = cards[r[k]].want * col - CARD_W / 2;
			if (k)
				x[k] = std::max(x[k], x[k - 1] + col);
		}
		float shift = 0;
		for (size_t k = 0; k < r.size(); k++)
			shift += (cards[r[k]].want * col - CARD_W / 2) - x[k];
		shift /= r.size();
		for (size_t k = 0; k < r.size(); k++) {
			cards[r[k]].at.x = x[k] + shift;
			minX = std::min(minX, cards[r[k]].at.x);
			maxX = std::max(maxX, cards[r[k]].at.x + CARD_W);
		}
	}
	float y = 0;
	for (auto& r : rows) {
		float h = 0;
		for (int i : r) {
			cards[i].row = row;
			cards[i].size = cardSize(cards[i]);
			cards[i].at.x -= minX;
			cards[i].at.y = y;
			h = std::max(h, cards[i].size.y);
		}
		L.rowTop.push_back(y);
		L.rowBottom.push_back(y + h);
		y += h + ROW_GAP;
		row++;
	}
	L.width = maxX - minX;
	L.height = y - ROW_GAP;
	return L;
}

// a line from a parent's card down to a child's, going around any cards in
// the rows between
static void familyLine(const FamilyLayout& L, const FamilyCard& p, const FamilyCard& c, ImVec2 o, float k) {
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImU32 col = IM_COL32(170, 150, 110, 255);
	float ax = p.mid(), bx = c.mid();
	float below = (L.rowBottom[p.row] + L.rowTop[p.row + 1]) / 2; // the gap under the parent's row
	float above = (L.rowBottom[c.row - 1] + L.rowTop[c.row]) / 2; // the gap over the child's
	auto clear = [&](float x) {
		for (auto& k : L.cards)
			if (k.row > p.row && k.row < c.row && x > k.at.x - 6 && x < k.at.x + k.size.x + 6)
				return false;
		return true;
	};
	std::vector<ImVec2> pts = {ImVec2(ax, p.at.y + p.size.y)};
	if (clear(ax)) {
		pts.push_back(ImVec2(ax, above));
	} else if (clear(bx)) {
		pts.push_back(ImVec2(ax, below));
		pts.push_back(ImVec2(bx, below));
	} else { // a lane between cards, nearest the middle
		float best = 0, bestd = 1e9f, want = (ax + bx) / 2;
		std::vector<float> lanes = {-CARD_GAP / 2, L.width + CARD_GAP / 2};
		for (auto& k : L.cards) {
			lanes.push_back(k.at.x - CARD_GAP / 2);
			lanes.push_back(k.at.x + k.size.x + CARD_GAP / 2);
		}
		for (float x : lanes)
			if (clear(x) && std::abs(x - want) < bestd) {
				best = x;
				bestd = std::abs(x - want);
			}
		pts.push_back(ImVec2(ax, below));
		pts.push_back(ImVec2(best, below));
		pts.push_back(ImVec2(best, above));
	}
	pts.push_back(ImVec2(bx, pts.back().y));
	pts.push_back(ImVec2(bx, c.at.y));
	for (auto& q : pts)
		q = o + sc(q, k);
	dl->AddPolyline(pts.data(), (int)pts.size(), col, 0, std::max(1.0f, 2 * k));
}

// the zoomed-in photo over the tree: it grows out of the thumbnail; arrows
// and the row of thumbnails page through them; click outside it or Back to
// return
static void familyZoom(ImVec2 r0, ImVec2 r1) {
	monikerData* m = recordOf(s_zoom);
	if (!m || m->photos.empty()) {
		s_zoom.clear();
		return;
	}
	ImDrawList* dl = ImGui::GetWindowDrawList();
	int n = (int)m->photos.size();
	s_zoomPhoto = std::clamp(s_zoomPhoto, 0, n - 1);
	float t = std::clamp((float)((ImGui::GetTime() - s_zoomStart) / 0.2), 0.0f, 1.0f);
	float e = 1 - (1 - t) * (1 - t) * (1 - t); // ease out
	dl->AddRectFilled(r0, r1, IM_COL32(12, 12, 14, (int)(225 * e)));
	// where the photo ends up: as big as fits, leaving room for the caption
	// and the row of thumbnails
	const float capH = 26, stripH = n > 1 ? 50 : 0;
	ImVec2 room = (r1 - r0) - ImVec2(24, 24 + capH + stripH + 28);
	float scale = std::min({room.x / PHOTO_W, room.y / PHOTO_H, 3.0f});
	ImVec2 sz(PHOTO_W * scale, PHOTO_H * scale);
	ImVec2 p0(r0.x + ((r1.x - r0.x) - sz.x) / 2, r0.y + 36);
	auto lerp = [e](ImVec2 from, ImVec2 to) { return ImVec2(from.x + (to.x - from.x) * e, from.y + (to.y - from.y) * e); };
	ImVec2 a = lerp(s_zoomFrom0, p0), b = lerp(s_zoomFrom1, p0 + sz);
	Texture tex = KitPhotoTexture(engine.currentworlddir / m->photos[s_zoomPhoto]);
	if (tex)
		dl->AddImage(tex.as<ImTextureID>(), a, b);
	else {
		dl->AddRectFilled(a, b, IM_COL32(18, 18, 20, 255));
		dl->AddText(a + ImVec2(8, 8), IM_COL32(150, 150, 150, 255), "(this photo's file is missing)");
	}
	dl->AddRect(a - ImVec2(1, 1), b + ImVec2(1, 1), IM_COL32(205, 180, 125, (int)(255 * e)), 0, 0, 2);
	if (t < 1)
		return;
	// the title and Back
	ImGui::SetCursorScreenPos(r0 + ImVec2(12, 9));
	ImGui::TextColored(ImVec4(1, 0.9f, 0.7f, 1), "%s", recordName(m).c_str());
	ImGui::SetCursorScreenPos(ImVec2(r1.x - 132, r0.y + 6));
	bool back = ImGui::Button("Back to the tree", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape);
	// the caption, with arrows either side
	std::string cap = fmt::format("Photo {} of {}, taken {}", s_zoomPhoto + 1, n, KitDateText(photoTick(m->photos[s_zoomPhoto])));
	float cw = ImGui::CalcTextSize(cap.c_str()).x;
	float cy = p0.y + sz.y + 8;
	float cx = r0.x + ((r1.x - r0.x) - cw) / 2;
	if (n > 1) {
		ImGui::SetCursorScreenPos(ImVec2(cx - 34, cy - 2));
		if (ImGui::ArrowButton("prev", ImGuiDir_Left) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
			s_zoomPhoto = (s_zoomPhoto + n - 1) % n;
		ImGui::SetCursorScreenPos(ImVec2(cx + cw + 12, cy - 2));
		if (ImGui::ArrowButton("next", ImGuiDir_Right) || ImGui::IsKeyPressed(ImGuiKey_RightArrow))
			s_zoomPhoto = (s_zoomPhoto + 1) % n;
	}
	dl->AddText(ImVec2(cx, cy), IM_COL32(225, 225, 225, 255), cap.c_str());
	// all of them, small
	if (n > 1) {
		const float tw = 52, th = 39, gap = 6;
		int fit = std::max(1, (int)((r1.x - r0.x - 24 + gap) / (tw + gap)));
		int first = std::clamp(s_zoomPhoto - fit / 2, 0, std::max(0, n - fit));
		int shown = std::min(n, fit);
		float sx = r0.x + ((r1.x - r0.x) - (shown * (tw + gap) - gap)) / 2;
		for (int i = first; i < first + shown; i++) {
			ImVec2 q(sx + (i - first) * (tw + gap), cy + capH);
			Texture th2 = KitPhotoTexture(engine.currentworlddir / m->photos[i]);
			if (th2)
				dl->AddImage(th2.as<ImTextureID>(), q, q + ImVec2(tw, th));
			else
				dl->AddRectFilled(q, q + ImVec2(tw, th), IM_COL32(40, 40, 44, 255));
			dl->AddRect(q - ImVec2(1, 1), q + ImVec2(tw + 1, th + 1), i == s_zoomPhoto ? IM_COL32(255, 220, 120, 255) : IM_COL32(80, 80, 88, 255), 0, 0, i == s_zoomPhoto ? 2.0f : 1.0f);
			ImGui::SetCursorScreenPos(q);
			ImGui::PushID(i);
			if (ImGui::InvisibleButton("thumb", ImVec2(tw, th)))
				s_zoomPhoto = i;
			ImGui::PopID();
		}
	}
	// a click anywhere else on the dimmed tree goes back too
	bool outside = ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsMouseHoveringRect(r0, r1) && !ImGui::IsAnyItemHovered() && !ImGui::IsMouseHoveringRect(p0, p0 + sz);
	if (back || outside)
		s_zoom.clear();
}

static void FamilyPage() {
	if (!KitNeedsCreature())
		return;
	Agent* a = KitCreatureAgent();
	monikerData* m = KitRecordsOf(a);
	if (!m)
		return;
	std::string self = m->getName();
	FamilyLayout L = layOutFamily(self);
	// developer aid: OPENC2E_TEST_ALBUM=1 zooms in on this creature's photo
	static bool testAlbum = devenv("OPENC2E_TEST_ALBUM") != nullptr;
	if (testAlbum && !m->photos.empty()) {
		testAlbum = false;
		zoomTo(self, (int)m->photos.size() - 1, ImVec2(), ImVec2());
		s_zoomStart = -10; // already grown
	}
	// developer aid: OPENC2E_TEST_TREE_ZOOM=60 starts the tree at 60%
	static bool zoomEnv = true;
	if (zoomEnv) {
		zoomEnv = false;
		if (const char* z = devenv("OPENC2E_TEST_TREE_ZOOM"))
			s_treeScale = std::clamp(atoi(z) / 100.0f, 0.25f, 2.0f);
	}
	if (!s_zoom.empty() && !recordOf(s_zoom))
		s_zoom.clear();
	bool zoomed = !s_zoom.empty();
	bool parents = false;
	for (auto& c : L.cards)
		if (c.gen > 0)
			parents = true;
	bool children = false;
	for (auto& c : L.cards)
		if (c.gen < 0)
			children = true;
	bool engineered = false;
	for (auto& e : m->events)
		if (e.eventno == 2)
			engineered = true;

	// the view: as big as the tree at 100% up to what the screen allows,
	// then scrollbars; the zoom changes what is in it, not its size (and a
	// zoomed-in photo wants at least a good-sized view)
	const float k = s_treeScale;
	float height = L.height + (children ? 0 : 24);
	ImGuiIO& io = ImGui::GetIO();
	float room = std::max(300.0f, io.DisplaySize.y - 300);
	float viewW = std::clamp(L.width, 4 * (CARD_W + CARD_GAP) - CARD_GAP, std::max(780.0f, io.DisplaySize.x - 160));
	float viewH = std::min(height, room);
	if (zoomed)
		viewH = std::max(viewH, std::min(room, 470.0f));
	float contentW = std::max(L.width * k, viewW), contentH = height * k;
	const float bar = ImGui::GetStyle().ScrollbarSize;
	bool barX = contentW > viewW + 0.5f, barY = contentH > viewH + 0.5f;
	ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_HorizontalScrollbar;
	ImGui::BeginChild("tree", ImVec2(viewW + (barY ? bar : 0), viewH + (barX ? bar : 0)), false, flags);
	if (zoomed)
		ImGui::SetScrollY(s_zoomTreeScroll);
	ImGui::SetWindowFontScale(k);
	ImVec2 start = ImGui::GetCursorScreenPos();
	ImVec2 o = start + ImVec2((contentW - L.width * k) / 2, 0);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	for (auto& [p, c] : L.edges) {
		const FamilyCard& pc = L.cards[p];
		const FamilyCard& cc = L.cards[c];
		if (cc.row <= pc.row)
			continue;
		// this creature's children past the first row of them go without
		// lines, which would only cross the cards above them
		if (pc.self && cc.gen == -1 && cc.row > pc.row + 1)
			continue;
		familyLine(L, pc, cc, o, k);
	}
	for (auto& c : L.cards)
		drawCard(c, o, !zoomed, k);
	for (auto& c : L.cards)
		if (c.self) {
			ImVec2 q = o + sc(c.at, k);
			if (!parents) {
				std::string why = engineered ? "No parents: it hatched from an egg the Hatchery or Breeder's Kit laid." : "No parents recorded.";
				float w = std::min(c.at.x - 16, 240.0f) * k;
				if (w > 80 * k)
					dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(q.x - w - 12 * k, q.y + 4 * k), IM_COL32(140, 140, 140, 255), why.c_str(), nullptr, w);
			}
			if (!children)
				dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), q + ImVec2(0, (c.size.y + 8) * k), IM_COL32(140, 140, 140, 255), "No children yet.");
		}
	ImGui::SetCursorScreenPos(start);
	ImGui::Dummy(ImVec2(contentW, contentH));
	ImGui::SetWindowFontScale(1);
	if (zoomed) {
		ImVec2 r0 = ImGui::GetWindowPos(), r1 = r0 + ImGui::GetWindowSize();
		familyZoom(r0, r1);
	} else {
		s_zoomTreeScroll = ImGui::GetScrollY();
		bool over = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
		// the mouse wheel zooms, keeping the spot under the pointer in place
		if (over && io.MouseWheel != 0) {
			float nk = std::clamp(std::round(k * (io.MouseWheel > 0 ? 1.15f : 1 / 1.15f) * 20) / 20, 0.25f, 2.0f);
			if (nk != k) {
				ImVec2 view0 = ImGui::GetWindowPos() + ImGui::GetStyle().WindowPadding;
				ImVec2 mouse = io.MousePos - view0;
				ImVec2 scroll(ImGui::GetScrollX(), ImGui::GetScrollY());
				// the point under the pointer, in tree units
				float oldX = (contentW - L.width * k) / 2, newContentW = std::max(L.width * nk, viewW), newX = (newContentW - L.width * nk) / 2;
				ImVec2 tree((mouse.x + scroll.x - oldX) / k, (mouse.y + scroll.y) / k);
				ImGui::SetScrollX(std::max(0.0f, tree.x * nk + newX - mouse.x));
				ImGui::SetScrollY(std::max(0.0f, tree.y * nk - mouse.y));
				s_treeScale = nk;
			}
		}
		// the middle button drags the tree around
		static bool panning = false;
		if (over && ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
			panning = true;
		if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle))
			panning = false;
		if (panning) {
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
			ImGui::SetScrollX(ImGui::GetScrollX() - io.MouseDelta.x);
			ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
		}
	}
	ImGui::EndChild();
	// a hint, and the zoom as a percentage on the right
	ImGui::TextDisabled(zoomed ? "Arrows or thumbnails: other photos. Back, Esc or a click outside: the tree."
							   : "Click a photo to see it big. Wheel: zoom; middle-drag: move.");
	if (!zoomed) {
		const float sliderW = 150;
		ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - sliderW - 50));
		if (ImGui::SmallButton("Fit")) { // the whole tree in view
			float fit = std::min(viewW / L.width, viewH / height);
			s_treeScale = std::clamp(std::floor(fit * 20) / 20, 0.25f, 2.0f);
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Zoom so the whole tree fits");
		ImGui::SameLine();
		int pct = (int)std::lround(s_treeScale * 100);
		ImGui::SetNextItemWidth(sliderW);
		if (ImGui::SliderInt("##treezoom", &pct, 25, 200, "%d%%"))
			s_treeScale = pct / 100.0f;
	}
}

// A trait's bar: its own color filling to where the creature stands, a mark
// at the middle (average), nothing judged good or bad. Colors from the
// commonest color-feeling associations, kept apart for color-blind eyes:
// temper red (anger), energy yellow (activity), steadiness blue (calm),
// boldness orange, curiosity teal (the new and open), sociability pink
// (warmth), reactivity purple (nervous arousal).
void TraitBar(int trait, int percent, const char* label, float width) {
	static const ImU32 colors[NUM_TRAITS] = {
		IM_COL32(235, 140, 40, 255), // boldness
		IM_COL32(40, 175, 170, 255), // curiosity
		IM_COL32(230, 200, 40, 255), // energy
		IM_COL32(230, 110, 170, 255), // sociability
		IM_COL32(215, 55, 50, 255), // temper
		IM_COL32(150, 90, 210, 255), // reactivity
		IM_COL32(70, 120, 220, 255), // steadiness
	};
	const float h = ImGui::GetTextLineHeight() + 6;
	ImVec2 p = ImGui::GetCursorScreenPos();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), IM_COL32(45, 45, 50, 255), 3);
	dl->AddRectFilled(p, ImVec2(p.x + width * std::clamp(percent, 0, 100) / 100.0f, p.y + h), colors[trait], 3);
	dl->AddLine(ImVec2(p.x + width / 2, p.y), ImVec2(p.x + width / 2, p.y + h), IM_COL32(255, 255, 255, 140), 1.0f);
	dl->AddRect(p, ImVec2(p.x + width, p.y + h), IM_COL32(110, 110, 120, 255), 3);
	ImVec2 ts = ImGui::CalcTextSize(label);
	ImVec2 tp(p.x + 6, p.y + (h - ts.y) / 2);
	dl->AddText(ImVec2(tp.x + 1, tp.y + 1), IM_COL32(0, 0, 0, 200), label);
	dl->AddText(tp, IM_COL32(255, 255, 255, 255), label);
	ImGui::Dummy(ImVec2(width, h));
}

// Temperament (openc2e addition, owner 2026-11-11): the creature's inborn
// traits (creatures/Temperament.h), each where it stands among creatures
// (50% is average), and a few words for the strongest.
static void TemperamentPage() {
	if (!KitNeedsCreature())
		return;
	Agent* a = KitCreatureAgent();
	auto* ca = dynamic_cast<CreatureAgent*>(a);
	auto* c = ca ? dynamic_cast<oldCreature*>(ca->getCreature()) : nullptr;
	if (!c)
		return;
	const Temperament& t = c->temperament;
	std::string name = KitCreatureName(a);
	std::string words = Temperaments::describe(t);
	KitNoteColored(ImGui::GetStyle().Colors[ImGuiCol_Text], "%s is %s.", name.c_str(), words.c_str());
	ImGui::Spacing();
	KitNote("Every creature has some of every trait. 50%% (the middle mark) is average: the share of creatures with less.");
	ImGui::Spacing();
	for (int i = 0; i < NUM_TRAITS; i++) {
		int pc = t.percent(i);
		std::string trait = Temperaments::traitName(i);
		trait[0] = (char)std::toupper((unsigned char)trait[0]);
		const char* lean = pc >= 70 ? Temperaments::highWord(i) : pc <= 30 ? Temperaments::lowWord(i) : "";
		std::string label = fmt::format("{} {}%{}", trait, pc, *lean ? fmt::format(" ({})", lean) : "");
		TraitBar(i, pc, label.c_str(), 280);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s ... %s", Temperaments::lowWord(i), Temperaments::highWord(i));
	}
	// how life has shifted it since it hatched (Temperament.h, drift while
	// young; owner, 2026-11-17)
	{
		std::string shifts;
		int n = 0;
		Temperament drifted = c->inheritableTemperament();
		for (int i = 0; i < NUM_TRAITS; i++) {
			float moved = drifted.z[i] - c->temperamentBorn.z[i];
			if (std::fabs(moved) < 0.05f)
				continue;
			shifts += fmt::format("{}{} ({:+.2f})", n++ ? ", " : "", moved > 0 ? Temperaments::highWord(i) : Temperaments::lowWord(i), moved);
		}
		ImGui::Spacing();
		if (n)
			KitNote("Since it hatched, life has made %s more: %s. Young creatures are shaped by being hurt or frightened, comforted and played with, fights, and how trying new things turns out; from youth on their temperament is settled.",
				name.c_str(), shifts.c_str());
		else
			KitNote("Its temperament is still the one it hatched with. Young creatures are shaped by being hurt or frightened, comforted and played with, fights, and how trying new things turns out; from youth on it is settled.");
	}
	// what its own C2 genes give (GenomeTraits.h; owner, 2026-11-20)
	{
		std::string parts;
		int n = 0;
		for (int i = 0; i < NUM_TRAITS; i++) {
			float g = c->geneShift[i];
			if (std::fabs(g) < 0.05f)
				continue;
			parts += fmt::format("{}{} ({:+.2f})", n++ ? ", " : "", g > 0 ? Temperaments::highWord(i) : Temperaments::lowWord(i), g);
		}
		ImGui::Spacing();
		if (n)
			KitNote("Its genes' chemistry (how strongly its body raises fear, boredom, tiredness, loneliness, anger and stress, against its species' usual genome) makes it more %s.", parts.c_str());
		else
			KitNote("Its genes' chemistry is its species' usual, so they leave its temperament as it is; bred, mutated and custom genomes can make a creature bolder, calmer, lonelier and so on.");
	}
	// the seasonal rhythm (Temperament.h, SeasonalRhythm): inherited, unlike
	// the traits above (owner, 2026-11-16)
	ImGui::Spacing();
	ImGui::Spacing();
	{
		int pc = SeasonalRhythm::percent(c->seasonalZ);
		std::string label = fmt::format("Seasonal rhythm {}%", pc);
		const float h = ImGui::GetTextLineHeight() + 6, width = 280;
		ImVec2 p = ImGui::GetCursorScreenPos();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), IM_COL32(45, 45, 50, 255), 3);
		dl->AddRectFilled(p, ImVec2(p.x + width * std::clamp(pc, 0, 100) / 100.0f, p.y + h), IM_COL32(190, 120, 50, 255), 3); // autumn
		dl->AddLine(ImVec2(p.x + width / 2, p.y), ImVec2(p.x + width / 2, p.y + h), IM_COL32(255, 255, 255, 140), 1.0f);
		dl->AddRect(p, ImVec2(p.x + width, p.y + h), IM_COL32(110, 110, 120, 255), 3);
		ImVec2 ts = ImGui::CalcTextSize(label.c_str());
		ImVec2 tp(p.x + 6, p.y + (h - ts.y) / 2);
		dl->AddText(ImVec2(tp.x + 1, tp.y + 1), IM_COL32(0, 0, 0, 200), label.c_str());
		dl->AddText(tp, IM_COL32(255, 255, 255, 255), label.c_str());
		ImGui::Dummy(ImVec2(width, h));
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("lives the same all year ... fattens up in autumn, saves itself in winter");
		KitNote("%s has %s. Passed on from its parents (a mix of theirs, with some chance); a creature from the game's own eggs has its own by chance.",
			name.c_str(), SeasonalRhythm::describe(c->seasonalZ));
	}
	// the mind's own heritable settings (MindGenes.h; owner, 2026-11-20):
	// two columns of four, each where it stands among creatures
	ImGui::Spacing();
	ImGui::Spacing();
	{
		const float h = ImGui::GetTextLineHeight() + 6, width = 180, gap = 10;
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImVec2 origin = ImGui::GetCursorScreenPos();
		for (int i = 0; i < NUM_MIND_GENES; i++) {
			float z = c->mindGenes.z[i];
			int pc = (int)std::lround(50.0 * (1.0 + std::erf(z / std::sqrt(2.0))));
			std::string gname = MindGeneSet::name(i);
			gname[0] = (char)std::toupper((unsigned char)gname[0]);
			std::string label = fmt::format("{} {}%", gname, pc);
			int col = i % 2, row = i / 2;
			ImVec2 p(origin.x + col * (width + gap), origin.y + row * (h + 4));
			ImU32 fill = i < 4 ? IM_COL32(60, 140, 150, 255) : IM_COL32(120, 100, 170, 255); // settings teal, readinesses violet
			dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), IM_COL32(45, 45, 50, 255), 3);
			dl->AddRectFilled(p, ImVec2(p.x + width * std::clamp(pc, 0, 100) / 100.0f, p.y + h), fill, 3);
			dl->AddLine(ImVec2(p.x + width / 2, p.y), ImVec2(p.x + width / 2, p.y + h), IM_COL32(255, 255, 255, 140), 1.0f);
			dl->AddRect(p, ImVec2(p.x + width, p.y + h), IM_COL32(110, 110, 120, 255), 3);
			ImVec2 ts = ImGui::CalcTextSize(label.c_str());
			ImVec2 tp(p.x + 6, p.y + (h - ts.y) / 2);
			dl->AddText(ImVec2(tp.x + 1, tp.y + 1), IM_COL32(0, 0, 0, 200), label.c_str());
			dl->AddText(tp, IM_COL32(255, 255, 255, 255), label.c_str());
			if (ImGui::IsMouseHoveringRect(p, ImVec2(p.x + width, p.y + h)))
				ImGui::SetTooltip("%s ... %s", MindGeneSet::lowWord(i), MindGeneSet::highWord(i));
		}
		ImGui::Dummy(ImVec2(2 * width + gap, 4 * (h + 4)));
		KitNote("Its mind's own settings (teal) and how readily it learns about danger, food, water and company (violet). Passed on like its seasonal rhythm; what it learns is not passed on, but a line quick to learn what keeps it alive tends to last.");
	}
}

void DrawOwnersKit() {
	KitCreatureHeader();
	if (ImGui::BeginTabBar("pages")) {
		if (KitTab("Registration")) {
			RegistrationPage();
			ImGui::EndTabItem();
		}
		if (KitTab("Photos")) {
			PhotosPage();
			ImGui::EndTabItem();
		}
		if (KitTab("Family")) {
			FamilyPage();
			ImGui::EndTabItem();
		}
		if (KitTab("Temperament")) {
			TemperamentPage();
			ImGui::EndTabItem();
		}
		if (KitTab("Relationships")) {
			RelationshipsPage();
			ImGui::EndTabItem();
		}
		if (KitTab("Life Story")) {
			LifeStoryPage();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
	KitNoticeSwitches({{lifeLogEntry::changed, "Changes"}});
}

} // namespace Openc2eImgui
