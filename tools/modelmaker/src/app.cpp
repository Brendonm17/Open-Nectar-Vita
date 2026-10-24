#include "app.h"

#include "lang.h"
#include "launcher_platform.h"
#include "simplify.h"

#include <SDL.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>

namespace mm {

namespace fs = std::filesystem;
using json   = nlohmann::json;

namespace {

bool sSpanish = false;

constexpr float kPi = 3.14159265358979f;

// Animaciones con nombre para mostrar, en el orden en que salen.
struct KnownAnim {
	const char* file;
	const char* es;
	const char* en;
};
const KnownAnim kKnownAnims[] = {
	{ "walk", "Andar", "Walk" },          { "run2", "Correr", "Run" },          { "wait", "Esperar", "Idle" },
	{ "throw", "Lanzar", "Throw" },       { "fue", "Silbar", "Whistle" },       { "nuku", "Arrancar", "Pluck" },
	{ "punch", "Puñetazo", "Punch" },     { "damage", "Daño", "Hurt" },         { "korobu", "Tropezar", "Trip" },
	{ "dead", "Caer KO", "Knocked out" }, { "jump", "Saltar", "Jump" },         { "oboreru", "Ahogarse", "Drown" },
	{ "akubi", "Bostezar", "Yawn" },      { "neru", "Dormir", "Sleep" },        { "chatting", "Charlar", "Chat" },
	{ "gakkari", "Decepción", "Let down" }, { "kaifuku", "Recuperarse", "Recover" }, { "sagasu", "Buscar", "Look around" },
};

const char* animLabel(const Anim& a)
{
	for (const KnownAnim& k : kKnownAnims)
		if (a.name == k.file) return L(k.es, k.en);
	return a.name.c_str();
}

// Color de cada articulación en la vista de pesos.
const float kJointColour[kJoints][3] = {
	{ 0.5f, 0.5f, 0.5f }, { 0.35f, 0.75f, 0.3f }, { 0.95f, 0.85f, 0.25f }, { 0.95f, 0.85f, 0.25f },
	{ 0.75f, 0.4f, 0.9f }, { 0.75f, 0.4f, 0.9f }, { 0.75f, 0.4f, 0.9f },  { 0.3f, 0.55f, 0.95f },
	{ 0.95f, 0.35f, 0.3f }, { 0.5f, 0.5f, 0.5f }, { 0.3f, 0.85f, 0.85f }, { 0.97f, 0.6f, 0.2f },
};

ImU32 levelColour(Check::Level l)
{
	switch (l) {
	case Check::Ok: return IM_COL32(120, 210, 90, 255);
	case Check::Warn: return IM_COL32(240, 200, 70, 255);
	default: return IM_COL32(235, 85, 70, 255);
	}
}

bool bigButton(const char* label, float width = -1, float height = 40)
{
	return ImGui::Button(label, ImVec2(width < 0 ? ImGui::GetContentRegionAvail().x : width, height));
}

void stepHeader(int n, const char* title)
{
	ImGui::Spacing();
	ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(240, 205, 90, 255));
	ImGui::Text("%d  %s", n, title);
	ImGui::PopStyleColor();
	ImGui::Separator();
}

void hint(const char* text)
{
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::TextWrapped("%s", text);
	ImGui::PopStyleColor();
}

} // namespace

void setSpanish(bool spanish) { sSpanish = spanish; }
bool spanish() { return sSpanish; }

App::~App()
{
	if (worker_.joinable()) worker_.join();
	for (GLuint t : modelTex_)
		if (t) glDeleteTextures(1, &t);
	for (GLuint t : olimarTex_)
		if (t) glDeleteTextures(1, &t);
}

bool App::init(const std::string& openPath, std::string& error)
{
	cfg_.load();
	setSpanish(cfg_.language == "es");
	if (!view_.init(error)) return false;
	std::string why;
	if (!setupGame(why)) {
		askGameDir_ = true;
		say(why, true);
	}
	if (!openPath.empty()) {
		loadModel(openPath, false);
	} else if (gameReady_) {
		std::string source;
		if (loadProject(source)) loadModel(source, true);
	}
	return true;
}

bool App::setupGame(std::string& error)
{
	gameReady_ = false;
	std::string why;
	// El launcher nos abre en la carpeta del juego; si no, la del editor de mapas.
	std::string assets = nm::resolveAssetsDir((fs::current_path() / "assets").string(), why);
	if (assets.empty() && !cfg_.assetsDir.empty()) assets = nm::resolveAssetsDir(cfg_.assetsDir, why);
	if (assets.empty()) assets = nm::guessAssetsDir();
	if (assets.empty()) {
		error = L("No encuentro el juego. Elige la carpeta donde está instalado Pikmin.", "Can't find the game. Choose the folder where Pikmin is installed.");
		return false;
	}
	OlimarRig rig;
	if (!rig.load(assets + "/dataDir", error)) return false;
	std::vector<Anim> anims;
	if (!loadAnimBundle(assets + "/dataDir/pikis/bluModel.anm", anims, error)) return false;
	olimar_    = std::move(rig);
	anims_     = std::move(anims);
	assetsDir_ = assets;
	if (cfg_.assetsDir != assets) {
		cfg_.assetsDir = assets;
		cfg_.save();
	}
	animOrder_.clear();
	for (const KnownAnim& k : kKnownAnims)
		for (std::size_t i = 0; i < anims_.size(); ++i)
			if (anims_[i].name == k.file) animOrder_.push_back(int(i));
	for (std::size_t i = 0; i < anims_.size(); ++i)
		if (std::find(animOrder_.begin(), animOrder_.end(), int(i)) == animOrder_.end()) animOrder_.push_back(int(i));

	for (GLuint t : olimarTex_)
		if (t) glDeleteTextures(1, &t);
	olimarTex_.clear();
	for (const nectar::Image& img : olimar_.materialTex) olimarTex_.push_back(uploadTexture(img, true));
	gameReady_ = true;
	resetCamera();
	return true;
}

