/* Cg fragment-shader generator for the PS Vita. */
#include "vita_shaders.h"
#include "gl/pc_tev_shader.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

enum : uint8_t {
	CC_CPREV = 0, CC_APREV, CC_C0, CC_A0, CC_C1, CC_A1, CC_C2, CC_A2,
	CC_TEXC, CC_TEXA, CC_RASC, CC_RASA, CC_ONE, CC_HALF, CC_KONST, CC_ZERO
};
enum : uint8_t {
	CA_APREV = 0, CA_A0, CA_A1, CA_A2, CA_TEXA, CA_RASA, CA_KONST, CA_ZERO
};
enum : uint8_t { CMP_NEVER = 0, CMP_ALWAYS = 7 };

// Precision: TEV in half.
const char* color_arg_expr(uint8_t id)
{
	switch (id) {
	case CC_CPREV: return "prev.rgb";
	case CC_APREV: return "half3(prev.a, prev.a, prev.a)";
	case CC_C0:    return "c0.rgb";
	case CC_A0:    return "half3(c0.a, c0.a, c0.a)";
	case CC_C1:    return "c1.rgb";
	case CC_A1:    return "half3(c1.a, c1.a, c1.a)";
	case CC_C2:    return "c2.rgb";
	case CC_A2:    return "half3(c2.a, c2.a, c2.a)";
	case CC_TEXC:  return "tex.rgb";
	case CC_TEXA:  return "half3(tex.a, tex.a, tex.a)";
	case CC_RASC:  return "rast.rgb";
	case CC_RASA:  return "half3(rast.a, rast.a, rast.a)";
	case CC_ONE:   return "half3(1.0h, 1.0h, 1.0h)";
	case CC_HALF:  return "half3(0.5h, 0.5h, 0.5h)";
	case CC_KONST: return "konst.rgb";
	default:       return "half3(0.0h, 0.0h, 0.0h)";
	}
}

const char* alpha_arg_expr(uint8_t id)
{
	switch (id) {
	case CA_APREV: return "prev.a";
	case CA_A0:    return "c0.a";
	case CA_A1:    return "c1.a";
	case CA_A2:    return "c2.a";
	case CA_TEXA:  return "tex.a";
	case CA_RASA:  return "rast.a";
	case CA_KONST: return "konst.a";
	default:       return "0.0h";
	}
}

const char* bias_literal(uint8_t bias)
{
	return bias == 1 ? "0.5h" : bias == 2 ? "-0.5h" : "0.0h";
}

const char* scale_literal(uint8_t scale)
{
	return scale == 1 ? "2.0h" : scale == 2 ? "4.0h" : scale == 3 ? "0.5h" : "1.0h";
}

const char* register_name(uint8_t reg)
{
	switch (reg) {
	case 0:  return "prev";
	case 1:  return "c0";
	case 2:  return "c1";
	default: return "c2";
	}
}

const char* swizzle_component(uint8_t component)
{
	switch (component) {
	case 0:  return "r";
	case 1:  return "g";
	case 2:  return "b";
	default: return "a";
	}
}

bool swap_table_is_identity(const uint8_t table[4])
{
	return table[0] == 0 && table[1] == 1 && table[2] == 2 && table[3] == 3;
}

void emit_swap(std::string& out, const char* target, const uint8_t table[4])
{
	if (swap_table_is_identity(table)) return;
	out += "\t\t";
	out += target;
	out += " = ";
	out += target;
	out += ".";
	for (int i = 0; i < 4; ++i) out += swizzle_component(table[i]);
	out += ";\n";
}

bool compare_is_static(uint8_t func) { return func == CMP_NEVER || func == CMP_ALWAYS; }

bool combine_static(uint8_t op, bool test0, bool test1)
{
	switch (op) {
	case 0:  return test0 && test1;
	case 1:  return test0 || test1;
	case 2:  return test0 != test1;
	default: return test0 == test1;
	}
}

