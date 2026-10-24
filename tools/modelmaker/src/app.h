#pragma once

// Ventana del creador de modelos: un asistente de cinco pasos a la izquierda
// (modelo, colocar, esqueleto, animaciones, guardar) y la vista 3D con Olimar
// al lado.

#include "config.h"
#include "export_nhm.h"
#include "import.h"
#include "olimar.h"
#include "rig.h"
#include "view.h"

#include "imgui.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace mm {

class App {
public:
	bool init(const std::string& openPath, std::string& error);
	void frame();
	void dropFile(const std::string& path);
	bool wantsQuit() const { return quit_; }
	void requestQuit() { quit_ = true; }
	~App();

private:
	// Carpeta del juego y Olimar.
	nm::Config cfg_;
	std::string assetsDir_;
	bool gameReady_ = false;
	OlimarRig olimar_;
	std::vector<Anim> anims_;
	std::vector<int> animOrder_; ///< índices de anims_: primero las conocidas

	// El modelo (y el original, para simplificarlo otra vez).
	bool hasModel_ = false;
	ImpModel model_, source_;
	int targetTris_ = 30000;

	// Abrir y simplificar va en otro hilo (un modelo de 2 millones de
	// triángulos tarda medio minuto).
	std::thread worker_;
	std::atomic<bool> working_ { false }, workDone_ { false };
	ImpModel pending_, pendingSource_;
	std::string pendingError_, pendingPath_;
	bool pendingRestore_ = false, pendingResimplify_ = false;
	FitSettings fit_;
	RigResult rig_;
	ExportOptions exportOpt_;
	ExportResult export_;
	bool rigDirty_ = false, exportDirty_ = false;
	double lastRebuild_ = 0;
	std::vector<std::size_t> order_; ///< triángulos ordenados por material

	// Vista.
	View view_;
	OrbitCamera cam_;
	nm::M4 viewProj_;
	ImVec2 viewOrigin_, viewSize_;
	DynamicMesh modelMesh_, olimarMesh_;
	std::vector<GLuint> modelTex_, olimarTex_;
	int anim_      = -1; ///< -1: pose de reposo, con los puntos para editar
	float frame_   = 0;
	bool playing_  = true;
	float speed_   = 1.0f;
	int olimarMode_ = 1; ///< 0 oculto, 1 al lado, 2 encima (transparente)
	bool showSkeleton_ = true, showWeights_ = false;
	int dragMarker_ = -1, hoverMarker_ = -1;
	bool orbiting_  = false, panning_ = false;

	std::string status_;
	bool statusError_ = false;
	bool quit_        = false;
	bool askGameDir_  = false;

	bool setupGame(std::string& error);
	void loadModel(const std::string& path, bool restoreProject);
	void resimplify();
	void finishLoad();
	void rebuild();
	void uploadTextures();
	void fillMeshes();
	void resetCamera();

	void drawPanel(float width, float height);
	void drawView(float x, float width, float height);
	void drawOverlay(const std::vector<Mat34>& skin);
	bool project(const Vec3& p, ImVec2& out) const;
	void dragMarker(const ImVec2& mouse);

	std::string customDir() const;
	std::string packPath() const;
	std::string projectPath() const;
	void saveProject() const;
	bool loadProject(std::string& source);
	bool save();
	void playInGame();
	void removeCustom();
	void say(const std::string& text, bool error = false);
};

} // namespace mm