std::string App::customDir() const { return (fs::path(cfg_.gameDir()) / "Load" / "Models" / "Custom").string(); }
std::string App::packPath() const { return (fs::path(customDir()) / "olimar.nhm").string(); }
std::string App::projectPath() const { return (fs::path(customDir()) / "olimar_project.json").string(); }

void App::say(const std::string& text, bool error)
{
	status_      = text;
	statusError_ = error;
}

// Por encima de esto se simplifica solo al abrir.
constexpr std::size_t kAutoSimplify = 40000;

void App::loadModel(const std::string& path, bool restoreProject)
{
	if (!gameReady_) {
		say(L("Primero elige la carpeta del juego.", "Choose the game folder first."), true);
		return;
	}
	if (working_) return;
	if (worker_.joinable()) worker_.join();
	pendingPath_       = path;
	pendingRestore_    = restoreProject;
	pendingResimplify_ = false;
	working_           = true;
	workDone_          = false;
	const std::size_t target = std::size_t(targetTris_);
	worker_ = std::thread([this, path, target] {
		ImpModel m;
		std::string error;
		if (importModel(path, m, error)) {
			pendingSource_ = m;
			if (m.triangles() > kAutoSimplify) simplifyModel(m, target);
		}
		pending_      = std::move(m);
		pendingError_ = error;
		workDone_     = true;
	});
}

void App::resimplify()
{
	if (working_ || !hasModel_) return;
	if (worker_.joinable()) worker_.join();
	pendingResimplify_ = true;
	working_           = true;
	workDone_          = false;
	pendingSource_     = source_;
	const std::size_t target = std::size_t(targetTris_);
	worker_ = std::thread([this, target] {
		ImpModel m = pendingSource_;
		simplifyModel(m, target);
		pending_      = std::move(m);
		pendingError_.clear();
		workDone_ = true;
	});
}

void App::finishLoad()
{
	if (worker_.joinable()) worker_.join();
	working_ = false;
	if (!pendingError_.empty()) {
		say(std::string(L("No se pudo abrir: ", "Could not open: ")) + pendingError_, true);
		return;
	}
	if (pendingResimplify_) {
		// Misma colocación y puntos: solo cambia la malla.
		model_ = std::move(pending_);
		uploadTextures();
		rebuild();
		say(std::to_string(model_.triangles()) + L(" triángulos.", " triangles."));
		return;
	}
	const std::string path     = pendingPath_;
	const bool restoreProject  = pendingRestore_;
	model_    = std::move(pending_);
	source_   = std::move(pendingSource_);
	hasModel_ = true;
	fit_                 = FitSettings();
	fit_.useSkeleton     = model_.skinned;
	autoFit(model_, olimar_, fit_);
	exportOpt_.noTint.assign(model_.materials.size(), 0);
	exportOpt_.textureMax = 1024;
	if (restoreProject) {
		// Los ajustes guardados con el proyecto.
		std::ifstream f(fs::u8path(projectPath()));
		try {
			const json j = json::parse(f);
			fit_.scale          = j.value("scale", 1.0f);
			fit_.quarter        = j.value("quarter", 0);
			fit_.lyingDown      = j.value("lyingDown", false);
			fit_.offsetY        = j.value("offsetY", 0.0f);
			fit_.offsetZ        = j.value("offsetZ", 0.0f);
			fit_.useSkeleton    = j.value("useSkeleton", model_.skinned) && model_.skinned;
			fit_.straightenArms = j.value("straightenArms", true);
			fit_.straightenLegs = j.value("straightenLegs", false);
			fit_.symmetry       = j.value("symmetry", true);
			fit_.markersCustom  = j.value("markersCustom", false);
			exportOpt_.textureMax = j.value("textureMax", 1024);
			if (fit_.markersCustom && j.contains("markers") && j["markers"].size() == M_COUNT) {
				for (int i = 0; i < M_COUNT; ++i)
					fit_.markers[std::size_t(i)] = { j["markers"][std::size_t(i)][0].get<float>(), j["markers"][std::size_t(i)][1].get<float>(),
						                             j["markers"][std::size_t(i)][2].get<float>() };
			} else {
				fit_.markers = estimateMarkers(model_, olimar_, fit_);
			}
			const std::vector<int> nt = j.value("noTint", std::vector<int>());
			for (std::size_t i = 0; i < nt.size() && i < exportOpt_.noTint.size(); ++i) exportOpt_.noTint[i] = char(nt[i] != 0);
		} catch (const std::exception&) {
		}
		say(L("Seguimos con tu modelo de la última vez.", "Picking up your model from last time."));
	} else {
		say(std::string(L("Listo: ", "Loaded: ")) + fs::u8path(path).filename().u8string());
	}
	// Por defecto, andando: se ve enseguida cómo queda.
	anim_ = -1;
	for (std::size_t i = 0; i < anims_.size(); ++i)
		if (anims_[i].name == "walk") anim_ = int(i);
	frame_ = 0;
	uploadTextures();
	rebuild();
	resetCamera();
}

void App::uploadTextures()
{
	for (GLuint t : modelTex_)
		if (t) glDeleteTextures(1, &t);
	modelTex_.clear();
	for (const ImpMaterial& m : model_.materials) {
		bool repeat = true;
		modelTex_.push_back(m.texture.empty() ? 0 : uploadTexture(m.texture, repeat));
	}
	order_.clear();
	for (std::size_t t = 0; t < model_.triMaterial.size(); ++t) order_.push_back(t);
	std::stable_sort(order_.begin(), order_.end(), [&](std::size_t a, std::size_t b) { return model_.triMaterial[a] < model_.triMaterial[b]; });
}

void App::rebuild()
{
	if (!hasModel_) return;
	rig_          = buildRig(model_, olimar_, fit_);
	rigDirty_     = false;
	exportDirty_  = true;
	lastRebuild_  = ImGui::GetTime();
}

