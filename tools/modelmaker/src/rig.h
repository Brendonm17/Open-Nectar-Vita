#pragma once

// Del modelo importado al Olimar propio: se coloca sobre Olimar (tamaño,
// giro, suelo), se reparte cada vértice entre sus articulaciones y se guardan
// los pivotes del personaje (hombros, cadera, cuello) para que el juego mueva
// sus brazos desde sus propios hombros (pc_hd_models.cpp, retargetSkin).

#include "import.h"
#include "olimar.h"

#include <array>
#include <string>
#include <vector>

namespace mm {

/// Puntos del esqueleto que el usuario puede mover.
enum Marker : int {
	M_PELVIS,
	M_NECK,
	M_HEADTOP,
	M_RSHOULDER,
	M_LSHOULDER,
	M_RHAND,
	M_LHAND,
	M_RHIP,
	M_LHIP,
	M_RFOOT,
	M_LFOOT,
	M_STEMTOP, ///< Pikmin: donde el juego pone la hoja (articulación 6)
	M_COUNT
};
const char* markerName(int marker);
/// El del otro lado (-1 si va en el centro).
int markerMirror(int marker);

struct FitSettings {
	float scale    = 1.0f;  ///< sobre la altura de Olimar
	int quarter    = 0;     ///< giros de 90° alrededor de Y
	bool lyingDown = false; ///< viene con Z hacia arriba
	float offsetY = 0, offsetZ = 0;
	bool useSkeleton  = true; ///< pesos del esqueleto del modelo si los trae
	bool straightenArms = true; ///< brazos en la dirección de los de Olimar
	bool straightenLegs = false;
	bool symmetry       = true;
	bool markersCustom  = false; ///< el usuario los ha movido: no recalcular
	bool pikmin         = false; ///< rig de un Pikmin: tallo (4-6) con su punta para la hoja
	bool keepPlacement  = false; ///< ya está en su sitio (un model.nhm que se vuelve a editar)
	bool addStem        = true;  ///< Pikmin: añadir el tallo original si el modelo no trae
	int stemMaterial    = -1;    ///< material (del modelo) con la textura del tallo original
	std::array<Vec3, M_COUNT> markers {}; ///< en el espacio de Olimar, antes de enderezar
	int textureMax = 1024;
};

/// Qué es cada hueso del modelo importado.
enum BoneKind : int {
	B_NONE,
	B_HIPS,
	B_SPINE,
	B_NECK,
	B_HEAD,
	B_SHOULDER,
	B_UPPERARM,
	B_LOWERARM,
	B_HAND,
	B_UPPERLEG,
	B_LOWERLEG,
	B_FOOT,
	B_ANTENNA,
};
struct BoneInfo {
	BoneKind kind = B_NONE;
	int side      = 0;  ///< -1 derecha (-x), +1 izquierda (+x), 0 centro
	int joint     = J_SPINE; ///< articulación de Olimar que lo mueve
	bool guessed  = false;   ///< heredado del padre (nombre desconocido)
};

struct RigResult {
	std::vector<SkinVertex> verts; ///< colocados y enderezados; joint[] = articulación de Olimar
	std::vector<int> triMaterial;
	std::vector<Vec3> unbent; ///< posiciones antes de enderezar (para editar los puntos)
	std::array<Vec3, kJoints> pivots {}; ///< pose de reposo propia (traslación); giro = el de Olimar
	std::array<Vec3, M_COUNT> markers {}; ///< ya enderezados (para dibujarlos)
	Vec3 min, max;
	bool fromSkeleton   = false;
	float mappedShare   = 0; ///< parte del peso que va por huesos reconocidos
	int autoVertices    = 0; ///< vértices con pesos automáticos
	int stemTriangles   = 0; ///< del tallo original añadido
	std::vector<BoneInfo> bones;
	std::string skeletonNote;
};

/// Matriz del modelo importado al espacio de Olimar.
Mat34 fitTransform(const ImpModel& model, const OlimarRig& olimar, const FitSettings& fit);
/// Ajuste automático: orientación (con los huesos, si hay), tamaño de Olimar
/// y marcadores recolocados.
void autoFit(const ImpModel& model, const OlimarRig& olimar, FitSettings& fit);
/// Marcadores estimados (con los huesos o mirando la malla).
std::array<Vec3, M_COUNT> estimateMarkers(const ImpModel& model, const OlimarRig& olimar, const FitSettings& fit);
std::vector<BoneInfo> classifyBones(const ImpModel& model, const Mat34& fit);

RigResult buildRig(const ImpModel& model, const OlimarRig& olimar, const FitSettings& fit);

/// Matrices de piel de cada articulación para una pose del juego (`anim`,
/// mundo de cada articulación), igual que retargetSkin en pc_hd_models.cpp.
void retargetSkin(const OlimarRig& olimar, const std::array<Vec3, kJoints>& pivots, const std::vector<Mat34>& anim,
                  std::vector<Mat34>& skin);

} // namespace mm
