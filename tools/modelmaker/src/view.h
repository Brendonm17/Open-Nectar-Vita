#pragma once

// Vista 3D del creador de modelos (OpenGL 3.3, a una textura que ImGui
// muestra). Las mallas se mueven en la CPU cada fotograma, como en el juego.

#include "gl.h"
#include "mm_math.h"
#include "nectar_assets/image.h"

#include <string>
#include <vector>

namespace mm {

struct GpuVertex {
	float p[3], n[3], uv[2], c[4];
};

struct OrbitCamera {
	Vec3 target { 0, 8, 0 };
	float yaw = 0.5f, pitch = 0.2f, distance = 45.0f, fovY = 0.75f;
	Vec3 eye() const;
	nm::M4 view() const;
	nm::M4 projection(float aspect) const;
};

/// Malla que se vuelve a subir cada fotograma; dibujada por tramos (uno por
/// material, cada uno con su textura).
struct DynamicMesh {
	GLuint vao = 0, vbo = 0;
	struct Range {
		int first = 0, count = 0;
		GLuint texture = 0;
		bool blend     = false;
	};
	std::vector<Range> ranges;
	std::vector<GpuVertex> data;
	void upload();
	~DynamicMesh();
};

class View {
public:
	bool init(std::string& error);
	void resize(int w, int h);
	int width() const { return width_; }
	int height() const { return height_; }
	GLuint colorTexture() const { return color_; }

	void begin(const nm::M4& viewProj);
	void drawMesh(const DynamicMesh& m, bool lit, float alpha = 1.0f);
	void drawLines(const std::vector<Vec3>& pairs, const float colour[4]);
	void end();

	/// `mesh` en una imagen aparte, con fondo transparente (la tarjeta del
	/// personaje en el selector de capitán).
	bool renderImage(int w, int h, const nm::M4& viewProj, const DynamicMesh& mesh, nectar::Image& out);

	~View();

private:
	GLuint program_ = 0, fbo_ = 0, color_ = 0, depth_ = 0, white_ = 0;
	GLint uMvp_ = -1, uUseTex_ = -1, uLit_ = -1, uAlpha_ = -1, uTex_ = -1;
	int width_ = 0, height_ = 0;
	nm::M4 viewProj_;
	DynamicMesh lines_;
};

/// Textura RGBA (con mipmaps). 0 si la imagen está vacía.
GLuint uploadTexture(const nectar::Image& img, bool repeat);

} // namespace mm