void App::resetCamera()
{
	const float h = hasModel_ ? std::max(rig_.max.y - rig_.min.y, olimar_.bodyTop) : olimar_.max.y - olimar_.min.y;
	cam_.target   = { olimarMode_ == 1 && hasModel_ ? -(olimar_.max.x - olimar_.min.x) * 0.6f : 0.0f, h * 0.5f, 0 };
	cam_.distance = h * 2.4f + 10.0f;
	cam_.yaw      = 0.35f;
	cam_.pitch    = 0.15f;
}

// Vértices de las dos mallas para este fotograma (con la piel en la CPU).
void App::fillMeshes()
{
	std::vector<Mat34> world, skin, olimarSkin(kJoints);
	const bool animated = anim_ >= 0 && std::size_t(anim_) < anims_.size();
	if (animated) anims_[std::size_t(anim_)].pose(frame_, olimar_.parent, world);
	else world = olimar_.bind;
	for (int j = 0; j < kJoints; ++j) olimarSkin[std::size_t(j)] = world[std::size_t(j)] * inverse(olimar_.bind[std::size_t(j)]);

	// Olimar: al lado (a la izquierda de la pantalla) o encima.
	const float gap    = 3.0f;
	const float offset = olimarMode_ == 1 && hasModel_ ? rig_.min.x - olimar_.max.x - gap : 0.0f;
	olimarMesh_.data.clear();
	olimarMesh_.ranges.clear();
	if (olimarMode_ != 0) {
		int lastMat = -2;
		for (std::size_t t = 0; t < olimar_.triMaterial.size(); ++t) {
			const int mat = olimar_.triMaterial[t];
			if (mat != lastMat) {
				DynamicMesh::Range r;
				r.first   = int(olimarMesh_.data.size());
				r.texture = std::size_t(mat) < olimarTex_.size() ? olimarTex_[std::size_t(mat)] : 0;
				olimarMesh_.ranges.push_back(r);
				lastMat = mat;
			}
			const nectar::Rgba8 mc = std::size_t(mat) < olimar_.materialColour.size() ? olimar_.materialColour[std::size_t(mat)] : nectar::Rgba8 {};
			for (int k = 0; k < 3; ++k) {
				const SkinVertex& v = olimar_.verts[t * 3 + std::size_t(k)];
				Vec3 p, n;
				for (int i = 0; i < 4; ++i) {
					if (v.weight[i] <= 0) continue;
					p += olimarSkin[std::size_t(v.joint[i])].point(v.p) * v.weight[i];
					n += olimarSkin[std::size_t(v.joint[i])].dir(v.n) * v.weight[i];
				}
				n = normalize(n);
				GpuVertex g {};
				g.p[0] = p.x + offset, g.p[1] = p.y, g.p[2] = p.z;
				g.n[0] = n.x, g.n[1] = n.y, g.n[2] = n.z;
				g.uv[0] = v.uv.x, g.uv[1] = v.uv.y;
				g.c[0] = v.col[0] * mc.r / 255.0f, g.c[1] = v.col[1] * mc.g / 255.0f, g.c[2] = v.col[2] * mc.b / 255.0f, g.c[3] = 1;
				olimarMesh_.data.push_back(g);
			}
			olimarMesh_.ranges.back().count = int(olimarMesh_.data.size()) - olimarMesh_.ranges.back().first;
		}
		olimarMesh_.upload();
	}

	// El modelo: en reposo sin enderezar (para colocar los puntos) o animado
	// como en el juego.
	modelMesh_.data.clear();
	modelMesh_.ranges.clear();
	if (!hasModel_ || rig_.verts.empty()) return;
	if (animated) retargetSkin(olimar_, rig_.pivots, world, skin);
	int lastMat = -2;
	for (std::size_t t : order_) {
		if (t * 3 + 2 >= rig_.verts.size()) continue;
		const int mat           = model_.triMaterial[t];
		const ImpMaterial& im   = model_.materials[std::size_t(mat)];
		if (mat != lastMat) {
			DynamicMesh::Range r;
			r.first   = int(modelMesh_.data.size());
			r.texture = showWeights_ ? 0 : modelTex_[std::size_t(mat)];
			r.blend   = im.alpha == ImpMaterial::Blend && !showWeights_;
			modelMesh_.ranges.push_back(r);
			lastMat = mat;
		}
		for (int k = 0; k < 3; ++k) {
			const SkinVertex& v = rig_.verts[t * 3 + std::size_t(k)];
			Vec3 p = v.p, n = v.n;
			if (animated) {
				p = n = Vec3 {};
				for (int i = 0; i < 4; ++i) {
					if (v.weight[i] <= 0) continue;
					p += skin[std::size_t(v.joint[i])].point(v.p) * v.weight[i];
					n += skin[std::size_t(v.joint[i])].dir(v.n) * v.weight[i];
				}
				n = normalize(n);
			} else if (t * 3 + std::size_t(k) < rig_.unbent.size()) {
				p = rig_.unbent[t * 3 + std::size_t(k)];
			}
			GpuVertex g {};
			g.p[0] = p.x, g.p[1] = p.y, g.p[2] = p.z;
			g.n[0] = n.x, g.n[1] = n.y, g.n[2] = n.z;
			g.uv[0] = v.uv.x, g.uv[1] = v.uv.y;
			if (showWeights_) {
				for (int c = 0; c < 3; ++c) {
					g.c[c] = 0;
					for (int i = 0; i < 4; ++i) g.c[c] += kJointColour[v.joint[i]][c] * v.weight[i];
				}
				g.c[3] = 1;
			} else {
				const bool useVertexColour = im.texture.empty() && im.vertexColors;
				for (int c = 0; c < 4; ++c) g.c[c] = im.color[c] * (useVertexColour ? v.col[c] : 1.0f);
				if (im.alpha == ImpMaterial::Opaque) g.c[3] = 1;
			}
			modelMesh_.data.push_back(g);
		}
		modelMesh_.ranges.back().count = int(modelMesh_.data.size()) - modelMesh_.ranges.back().first;
	}
	modelMesh_.upload();
}