// value and reference on the 0..255 scale.
std::string compare_expr(uint8_t func, const char* value, const char* reference)
{
	char buffer[256];
	switch (func) {
	case 1: snprintf(buffer, sizeof(buffer), "(%s < %s)", value, reference); break;
	case 2: snprintf(buffer, sizeof(buffer), "(abs(%s - %s) < 0.5)", value, reference); break;
	case 3: snprintf(buffer, sizeof(buffer), "(%s <= %s)", value, reference); break;
	case 4: snprintf(buffer, sizeof(buffer), "(%s > %s)", value, reference); break;
	case 5: snprintf(buffer, sizeof(buffer), "(abs(%s - %s) >= 0.5)", value, reference); break;
	case 6: snprintf(buffer, sizeof(buffer), "(%s >= %s)", value, reference); break;
	default: return std::string();
	}
	return std::string(buffer);
}

// Alpha comparisons against ref/255 with a half-level offset.
std::string threshold_expr(uint8_t func, const char* value, const char* reference)
{
	char buffer[256];
	switch (func) {
	// Threshold compares take a pre-offset reference.
	case 1: snprintf(buffer, sizeof(buffer), "(%s < (half)%s)", value, reference); break;                        // LESS
	case 2: snprintf(buffer, sizeof(buffer), "(abs(float(%s) - %s) < 0.0019607843)", value, reference); break;  // EQUAL
	case 3: snprintf(buffer, sizeof(buffer), "(%s < (half)%s)", value, reference); break;                        // LEQUAL
	case 4: snprintf(buffer, sizeof(buffer), "(%s >= (half)%s)", value, reference); break;                       // GREATER
	case 5: snprintf(buffer, sizeof(buffer), "(abs(float(%s) - %s) >= 0.0019607843)", value, reference); break; // NEQUAL
	case 6: snprintf(buffer, sizeof(buffer), "(%s >= (half)%s)", value, reference); break;                       // GEQUAL
	default: return std::string();
	}
	return std::string(buffer);
}

// Combiner value ranges.
struct Rng { float lo, hi; };
static Rng gRegC[4], gRegA[4];
static void tev_ranges_reset() { for (int i = 0; i < 4; ++i) { gRegC[i] = { -4.0f, 4.0f }; gRegA[i] = { -4.0f, 4.0f }; } }
static Rng rng_color_arg(uint8_t id)
{
	switch (id) {
	case CC_CPREV: return gRegC[0]; case CC_C0: return gRegC[1]; case CC_C1: return gRegC[2]; case CC_C2: return gRegC[3];
	case CC_APREV: return gRegA[0]; case CC_A0: return gRegA[1]; case CC_A1: return gRegA[2]; case CC_A2: return gRegA[3];
	case CC_ONE:   return { 1.0f, 1.0f };
	case CC_HALF:  return { 0.5f, 0.5f };
	case CC_ZERO:  return { 0.0f, 0.0f };
	default:       return { 0.0f, 1.0f };   // TEXC, TEXA, RASC, RASA, KONST
	}
}
static Rng rng_alpha_arg(uint8_t id)
{
	switch (id) {
	case CA_APREV: return gRegA[0]; case CA_A0: return gRegA[1]; case CA_A1: return gRegA[2]; case CA_A2: return gRegA[3];
	case CA_ZERO:  return { 0.0f, 0.0f };
	default:       return { 0.0f, 1.0f };   // TEXA, RASA, KONST
	}
}
static Rng rng_mix(Rng a, Rng b, Rng c)
{
	Rng r = { 1.0e30f, -1.0e30f };
	for (int k = 0; k < 8; ++k) {
		const float av = (k & 1) ? a.hi : a.lo, bv = (k & 2) ? b.hi : b.lo, cv = (k & 4) ? c.hi : c.lo;
		const float v = av * (1.0f - cv) + bv * cv;
		r.lo = std::min(r.lo, v); r.hi = std::max(r.hi, v);
	}
	return r;
}
static Rng rng_combine(Rng a, Rng b, Rng c, Rng d, uint8_t op, uint8_t bias, uint8_t scale)
{
	Rng r;
	if (op >= 8) {   // compare ops: d + (cond ? c : 0)
		r = { d.lo + std::min(0.0f, c.lo), d.hi + std::max(0.0f, c.hi) };
	} else {
		const Rng m = rng_mix(a, b, c);
		r = (op == 0) ? Rng{ d.lo + m.lo, d.hi + m.hi } : Rng{ d.lo - m.hi, d.hi - m.lo };
		if (bias != 0 || scale != 0) {
			const float bv = bias == 1 ? 0.5f : bias == 2 ? -0.5f : 0.0f;
			const float sv = scale == 1 ? 2.0f : scale == 2 ? 4.0f : scale == 3 ? 0.5f : 1.0f;
			r = { (r.lo + bv) * sv, (r.hi + bv) * sv };
		}
	}
	return r;
}

