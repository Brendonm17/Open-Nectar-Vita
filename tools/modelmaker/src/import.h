#pragma once

// Modelos del usuario: glTF 2.0 (.gltf/.glb), VRM (VRoid), FBX y OBJ, pasados a
// triángulos sueltos en la pose de reposo, con sus huesos si los tienen.

#include "olimar.h"

#include <string>
#include <vector>

namespace mm {

struct ImpMaterial {
	std::string name;
	nectar::Image texture;            ///< vacía: solo color
	float color[4]   = { 1, 1, 1, 1 }; ///< multiplica la textura
	bool doubleSided = false;
	enum Alpha { Opaque, Mask, Blend } alpha = Opaque;
	float cutoff      = 0.5f;
	bool vertexColors = false; ///< sus vértices traen color (sin textura se usa)
};

struct ImpBone {
	std::string name;
	std::string humanoid; ///< hueso humanoide del VRM ("leftUpperArm"...) si lo hay
	int parent = -1;
	Vec3 head; ///< posición en reposo
};

struct ImpModel {
	std::string path, format;
	/// 3 vértices por triángulo, de frente en sentido antihorario (glTF). En
	/// joint[] van índices de `bones`; peso total 0 = sin hueso.
	std::vector<SkinVertex> verts;
	std::vector<int> triMaterial;
	std::vector<ImpMaterial> materials;
	std::vector<ImpBone> bones;
	bool skinned     = false;
	bool facesMinusZ = false; ///< VRM 0.x mira hacia -Z
	std::string warnings;     ///< una por línea
	/// Todos los archivos leídos (el modelo, sus .bin, texturas, .mtl...): se
	/// copian al proyecto para volver a abrirlo aunque se borre el original.
	std::vector<std::string> files;

	std::size_t triangles() const { return verts.size() / 3; }
};

/// Según la extensión. false con el motivo (en `error`).
bool importModel(const std::string& path, ImpModel& out, std::string& error);

bool importGltf(const std::string& path, ImpModel& out, std::string& error);
bool importFbx(const std::string& path, ImpModel& out, std::string& error);
bool importObjModel(const std::string& path, ImpModel& out, std::string& error);
/// Un pack ya hecho (model.nhm): para volver a editar un personaje sin proyecto.
bool importNhm(const std::string& path, ImpModel& out, std::string& error);

} // namespace mm