void App::frame()
{
	ImGuiIO& io = ImGui::GetIO();
	if (working_ && workDone_) finishLoad();
	if (playing_ && anim_ >= 0 && std::size_t(anim_) < anims_.size()) {
		frame_ += io.DeltaTime * 30.0f * speed_;
		const float frames = float(anims_[std::size_t(anim_)].frames);
		if (frames > 0) frame_ = std::fmod(frame_, frames);
	}
	// Reconstruir cuando se suelta el ratón (o cada poco mientras se arrastra).
	if (rigDirty_ && (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::GetTime() - lastRebuild_ > 0.2)) rebuild();
	if (exportDirty_ && hasModel_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
		export_      = buildParts(model_, rig_, olimar_, fit_, exportOpt_);
		exportDirty_ = false;
	}

	const ImGuiViewport* vp = ImGui::GetMainViewport();
	const float panelW      = std::clamp(vp->WorkSize.x * 0.27f, 330.0f, 470.0f);
	ImGui::SetNextWindowPos(vp->WorkPos);
	ImGui::SetNextWindowSize(ImVec2(panelW, vp->WorkSize.y));
	drawPanel(panelW, vp->WorkSize.y);
	drawView(vp->WorkPos.x + panelW, vp->WorkSize.x - panelW, vp->WorkSize.y);

	if (working_) {
		ImGui::OpenPopup("##working");
	}
	if (ImGui::BeginPopupModal("##working", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
		static const char* dots[] = { "", ".", "..", "..." };
		ImGui::Text("%s%s", L("Abriendo y simplificando el modelo", "Opening and simplifying the model"), dots[int(ImGui::GetTime() * 3) % 4]);
		hint(L("Los modelos muy detallados tardan un poco.", "Very detailed models take a moment."));
		if (!working_) ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	if (askGameDir_) {
		ImGui::OpenPopup("##gamedir");
		askGameDir_ = false;
	}
	if (ImGui::BeginPopupModal("##gamedir", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
		ImGui::TextUnformatted(L("¿Dónde está el juego?", "Where is the game?"));
		ImGui::Spacing();
		ImGui::PushTextWrapPos(420);
		hint(L("Abre el creador desde el launcher o elige la carpeta donde instalaste Pikmin (la que tiene \"assets\").",
		       "Open the model maker from the launcher, or choose the folder where Pikmin is installed (the one with \"assets\")."));
		ImGui::PopTextWrapPos();
		if (!status_.empty() && statusError_) ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1), "%s", status_.c_str());
		if (bigButton(L("Elegir carpeta…", "Choose folder…"), 420)) {
			const fs::path p = pikmin::launcher::platform::askForInstallDirectory(L("Carpeta de Pikmin", "Pikmin folder"));
			if (!p.empty()) {
				std::string why;
				const std::string assets = nm::resolveAssetsDir(p.string(), why);
				if (assets.empty()) {
					say(why, true);
				} else {
					cfg_.assetsDir = assets;
					cfg_.save();
					std::string err;
					if (setupGame(err)) {
						say(L("Juego encontrado.", "Game found."));
						ImGui::CloseCurrentPopup();
					} else {
						say(err, true);
					}
				}
			}
		}
		if (ImGui::Button(L("Salir", "Quit"), ImVec2(420, 0))) quit_ = true;
		ImGui::EndPopup();
	}
}

void App::drawPanel(float width, float height)
{
	(void)width;
	(void)height;
	ImGui::Begin("##panel", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
	ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(250, 220, 120, 255));
	ImGui::SetWindowFontScale(1.35f);
	ImGui::TextUnformatted(L("Creador de modelos", "Model Maker"));
	ImGui::SetWindowFontScale(1.0f);
	ImGui::PopStyleColor();
	ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60);
	if (ImGui::SmallButton(spanish() ? "EN" : "ES")) {
		setSpanish(!spanish());
		cfg_.language = spanish() ? "es" : "en";
		cfg_.save();
		exportDirty_ = true;
	}
	hint(L("Tu propio Olimar: abre un modelo 3D y el creador lo pone sobre su esqueleto.", "Your own Olimar: open a 3D model and it is fitted onto his skeleton."));

	// 1 · Modelo.
	stepHeader(1, L("Tu modelo", "Your model"));
	if (bigButton(L("Abrir modelo 3D…", "Open 3D model…"), -1, 46)) {
		const fs::path p = pikmin::launcher::platform::askForFile(L("Abrir modelo", "Open model"), L("Modelo 3D", "3D model"), "*.glb *.gltf *.vrm *.fbx *.obj");
		if (!p.empty()) loadModel(p.u8string(), false);
	}
	hint(L("O arrástralo a la ventana. Valen .glb, .gltf, .vrm (VRoid), .fbx (Mixamo) y .obj.",
	       "Or drop it on the window. Works with .glb, .gltf, .vrm (VRoid), .fbx (Mixamo) and .obj."));
	if (hasModel_) {
		ImGui::Text("%s  ·  %s", fs::u8path(model_.path).filename().u8string().c_str(), model_.format.c_str());
		ImGui::Text(L("%zu triángulos, %zu materiales, %zu huesos", "%zu triangles, %zu materials, %zu bones"), model_.triangles(), model_.materials.size(),
		            model_.bones.size());
		if (source_.triangles() > model_.triangles())
			hint((std::string(L("Simplificado solo (tenía ", "Simplified automatically (it had ")) + std::to_string(source_.triangles()) + L(").", ").")).c_str());
		if (source_.triangles() > 5000) {
			ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 110);
			ImGui::SliderInt("##target", &targetTris_, 5000, int(std::min<std::size_t>(source_.triangles(), 150000)), L("%d triángulos", "%d triangles"));
			ImGui::SameLine();
			if (ImGui::Button(L("Aplicar", "Apply"), ImVec2(-1, 0))) resimplify();
		}
	}
	if (!hasModel_) {
		ImGui::End();
		return;
	}

	// 2 · Colocar.
	stepHeader(2, L("Colocar", "Place"));
	if (bigButton(L("Ajustar solo", "Auto-fit"))) {
		autoFit(model_, olimar_, fit_);
		rigDirty_ = true;
		resetCamera();
	}
	float pct = fit_.scale * 100.0f;
	ImGui::SetNextItemWidth(-1);
	if (ImGui::SliderFloat("##scale", &pct, 30.0f, 300.0f, L("Tamaño %.0f%% de Olimar", "Size %.0f%% of Olimar"))) {
		const float old = fit_.scale;
		fit_.scale      = pct / 100.0f;
		// Los puntos crecen con el modelo (desde el suelo).
		const float k = fit_.scale / std::max(old, 1e-3f);
		for (Vec3& m : fit_.markers) m = { m.x * k, olimar_.min.y + (m.y - olimar_.min.y) * k, m.z * k };
		rigDirty_ = true;
	}
	const float half = ImGui::GetContentRegionAvail().x * 0.5f - 4;
	auto turn        = [&](int q) {
        fit_.quarter = (fit_.quarter + q + 4) % 4;
        fit_.markers = estimateMarkers(model_, olimar_, fit_);
        fit_.markersCustom = false;
        rigDirty_          = true;
	};
	if (ImGui::Button(L("Girar a la izq.", "Turn left"), ImVec2(half, 0))) turn(1);
	ImGui::SameLine();
	if (ImGui::Button(L("Girar a la der.", "Turn right"), ImVec2(half, 0))) turn(3);
	if (ImGui::Checkbox(L("Viene tumbado (Z arriba)", "Lying down (Z up)"), &fit_.lyingDown)) {
		fit_.markers       = estimateMarkers(model_, olimar_, fit_);
		fit_.markersCustom = false;
		rigDirty_          = true;
	}
	const float H = std::max(olimar_.bodyTop - olimar_.min.y, 1.0f);
	ImGui::SetNextItemWidth(-1);
	float oy = fit_.offsetY;
	if (ImGui::SliderFloat("##oy", &oy, -H * 0.5f, H * 0.5f, L("Subir / bajar %.1f", "Up / down %.1f"))) {
		for (Vec3& m : fit_.markers) m.y += oy - fit_.offsetY;
		fit_.offsetY = oy;
		rigDirty_    = true;
	}
	ImGui::SetNextItemWidth(-1);
	float oz = fit_.offsetZ;
	if (ImGui::SliderFloat("##oz", &oz, -H * 0.5f, H * 0.5f, L("Adelante / atrás %.1f", "Forward / back %.1f"))) {
		for (Vec3& m : fit_.markers) m.z += oz - fit_.offsetZ;
		fit_.offsetZ = oz;
		rigDirty_    = true;
	}

	// 3 · Esqueleto.
	stepHeader(3, L("Esqueleto", "Skeleton"));
	const bool editing = anim_ < 0;
	if (bigButton(editing ? L("Ver cómo se mueve", "See it move") : L("Mover los puntos del esqueleto", "Move the skeleton dots"))) {
		if (editing) {
			for (std::size_t i = 0; i < anims_.size(); ++i)
				if (anims_[i].name == "walk") anim_ = int(i);
			if (anim_ < 0 && !anims_.empty()) anim_ = 0;
		} else {
			anim_ = -1;
		}
	}
	if (editing)
		hint(L("Arrastra cada punto a su sitio: hombros, manos, cadera, piernas, pies y cuello. Los brazos giran desde los hombros que marques.",
		       "Drag each dot into place: shoulders, hands, pelvis, hips, feet and neck. The arms swing from the shoulders you mark."));
	ImGui::Checkbox(L("Simetría (izquierda = derecha)", "Symmetry (left = right)"), &fit_.symmetry);
	if (ImGui::Button(L("Recolocar los puntos solos", "Place the dots automatically"), ImVec2(-1, 0))) {
		fit_.markers       = estimateMarkers(model_, olimar_, fit_);
		fit_.markersCustom = false;
		rigDirty_          = true;
	}
	ImGui::BeginDisabled(!model_.skinned);
	int weights = fit_.useSkeleton ? 0 : 1;
	if (ImGui::RadioButton(L("Pesos del modelo", "Model's weights"), &weights, 0)) {
		fit_.useSkeleton = true;
		if (!fit_.markersCustom) fit_.markers = estimateMarkers(model_, olimar_, fit_);
		rigDirty_ = true;
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::RadioButton(L("Automáticos", "Automatic"), &weights, 1)) {
		fit_.useSkeleton = false;
		rigDirty_        = true;
	}
	if (!model_.skinned) hint(L("El modelo no trae esqueleto: los pesos se calculan con los puntos.", "The model has no skeleton: weights come from the dots."));
	if (ImGui::Checkbox(L("Brazos como los de Olimar", "Arms like Olimar's"), &fit_.straightenArms)) rigDirty_ = true;
	ImGui::SameLine();
	if (ImGui::Checkbox(L("Piernas", "Legs"), &fit_.straightenLegs)) rigDirty_ = true;
	ImGui::Checkbox(L("Ver pesos (colores)", "Show weights (colours)"), &showWeights_);

	// 4 · Animaciones.
	stepHeader(4, L("Animaciones", "Animations"));
	ImGui::SetNextItemWidth(-1);
	const char* current = anim_ >= 0 ? animLabel(anims_[std::size_t(anim_)]) : L("Pose de reposo (editar puntos)", "Rest pose (edit dots)");
	if (ImGui::BeginCombo("##anim", current, ImGuiComboFlags_HeightLarge)) {
		if (ImGui::Selectable(L("Pose de reposo (editar puntos)", "Rest pose (edit dots)"), anim_ < 0)) anim_ = -1;
		for (int i : animOrder_) {
			ImGui::PushID(i);
			if (ImGui::Selectable(animLabel(anims_[std::size_t(i)]), anim_ == i)) {
				anim_  = i;
				frame_ = 0;
			}
			ImGui::PopID();
		}
		ImGui::EndCombo();
	}
	if (ImGui::Button(playing_ ? L("Pausa", "Pause") : L("Seguir", "Play"), ImVec2(90, 0))) playing_ = !playing_;
	ImGui::SameLine();
	ImGui::SetNextItemWidth(-1);
	ImGui::SliderFloat("##speed", &speed_, 0.1f, 2.0f, L("Velocidad %.1fx", "Speed %.1fx"));
	const char* modes[] = { L("Olimar: oculto", "Olimar: hidden"), L("Olimar: al lado", "Olimar: beside"), L("Olimar: encima", "Olimar: overlaid") };
	ImGui::SetNextItemWidth(-1);
	if (ImGui::Combo("##olimar", &olimarMode_, modes, 3)) resetCamera();
	ImGui::Checkbox(L("Ver esqueleto", "Show skeleton"), &showSkeleton_);

	// 5 · Guardar.
	stepHeader(5, L("Guardar en el juego", "Save to the game"));
	if (exportDirty_) hint(L("Comprobando…", "Checking…"));
	for (const Check& c : export_.checks) {
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImVec2 p = ImGui::GetCursorScreenPos();
		dl->AddCircleFilled(ImVec2(p.x + 7, p.y + ImGui::GetTextLineHeight() * 0.5f + 1), 6, levelColour(c.level));
		ImGui::Dummy(ImVec2(18, 0));
		ImGui::SameLine();
		ImGui::TextWrapped("%s", c.text.c_str());
	}
	if (ImGui::TreeNode(L("Más opciones", "More options"))) {
		const int sizes[] = { 512, 1024, 2048 };
		int sel           = exportOpt_.textureMax <= 512 ? 0 : exportOpt_.textureMax <= 1024 ? 1 : 2;
		const char* names[] = { L("Texturas: 512 (Android)", "Textures: 512 (Android)"), L("Texturas: 1024", "Textures: 1024"), L("Texturas: 2048 (PC)", "Textures: 2048 (PC)") };
		ImGui::SetNextItemWidth(-1);
		if (ImGui::Combo("##tex", &sel, names, 3)) {
			exportOpt_.textureMax = sizes[sel];
			exportDirty_          = true;
		}
		ImGui::TextUnformatted(L("Sin tinte en cooperativo (cara, visor):", "No co-op tint (face, visor):"));
		for (std::size_t i = 0; i < model_.materials.size(); ++i) {
			bool on = exportOpt_.noTint[i] != 0;
			ImGui::PushID(int(i));
			if (ImGui::Checkbox(model_.materials[i].name.c_str(), &on)) {
				exportOpt_.noTint[i] = char(on);
				exportDirty_         = true;
			}
			ImGui::PopID();
		}
		ImGui::TreePop();
	}
	ImGui::BeginDisabled(exportDirty_ || !export_.ok());
	if (bigButton(L("Guardar mi Olimar", "Save my Olimar"), -1, 46)) save();
	if (bigButton(L("Guardar y jugar", "Save and play"))) {
		if (save()) playInGame();
	}
	ImGui::EndDisabled();
	std::error_code ec;
	if (fs::exists(fs::u8path(packPath()), ec)) {
		if (ImGui::Button(L("Quitar mi modelo (volver al Olimar normal)", "Remove my model (back to normal Olimar)"), ImVec2(-1, 0))) removeCustom();
	}
	if (!status_.empty()) {
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, statusError_ ? IM_COL32(240, 110, 90, 255) : IM_COL32(150, 220, 120, 255));
		ImGui::TextWrapped("%s", status_.c_str());
		ImGui::PopStyleColor();
	}
	ImGui::End();
}