void emit_stage(std::string& out, const PcTevShaderKey& key, int index)
{
	const PcTevStageKey& stage = key.stages[index];
	char line[512];

	snprintf(line, sizeof(line), "\t// stage %d\n\t{\n", index);
	out += line;

	if (stage.texMap >= 0) {
		snprintf(line, sizeof(line), "\t\thalf4 tex = h4tex2D(uTex%d, vTexCoord%d);\n",
		         int(stage.texMap), int(stage.texCoord));
	} else {
		snprintf(line, sizeof(line), "\t\thalf4 tex = half4(1.0h, 1.0h, 1.0h, 1.0h);\n");
	}
	out += line;
	emit_swap(out, "tex", key.swapTable[stage.texSwapSel & 3]);

	if (stage.rasChannel < 0) {
		out += "\t\thalf4 rast = half4(0.0h, 0.0h, 0.0h, 0.0h);\n";
	} else if (stage.rasChannel == 1) {
		out += "\t\thalf4 rast = rast1;\n";
	} else {
		out += "\t\thalf4 rast = rast0;\n";
	}
	emit_swap(out, "rast", key.swapTable[stage.rasSwapSel & 3]);

	snprintf(line, sizeof(line), "\t\thalf4 konst = (half4)uTevKonst_%d;\n", index);
	out += line;

	// colour
	const char* ca = color_arg_expr(stage.colorIn[0]);
	const char* cb = color_arg_expr(stage.colorIn[1]);
	const char* cc = color_arg_expr(stage.colorIn[2]);
	const char* cd = color_arg_expr(stage.colorIn[3]);
	Rng cRange = rng_combine(rng_color_arg(stage.colorIn[0]), rng_color_arg(stage.colorIn[1]), rng_color_arg(stage.colorIn[2]),
	                         rng_color_arg(stage.colorIn[3]), stage.colorOp, stage.colorBias, stage.colorScale);

	if (stage.colorOp >= 8) {
		const bool isEqual = (stage.colorOp & 1) != 0;
		if (stage.colorOp >= 14) {
			snprintf(line, sizeof(line),
			         "\t\tfloat3 qa = floor(saturate(float3(%s)) * 255.0 + 0.5);\n"
			         "\t\tfloat3 qb = floor(saturate(float3(%s)) * 255.0 + 0.5);\n"
			         "\t\tbool3 cpass = %s;\n"
			         "\t\thalf3 cResult = %s + half3(cpass.x ? (%s).r : 0.0h, cpass.y ? (%s).g : 0.0h, cpass.z ? (%s).b : 0.0h);\n",
			         ca, cb, isEqual ? "(qa == qb)" : "(qa > qb)", cd, cc, cc, cc);
			out += line;
		} else {
			const int components = (stage.colorOp < 10) ? 1 : (stage.colorOp < 12) ? 2 : 3;
			auto pack = [components](const char* name) {
				std::string expr(name);
				expr += ".r";
				if (components >= 2) { expr += " + "; expr += name; expr += ".g * 256.0"; }
				if (components >= 3) { expr += " + "; expr += name; expr += ".b * 65536.0"; }
				return expr;
			};
			snprintf(line, sizeof(line),
			         "\t\tfloat3 qa = floor(saturate(float3(%s)) * 255.0 + 0.5);\n"
			         "\t\tfloat3 qb = floor(saturate(float3(%s)) * 255.0 + 0.5);\n",
			         ca, cb);
			out += line;
			out += "\t\tfloat av = " + pack("qa") + ";\n";
			out += "\t\tfloat bv = " + pack("qb") + ";\n";
			snprintf(line, sizeof(line),
			         "\t\tbool cpass = %s;\n"
			         "\t\thalf3 cResult = %s + (cpass ? %s : half3(0.0h, 0.0h, 0.0h));\n",
			         isEqual ? "(av == bv)" : "(av > bv)", cd, cc);
			out += line;
		}
	} else {
		snprintf(line, sizeof(line), "\t\thalf3 cMix = %s * (half3(1.0h, 1.0h, 1.0h) - %s) + %s * %s;\n",
		         ca, cc, cb, cc);
		out += line;
		snprintf(line, sizeof(line), "\t\thalf3 cResult = %s %c cMix;\n",
		         cd, stage.colorOp == 0 ? '+' : '-');
		out += line;
		if (stage.colorBias != 0 || stage.colorScale != 0) {
			snprintf(line, sizeof(line), "\t\tcResult = (cResult + %s) * %s;\n",
			         bias_literal(stage.colorBias), scale_literal(stage.colorScale));
			out += line;
		}
	}
	{
		const Rng target = stage.colorClamp ? Rng{ 0.0f, 1.0f } : Rng{ -4.0f, 4.0f };
		if (cRange.lo < target.lo - 1.0e-4f || cRange.hi > target.hi + 1.0e-4f) {
			snprintf(line, sizeof(line), "\t\tcResult = clamp(cResult, %s);\n",
			         stage.colorClamp ? "0.0h, 1.0h" : "-4.0h, 4.0h");
			out += line;
			cRange = { std::max(cRange.lo, target.lo), std::min(cRange.hi, target.hi) };
		}
	}

	// alpha
	const char* aa = alpha_arg_expr(stage.alphaIn[0]);
	const char* ab = alpha_arg_expr(stage.alphaIn[1]);
	const char* ac = alpha_arg_expr(stage.alphaIn[2]);
	const char* ad = alpha_arg_expr(stage.alphaIn[3]);
	Rng aRange = rng_combine(rng_alpha_arg(stage.alphaIn[0]), rng_alpha_arg(stage.alphaIn[1]), rng_alpha_arg(stage.alphaIn[2]),
	                         rng_alpha_arg(stage.alphaIn[3]), stage.alphaOp, stage.alphaBias, stage.alphaScale);

	if (stage.alphaOp >= 14) {
		const bool isEqual = (stage.alphaOp & 1) != 0;
		snprintf(line, sizeof(line),
		         "\t\tfloat aqa = floor(saturate(float(%s)) * 255.0 + 0.5);\n"
		         "\t\tfloat aqb = floor(saturate(float(%s)) * 255.0 + 0.5);\n"
		         "\t\thalf aResult = %s + (%s ? %s : 0.0h);\n",
		         aa, ab, ad, isEqual ? "(aqa == aqb)" : "(aqa > aqb)", ac);
		out += line;
	} else {
		snprintf(line, sizeof(line), "\t\thalf aMix = %s * (1.0h - %s) + %s * %s;\n",
		         aa, ac, ab, ac);
		out += line;
		snprintf(line, sizeof(line), "\t\thalf aResult = %s %c aMix;\n",
		         ad, stage.alphaOp == 0 ? '+' : '-');
		out += line;
		if (stage.alphaOp < 8 && (stage.alphaBias != 0 || stage.alphaScale != 0)) {
			snprintf(line, sizeof(line), "\t\taResult = (aResult + %s) * %s;\n",
			         bias_literal(stage.alphaBias), scale_literal(stage.alphaScale));
			out += line;
		}
	}
	{
		const Rng target = stage.alphaClamp ? Rng{ 0.0f, 1.0f } : Rng{ -4.0f, 4.0f };
		if (aRange.lo < target.lo - 1.0e-4f || aRange.hi > target.hi + 1.0e-4f) {
			snprintf(line, sizeof(line), "\t\taResult = clamp(aResult, %s);\n",
			         stage.alphaClamp ? "0.0h, 1.0h" : "-4.0h, 4.0h");
			out += line;
			aRange = { std::max(aRange.lo, target.lo), std::min(aRange.hi, target.hi) };
		}
	}

	snprintf(line, sizeof(line), "\t\t%s.rgb = cResult;\n\t\t%s.a = aResult;\n\t}\n",
	         register_name(stage.colorOutReg), register_name(stage.alphaOutReg));
	out += line;
	gRegC[stage.colorOutReg & 3] = cRange;
	gRegA[stage.alphaOutReg & 3] = aRange;
}

} // namespace