bool App::project(const Vec3& p, ImVec2& out) const
{
	const nm::M4& m = viewProj_;
	const float x   = m.at(0, 0) * p.x + m.at(0, 1) * p.y + m.at(0, 2) * p.z + m.at(0, 3);
	const float y   = m.at(1, 0) * p.x + m.at(1, 1) * p.y + m.at(1, 2) * p.z + m.at(1, 3);
	const float w   = m.at(3, 0) * p.x + m.at(3, 1) * p.y + m.at(3, 2) * p.z + m.at(3, 3);
	if (w <= 1e-5f) return false;
	out = ImVec2(viewOrigin_.x + (x / w * 0.5f + 0.5f) * viewSize_.x, viewOrigin_.y + (0.5f - y / w * 0.5f) * viewSize_.y);
	return true;
}

void App::dragMarker(const ImVec2& mouse)
{
	// Se mueve en el plano de la pantalla que pasa por el punto.
	const nm::M4 inv = viewProj_.inverse();
	const float nx   = (mouse.x - viewOrigin_.x) / viewSize_.x * 2 - 1;
	const float ny   = 1 - (mouse.y - viewOrigin_.y) / viewSize_.y * 2;
	const nm::V3 a = inv.point({ nx, ny, -1 }), b = inv.point({ nx, ny, 1 });
	const Vec3 o { a.x, a.y, a.z }, d = normalize(Vec3 { b.x - a.x, b.y - a.y, b.z - a.z });
	const Vec3 f     = normalize(cam_.target - cam_.eye());
	Vec3& m          = fit_.markers[std::size_t(dragMarker_)];
	const float den  = dot(d, f);
	if (std::fabs(den) < 1e-6f) return;
	const float t = dot(m - o, f) / den;
	m             = o + d * t;
	if (fit_.symmetry) {
		const int mirror = markerMirror(dragMarker_);
		if (mirror >= 0) fit_.markers[std::size_t(mirror)] = { -m.x, m.y, m.z };
		else m.x = 0;
	}
	fit_.markersCustom = true;
	rigDirty_          = true;
}

void App::drawView(float x, float width, float height)
{
	const ImGuiViewport* vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(ImVec2(x, vp->WorkPos.y));
	ImGui::SetNextWindowSize(ImVec2(width, height));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::Begin("##view", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
	ImGui::PopStyleVar();
	viewOrigin_ = ImGui::GetCursorScreenPos();
	viewSize_   = ImGui::GetContentRegionAvail();
	viewSize_.x = std::max(viewSize_.x, 1.0f);
	viewSize_.y = std::max(viewSize_.y, 1.0f);
	view_.resize(int(viewSize_.x), int(viewSize_.y));

	if (gameReady_) {
		fillMeshes();
		viewProj_ = cam_.projection(viewSize_.x / viewSize_.y) * cam_.view();
		view_.begin(viewProj_);
		// Suelo: rejilla.
		std::vector<Vec3> grid;
		const float step = 2.5f, ext = 25.0f;
		for (float g = -ext; g <= ext + 0.01f; g += step) {
			grid.push_back({ g, olimar_.min.y, -ext });
			grid.push_back({ g, olimar_.min.y, ext });
			grid.push_back({ -ext, olimar_.min.y, g });
			grid.push_back({ ext, olimar_.min.y, g });
		}
		const float gridColour[4] = { 0.30f, 0.38f, 0.24f, 1 };
		view_.drawLines(grid, gridColour);
		if (olimarMode_ == 1 || (olimarMode_ == 2 && !hasModel_)) view_.drawMesh(olimarMesh_, true);
		view_.drawMesh(modelMesh_, !showWeights_);
		if (olimarMode_ == 2 && hasModel_) view_.drawMesh(olimarMesh_, true, 0.35f);
		view_.end();
	}
	ImGui::Image(ImTextureID(std::intptr_t(view_.colorTexture())), viewSize_, ImVec2(0, 1), ImVec2(1, 0));
	const bool hovered = ImGui::IsItemHovered();

	// Esqueleto encima.
	std::vector<Mat34> skin;
	if (hasModel_ && anim_ >= 0 && std::size_t(anim_) < anims_.size()) {
		std::vector<Mat34> world;
		anims_[std::size_t(anim_)].pose(frame_, olimar_.parent, world);
		retargetSkin(olimar_, rig_.pivots, world, skin);
	}
	if (hasModel_) drawOverlay(skin);

	// Ratón: arrastrar un punto, girar (izquierdo/derecho), desplazar (medio
	// o Mayús), rueda para acercar.
	ImGuiIO& io = ImGui::GetIO();
	if (hovered && io.MouseWheel != 0) cam_.distance = std::clamp(cam_.distance * std::pow(0.88f, io.MouseWheel), 5.0f, 400.0f);
	if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
		if (hoverMarker_ >= 0) dragMarker_ = hoverMarker_;
		else if (io.KeyShift) panning_ = true;
		else orbiting_ = true;
	}
	if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) orbiting_ = true;
	if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) panning_ = true;
	if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseDown(ImGuiMouseButton_Right) && !ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
		dragMarker_ = -1;
		orbiting_ = panning_ = false;
	}
	if (dragMarker_ >= 0) {
		dragMarker(io.MousePos);
	} else if (orbiting_) {
		cam_.yaw -= io.MouseDelta.x * 0.008f;
		cam_.pitch = std::clamp(cam_.pitch + io.MouseDelta.y * 0.008f, -1.4f, 1.4f);
	} else if (panning_) {
		const Vec3 f = normalize(cam_.target - cam_.eye());
		const Vec3 r = normalize(cross(f, { 0, 1, 0 }));
		const Vec3 u = cross(r, f);
		const float k = cam_.distance * 0.0016f;
		cam_.target   = cam_.target - r * (io.MouseDelta.x * k) + u * (io.MouseDelta.y * k);
	}

	// Botones de cámara.
	ImGui::SetCursorScreenPos(ImVec2(viewOrigin_.x + 12, viewOrigin_.y + 12));
	if (ImGui::Button(L("De frente", "Front"))) {
		cam_.yaw   = 0;
		cam_.pitch = 0.05f;
	}
	ImGui::SameLine();
	if (ImGui::Button(L("De lado", "Side"))) {
		cam_.yaw   = kPi / 2;
		cam_.pitch = 0.05f;
	}
	ImGui::SameLine();
	if (ImGui::Button(L("Centrar", "Recentre"))) resetCamera();
	if (!hasModel_) {
		const char* msg = L("Abre o arrastra aquí un modelo 3D", "Open or drop a 3D model here");
		const ImVec2 ts = ImGui::CalcTextSize(msg);
		ImGui::GetWindowDrawList()->AddText(ImVec2(viewOrigin_.x + (viewSize_.x - ts.x) * 0.5f, viewOrigin_.y + viewSize_.y * 0.85f),
		                                    IM_COL32(240, 230, 190, 230), msg);
	} else {
		ImGui::SetCursorScreenPos(ImVec2(viewOrigin_.x + 12, viewOrigin_.y + viewSize_.y - 30));
		hint(L("Izq./der.: girar · Mayús o rueda pulsada: mover · Rueda: acercar", "Left/right drag: orbit · Shift or middle: pan · Wheel: zoom"));
	}
	ImGui::End();
}