const char* vita_cg_vertex_source(void)
{
	// Vertex Cg hook.
	return nullptr;
}

std::string vita_cg_fragment_source(const PcTevShaderKey& key) { return vita_cg_fragment_source(key, false); }

bool vita_tev_has_alpha_test(const PcTevShaderKey& key) { return !(key.alphaComp0 == CMP_ALWAYS && key.alphaComp1 == CMP_ALWAYS); }

std::string vita_cg_fragment_source(const PcTevShaderKey& key, bool softAlpha)
{
	const int stageCount = key.numStages < 1 ? 1 : (key.numStages > 16 ? 16 : key.numStages);

	bool usesSampler[8] = {};
	bool usesChannel1 = false;
	for (int i = 0; i < stageCount; ++i) {
		const PcTevStageKey& stage = key.stages[i];
		if (stage.texMap >= 0 && stage.texMap < 8) usesSampler[stage.texMap] = true;
		if (stage.rasChannel == 1) usesChannel1 = true;
	}
	// Register-1 fold.
	if (key.useMaterialRgb1 == 2) usesChannel1 = true;

	const bool needsRef0 = !compare_is_static(key.alphaComp0);
	const bool needsRef1 = !compare_is_static(key.alphaComp1);

	std::string out;
	out.reserve(4096);
	char line[256];

	out += "// Generated Cg TEV fragment shader (Open Nectar / PS Vita)\n";

	out += "void main(\n";
	// Rasterised colours.
	out += "\thalf4 vColor : COLOR0,\n";
	if (usesChannel1) out += "\thalf4 vColor1 : TEXCOORD5,\n";
	{
		// Texcoords read by textured stages.
		const unsigned tcMask = vita_tev_needed_texcoords(key);
		char tcl[64];
		for (int t = 0; t < 4; ++t) {
			if (tcMask & (1u << t)) {
				snprintf(tcl, sizeof(tcl), "\tfloat2 vTexCoord%d : TEXCOORD%d,\n", t, t);
				out += tcl;
			}
		}
	}
	for (int i = 0; i < 8; ++i) {
		if (!usesSampler[i]) continue;
		snprintf(line, sizeof(line), "\tuniform sampler2D uTex%d : TEXUNIT%d,\n", i, i);
		out += line;
	}
	out += "\tuniform half4 uTevPrev,\n";
	out += "\tuniform half4 uTevReg0,\n";
	out += "\tuniform half4 uTevReg1,\n";
	out += "\tuniform half4 uTevReg2,\n";
	for (int i = 0; i < stageCount; ++i) {
		snprintf(line, sizeof(line), "\tuniform half4 uTevKonst_%d,\n", i);
		out += line;
	}
	if (needsRef0) out += "\tuniform half uAlphaRef0,\n";
	if (needsRef1) out += "\tuniform half uAlphaRef1,\n";
	// Fog uniform.
	out += "\tuniform half4 uFogColour,\n";
	out += "\tfloat vFog : TEXCOORD6,\n";
	out += "\tout half4 out_color : COLOR)\n{\n";

	// Raster channels.
	out += "\thalf4 rast0 = vColor;\n";
	if (usesChannel1) out += "\thalf4 rast1 = vColor1;\n";

	out += "\thalf4 prev = (half4)uTevPrev;\n";
	out += "\thalf4 c0 = (half4)uTevReg0;\n";
	out += key.useMaterialRgb1 == 2 ? "\thalf4 c1 = rast1;\n" : "\thalf4 c1 = (half4)uTevReg1;\n";
	out += "\thalf4 c2 = (half4)uTevReg2;\n";

	tev_ranges_reset();
	for (int i = 0; i < stageCount; ++i) emit_stage(out, key, i);

	// Fog after the stages, colour only.
	static const char kFogLine[] =
		"\tif (uFogColour.a > 0.0h) prev.rgb = lerp(prev.rgb, uFogColour.rgb, (half)saturate(vFog));\n";
	// Soft variant: fog before the zeroing.
	if (softAlpha) out += kFogLine;

	if (compare_is_static(key.alphaComp0) && compare_is_static(key.alphaComp1)) {
		const bool passes = combine_static(key.alphaTestOp,
		                                   key.alphaComp0 == CMP_ALWAYS,
		                                   key.alphaComp1 == CMP_ALWAYS);
		if (!passes) out += softAlpha ? "\tprev = half4(0.0h, 0.0h, 0.0h, 0.0h);\n" : "\tdiscard;\n";
	} else {
		// 8-bit compare as an alpha threshold.
		std::string test0 = threshold_expr(key.alphaComp0, "prev.a", "uAlphaRef0");
		std::string test1 = threshold_expr(key.alphaComp1, "prev.a", "uAlphaRef1");
		if (test0.empty()) test0 = (key.alphaComp0 == CMP_ALWAYS) ? "true" : "false";
		if (test1.empty()) test1 = (key.alphaComp1 == CMP_ALWAYS) ? "true" : "false";
		const char* combiner = key.alphaTestOp == 0 ? "&&"
		                     : key.alphaTestOp == 1 ? "||"
		                     : key.alphaTestOp == 2 ? "!=" : "==";
		out += "\tif (!(" + test0 + " " + combiner + " " + test1 + ")) "
		     + (softAlpha ? "prev = half4(0.0h, 0.0h, 0.0h, 0.0h);" : "discard;") + "\n";
	}

	if (!softAlpha) out += kFogLine;
	out += "\tout_color = prev;\n";
	out += "}\n";
	return out;
}

// Varyings a configuration reads.

unsigned vita_tev_needed_texcoords(const PcTevShaderKey& key)
{
	const int stageCount = key.numStages < 1 ? 1 : (key.numStages > 16 ? 16 : key.numStages);
	unsigned mask = 0;
	for (int i = 0; i < stageCount; ++i) {
		const PcTevStageKey& st = key.stages[i];
		if (st.texMap >= 0) mask |= 1u << (st.texCoord & 3);
	}
	return mask;
}

bool vita_tev_uses_channel1(const PcTevShaderKey& key)
{
	const int stageCount = key.numStages < 1 ? 1 : (key.numStages > 16 ? 16 : key.numStages);
	for (int i = 0; i < stageCount; ++i) {
		if (key.stages[i].rasChannel == 1) return true;
	}
	return key.useMaterialRgb1 == 2;
}