void App::drawOverlay(const std::vector<Mat34>& skin)
{
	ImDrawList* dl   = ImGui::GetWindowDrawList();
	hoverMarker_     = -1;
	const bool edit  = skin.empty();
	auto pos         = [&](int marker) -> Vec3 {
        const Vec3& m = fit_.markers[std::size_t(marker)];
        if (edit) return m;
        // Animado: cada punto con la articulación que lo mueve.
        int joint = J_SPINE;
        switch (marker) {
        case M_NECK:
        case M_HEADTOP: joint = J_HEAD; break;
        case M_RSHOULDER:
        case M_RHAND: joint = J_RARM; break;
        case M_LSHOULDER:
        case M_LHAND: joint = J_LARM; break;
        case M_RHIP:
        case M_RFOOT: joint = J_RLEG; break;
        case M_LHIP:
        case M_LFOOT: joint = J_LLEG; break;
        default: break;
        }
        return skin[std::size_t(joint)].point(rig_.markers[std::size_t(marker)]);
	};
	if (!showSkeleton_ && !edit) return;
	const int bones[][2] = { { M_PELVIS, M_NECK },       { M_NECK, M_HEADTOP },   { M_NECK, M_RSHOULDER }, { M_NECK, M_LSHOULDER },
		                     { M_RSHOULDER, M_RHAND },   { M_LSHOULDER, M_LHAND }, { M_PELVIS, M_RHIP },    { M_PELVIS, M_LHIP },
		                     { M_RHIP, M_RFOOT },        { M_LHIP, M_LFOOT } };
	for (const auto& b : bones) {
		ImVec2 a, c;
		if (project(pos(b[0]), a) && project(pos(b[1]), c)) dl->AddLine(a, c, IM_COL32(255, 235, 140, 200), 2.5f);
	}
	const ImVec2 mouse = ImGui::GetIO().MousePos;
	float best         = 12.0f * 12.0f;
	for (int i = 0; i < M_COUNT; ++i) {
		ImVec2 s;
		if (!project(pos(i), s)) continue;
		if (edit) {
			const float d = (s.x - mouse.x) * (s.x - mouse.x) + (s.y - mouse.y) * (s.y - mouse.y);
			if (d < best && dragMarker_ < 0) {
				best         = d;
				hoverMarker_ = i;
			}
		}
	}
	for (int i = 0; i < M_COUNT; ++i) {
		ImVec2 s;
		if (!project(pos(i), s)) continue;
		const bool hot   = i == hoverMarker_ || i == dragMarker_;
		const float r    = edit ? (hot ? 9.0f : 7.0f) : 4.0f;
		const ImU32 fill = i == M_PELVIS || i == M_NECK || i == M_HEADTOP ? IM_COL32(250, 210, 80, 255)
		                 : (i == M_LSHOULDER || i == M_LHAND || i == M_LHIP || i == M_LFOOT) ? IM_COL32(240, 100, 80, 255)
		                                                                                      : IM_COL32(90, 150, 245, 255);
		dl->AddCircleFilled(s, r, fill);
		dl->AddCircle(s, r, IM_COL32(20, 25, 15, 255), 0, 1.5f);
		if (edit && hot) dl->AddText(ImVec2(s.x + 12, s.y - 8), IM_COL32(255, 250, 230, 255), markerName(i));
	}
}

void App::dropFile(const std::string& path) { loadModel(path, false); }

void App::saveProject() const
{
	json j;
	j["source"]         = model_.path;
	j["scale"]          = fit_.scale;
	j["quarter"]        = fit_.quarter;
	j["lyingDown"]      = fit_.lyingDown;
	j["offsetY"]        = fit_.offsetY;
	j["offsetZ"]        = fit_.offsetZ;
	j["useSkeleton"]    = fit_.useSkeleton;
	j["straightenArms"] = fit_.straightenArms;
	j["straightenLegs"] = fit_.straightenLegs;
	j["symmetry"]       = fit_.symmetry;
	j["markersCustom"]  = fit_.markersCustom;
	j["textureMax"]     = exportOpt_.textureMax;
	json markers        = json::array();
	for (const Vec3& m : fit_.markers) markers.push_back({ m.x, m.y, m.z });
	j["markers"] = markers;
	std::vector<int> nt;
	for (char c : exportOpt_.noTint) nt.push_back(c ? 1 : 0);
	j["noTint"] = nt;
	std::ofstream f(fs::u8path(projectPath()));
	f << j.dump(1);
}

bool App::loadProject(std::string& source)
{
	std::ifstream f(fs::u8path(projectPath()));
	if (!f) return false;
	try {
		const json j = json::parse(f);
		source       = j.value("source", "");
	} catch (const std::exception&) {
		return false;
	}
	std::error_code ec;
	return !source.empty() && fs::exists(fs::u8path(source), ec);
}

bool App::save()
{
	if (exportDirty_) {
		export_      = buildParts(model_, rig_, olimar_, fit_, exportOpt_);
		exportDirty_ = false;
	}
	std::string error;
	if (!writeNhm(packPath(), export_, rig_, olimar_, error)) {
		say(std::string(L("No se pudo guardar: ", "Could not save: ")) + error, true);
		return false;
	}
	saveProject();
	// Si estaba quitado, el apagado ya no vale.
	std::error_code ec;
	fs::remove(fs::u8path(packPath() + ".off"), ec);
	char size[64];
	std::snprintf(size, sizeof(size), " (%.1f MB)", double(export_.bytes) / (1024.0 * 1024.0));
	say(std::string(L("¡Guardado! El juego usará tu Olimar la próxima vez que lo abras.", "Saved! The game will use your Olimar next time it starts.")) + size);
	return true;
}

void App::playInGame()
{
	std::string exe = cfg_.gameExe;
	std::error_code ec;
	if (exe.empty() || !fs::exists(exe, ec)) exe = nm::guessGameExe();
	if (exe.empty()) {
		const fs::path guess = fs::path(cfg_.gameDir()) /
#ifdef _WIN32
		                       "nectar.exe";
#else
		                       "nectar";
#endif
		if (fs::exists(guess, ec)) exe = guess.string();
	}
	if (exe.empty()) {
		say(L("Guardado. No encuentro el juego para abrirlo: ábrelo desde el launcher.", "Saved. Can't find the game to start it: open it from the launcher."), true);
		return;
	}
	std::string error;
	if (!nm::launchGame(exe, cfg_.gameDir(), {}, error)) say(error, true);
	else say(L("Guardado y abriendo el juego…", "Saved, starting the game…"));
}

void App::removeCustom()
{
	// No se borra: se aparta, y "Guardar" lo vuelve a poner.
	std::error_code ec;
	fs::rename(fs::u8path(packPath()), fs::u8path(packPath() + ".off"), ec);
	if (ec) say(ec.message(), true);
	else say(L("Quitado: el juego vuelve a usar el Olimar normal.", "Removed: the game uses the normal Olimar again."));
}

} // namespace mm
