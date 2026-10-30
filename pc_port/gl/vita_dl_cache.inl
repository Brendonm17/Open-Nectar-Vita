// Vita: static display-list cache and GPU matrix palette
#ifdef __vita__

using VitaDlVertex = Vertex;

struct VitaDlChunk {
    uint32_t first = 0, count = 0;   // index range inside the list's IBO
    float    bmin[3], bmax[3];
};

struct VitaDlEntry {
    GLuint   vbo = 0;
    GLuint   ibo = 0;           // u16 triangle-list indices into vbo
    const char* gpu = nullptr;
    GLsizei  count = 0;         // index count (GL_TRIANGLES)
    uint32_t lastFrame = 0;
    uint8_t  maxSlot = 0;       // highest palette slot referenced
    bool     usesMtxIdx = false; // false: current matrix at draw time
    bool     cacheable = false; // false: negative entry
    bool     filled = false;    // decoded at least once
    uint64_t fullHash = 0;      // content hash at decode time
    size_t   bytes = 0;         // bytes added to sVitaDlBytes
    // Object-space bounds
    float    bmin[3] = { 0, 0, 0 }, bmax[3] = { 0, 0, 0 };
    bool     hasBounds = false;
    float    minAlpha = 1.0f;   // lowest vertex alpha (alpha-test elision input)
    float    uvMin[4][2], uvMax[4][2];   // texcoord bounds
    uint64_t uvHash = 0;        // hash of uvMin/uvMax
    // Spatial cells of big single-matrix lists
    std::vector<VitaDlChunk> chunks;
    float    slotMin[10][3], slotMax[10][3];
    uint16_t slotMask = 0;
};

static constexpr uint32_t kVitaDlIdleFrames = 900;
static constexpr int      kVitaDlMaxSlots   = 10;    // GX_PNMTX0..9 per list
static_assert(kVitaDlMaxSlots == kVgxMaxPalSlots, "the render thread's palette writer (rtunif) holds as many slots");
static constexpr size_t   kVitaRunMaxVerts  = 65535; // u16 indices
static constexpr GLuint   kVitaAttrMtxIdx   = 7;     // ATTR7 in nectar_v.cg
static constexpr size_t   kVitaDlVramBudget = 40u * 1024u * 1024u;

static std::unordered_map<uint64_t, VitaDlEntry> sVitaDlCache;
static std::vector<VitaDlVertex> sVitaDlVerts;    // every primitive's vertices, in order
static std::vector<uint16_t>     sVitaDlIdx;      // triangle-list indices into sVitaDlVerts
static uint32_t sVitaDlFrame = 0;

// Lazily maintained hashes of the decode state
static uint64_t sVitaVatHash = 0, sVitaDescHash = 0;

static uint64_t vita_dl_state_hash() {
    if (sVitaVatDirty) {
        u32 packed[GX_MAX_VTXFMT * GX_VA_MAX_ATTR];
        int n = 0;
        for (int f = 0; f < GX_MAX_VTXFMT; ++f)
            for (int a = 0; a < GX_VA_MAX_ATTR; ++a) {
                const VertexFormatState& s = sVtxFormats[f][a];
                packed[n++] = (u32(s.count) << 16) | (u32(s.type) << 8) | s.frac;
            }
        sVitaVatHash = hash_bytes(0x9E3779B97F4A7C15ull, packed, sizeof(packed));
        sVitaVatDirty = false;
    }
    if (sVitaDescDirty) {
        u32 packed[GX_VA_MAX_ATTR * 3];
        int n = 0;
        for (int a = 0; a < GX_VA_MAX_ATTR; ++a) {
            packed[n++] = u32(sVtxDesc[a]);
            packed[n++] = u32(reinterpret_cast<uintptr_t>(sVtxArrays[a].base));
            packed[n++] = sVtxArrays[a].stride;
        }
        sVitaDescHash = hash_bytes(0xC2B2AE3D27D4EB4Full, packed, sizeof(packed));
        sVitaDescDirty = false;
    }
    return sVitaVatHash ^ (sVitaDescHash * 0x100000001B3ull);
}

static uint64_t vita_dl_key(const void* list, u32 nbytes) {
    uint64_t h = 14695981039346656037ull;
    const uintptr_t addr = reinterpret_cast<uintptr_t>(list);
    h = hash_bytes(h, &addr, sizeof(addr));
    h = hash_bytes(h, &nbytes, sizeof(nbytes));
    const u8* p = static_cast<const u8*>(list);
    if (nbytes <= 256) {
        h = hash_bytes(h, p, nbytes);
    } else {
        h = hash_bytes(h, p, 128);
        h = hash_bytes(h, p + nbytes - 128, 128);
    }
    const uint64_t st = vita_dl_state_hash();
    h = hash_bytes(h, &st, sizeof(st));
    return h;
}
static uint64_t vita_dl_full_hash(const void* list, u32 nbytes) {
    return hash_bytes(0x51A3B7C9D2E4F601ull, list, nbytes);
}

static bool vita_dl_decode(const u8* list, u32 nbytes, std::vector<VitaDlVertex>& verts,
                           std::vector<uint16_t>& idx, u8& maxSlot, bool& usesMtxIdx) {
    const u8* cursor = list;
    const u8* const end = list + nbytes;
    verts.clear();
    idx.clear();
    maxSlot = 0;
    usesMtxIdx = sVtxDesc[GX_VA_PNMTXIDX] != GX_NONE;
    while (cursor < end) {
        const u8 command = *cursor++;
        if (command == 0 || command == GX_CMD_NOP) continue;
        const u8 opcode = command & GX_OPCODE_MASK;
        if (opcode < GX_CMD_DRAW_QUADS || opcode > GX_CMD_DRAW_POINTS) return false;
        if (opcode == GX_LINES || opcode == GX_LINESTRIP || opcode == GX_POINTS) return false;
        const u8 format = command & GX_VAT_MASK;
        u16 vertexCount = 0;
        if (!read_be_u16(cursor, end, vertexCount)) return false;

        const size_t base = verts.size();
        if (base + vertexCount > kVitaRunMaxVerts) return false;
        struct AttrPlan { uint8_t attr, kind, inlineSize, target, compSize; GXAttrType desc; const u8* base; u32 stride; GXCompType type; u8 frac; GXCompCnt count; };
        AttrPlan plan[GX_VA_TEX7 - GX_VA_PNMTXIDX + 1]; int nplan = 0;
        for (int attrNumber = GX_VA_PNMTXIDX; attrNumber <= GX_VA_TEX7; ++attrNumber) {
            GXAttr attr = static_cast<GXAttr>(attrNumber);
            GXAttrType desc = sVtxDesc[attr];
            if (desc == GX_NONE) {
                if (attr == GX_VA_NRM && sVtxDesc[GX_VA_NBT] != GX_NONE) { attr = GX_VA_NBT; desc = sVtxDesc[attr]; }
                else continue;
            }
            AttrPlan& p = plan[nplan];
            p.attr = uint8_t(attr); p.desc = desc; p.base = nullptr; p.stride = 0; p.inlineSize = 0; p.target = 0;
            const VertexFormatState& fmtState = sVtxFormats[format][attr];
            p.type = fmtState.type; p.frac = fmtState.frac; p.count = fmtState.count; p.compSize = get_comptype_size(fmtState.type);
            if (attr <= GX_VA_TEX7MTXIDX) {
                if (desc != GX_DIRECT) return false;
                p.kind = 0;
            } else if (desc == GX_DIRECT) {
                p.kind = 1;
                p.inlineSize = attr_inline_size(attr, fmtState);
                if (p.inlineSize == 0) return false;
            } else {
                p.kind = 2;
                p.base = sVtxArrays[attr].base; p.stride = sVtxArrays[attr].stride;
            }
            if (attr == GX_VA_POS) p.target = 1;
            else if (attr == GX_VA_CLR0) p.target = 2;
            else if (attr == GX_VA_NRM || attr == GX_VA_NBT) p.target = 4;
            else if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX3) p.target = uint8_t(5 + int(attr) - int(GX_VA_TEX0));
            ++nplan;
        }
        for (u16 vi = 0; vi < vertexCount; ++vi) {
            VitaDlVertex v;
            v.x = v.y = v.z = 0.0f;
            v.nx = 0.0f; v.ny = 0.0f; v.nz = 1.0f;
            v.r = v.g = v.b = v.a = 1.0f;
            v.mtxIdx = 0.0f;   // no PNMTXIDX attribute
            for (int tc = 0; tc < 4; ++tc) { v.tex[tc][0] = 0.0f; v.tex[tc][1] = 0.0f; }
            for (int pi = 0; pi < nplan; ++pi) {
                const AttrPlan& p = plan[pi];
                if (p.kind == 0) {
                    if (cursor >= end) return false;
                    const u8 value = *cursor++;
                    if (p.attr == GX_VA_PNMTXIDX) {
                        if (value % 3 != 0 || value / 3 >= kVitaDlMaxSlots) return false;
                        const u8 slot = value / 3;
                        v.mtxIdx = float(slot);
                        if (slot > maxSlot) maxSlot = slot;
                    }
                    continue;
                }
                const u8* element;
                if (p.kind == 1) {
                    if (end - cursor < p.inlineSize) return false;
                    element = cursor;
                    cursor += p.inlineSize;
                } else {
                    u16 index = 0;
                    if (!read_attribute_index(p.desc, cursor, end, index)) return false;
                    if (!p.base || p.stride == 0) continue;
                    element = p.base + size_t(index) * p.stride;
                }
                switch (p.target) {
                case 1:
                    v.x = read_attr_float(element, p.type, p.frac);
                    v.y = read_attr_float(element + p.compSize, p.type, p.frac);
                    v.z = (p.count == GX_POS_XYZ) ? read_attr_float(element + 2 * p.compSize, p.type, p.frac) : 0.0f;
                    break;
                case 2: {
                    u8 r, g, b, a;
                    read_attr_color(element, p.type, r, g, b, a);
                    v.r = r / 255.0f; v.g = g / 255.0f; v.b = b / 255.0f; v.a = a / 255.0f;
                    break;
                }
                case 4:
                    v.nx = read_attr_float(element, p.type, p.frac);
                    v.ny = read_attr_float(element + p.compSize, p.type, p.frac);
                    v.nz = read_attr_float(element + 2 * p.compSize, p.type, p.frac);
                    break;
                case 5: case 6: case 7: case 8: {
                    const int tc = int(p.target) - 5;
                    v.tex[tc][0] = read_attr_float(element, p.type, p.frac);
                    v.tex[tc][1] = (p.count == GX_TEX_ST) ? read_attr_float(element + p.compSize, p.type, p.frac) : 0.0f;
                    break;
                }
                default: break;   // CLR1, TEX4-7: skipped
                }
            }
            verts.push_back(v);
        }

        // Triangulation as in append_primitive_to_batch
        const size_t n = vertexCount;
        auto I = [&](size_t k) { idx.push_back(uint16_t(base + k)); };
        switch (opcode) {
        case GX_TRIANGLES:
            for (size_t i = 0; i < n; ++i) I(i);
            break;
        case GX_TRIANGLESTRIP:
            for (size_t i = 2; i < n; ++i) {
                if ((i & 1) == 0) { I(i - 2); I(i - 1); }
                else              { I(i - 1); I(i - 2); }
                I(i);
            }
            break;
        case GX_TRIANGLEFAN:
            for (size_t i = 2; i < n; ++i) { I(0); I(i - 1); I(i); }
            break;
        case GX_QUADS:
            for (size_t i = 0; i + 3 < n; i += 4) {
                I(i); I(i + 1); I(i + 2);
                I(i); I(i + 2); I(i + 3);
            }
            break;
        default:
            return false;
        }
    }
    return true;
}

static void vita_palette_snapshot(int dst, u32 gxSlot, float pos[][4], float nrm[][4]) {
    if (gxSlot >= 64) gxSlot = 0;
    const float* m = sPosMatrix[gxSlot];
    const float* nm = sNrmMatrix[gxSlot];
    for (int r = 0; r < 3; ++r) {
        pos[dst * 3 + r][0] = m[r];  pos[dst * 3 + r][1] = m[4 + r];
        pos[dst * 3 + r][2] = m[8 + r]; pos[dst * 3 + r][3] = m[12 + r];
        nrm[dst * 3 + r][0] = nm[r]; nrm[dst * 3 + r][1] = nm[3 + r];
        nrm[dst * 3 + r][2] = nm[6 + r]; nrm[dst * 3 + r][3] = 0.0f;
    }
}

// CPU-path draws
static void vita_upload_palette_slot0(const float* posColMajor4x4, const float* nrmColMajor3x3) {
    sVitaPalProvValid = false;
    vita_palprog_forget(sCurrentProgram);
    PcXlatSink xs;
    vgx_write_slot0(posColMajor4x4, nrmColMajor3x3, sLoc, xs);
}

static void vita_upload_palette_rows(int slots, bool useCurrentMatrix, unsigned which) {
    float pos[kVitaDlMaxSlots * 3][4];
    float nrm[kVitaDlMaxSlots * 3][4];
    for (int s = 0; s < slots; ++s)
        vita_palette_snapshot(s, useCurrentMatrix ? sCurrentPosMtxId : u32(s) * 3, pos, nrm);
    if (which & 1u) vita_uniform4fv(sLoc.posPal, slots * 3, &pos[0][0]);
    if (which & 2u) vita_uniform4fv(sLoc.nrmPal, slots * 3, &nrm[0][0]);
}

static int vita_palette_image_class(GLint loc, int slots) {
    if (!vita_slot_is_image(loc) || vita_slot_int(loc) || sCurrentProgram >= GLuint(kVitaMaxPrograms)) return -1;
    const VitaUniformImage& im = sVitaImg[sCurrentProgram];
    const int st = vita_slot_stage(loc);
    if (!im.img[st]) return -1;
    uint32_t words = uint32_t(slots) * 12u;
    const uint32_t maxWords = vita_slot_words(loc);
    if (words > maxWords) words = maxWords;
    const uint32_t off = vita_slot_offset(loc);
    const uint32_t bytes = words * 4u;
    if (bytes == 0 || off + bytes > im.size[st]) return 0;
    return 1;
}
static int vita_palette_image_write(GLint loc, int slots, bool useCurrentMatrix, bool nrm) {
    const int cls = vita_palette_image_class(loc, slots);
    if (cls != 1) return cls;
    VitaUniformImage& im = sVitaImg[sCurrentProgram];
    const int st = vita_slot_stage(loc);
    uint8_t* img = im.img[st];
    uint32_t words = uint32_t(slots) * 12u;
    const uint32_t maxWords = vita_slot_words(loc);
    if (words > maxWords) words = maxWords;
    const uint32_t off = vita_slot_offset(loc);
    const uint32_t bytes = words * 4u;
    uint8_t* dst = img + off;
    bool changed = false;
    uint32_t w = 0;
    for (int s = 0; s < slots && w < words; ++s) {
        u32 gx = useCurrentMatrix ? sCurrentPosMtxId : u32(s) * 3;
        if (gx >= 64) gx = 0;
        const float* m = nrm ? sNrmMatrix[gx] : sPosMatrix[gx];
        for (int r = 0; r < 3 && w < words; ++r) {
            float row[4];
            if (nrm) { row[0] = m[r]; row[1] = m[3 + r]; row[2] = m[6 + r]; row[3] = 0.0f; }
            else     { row[0] = m[r]; row[1] = m[4 + r]; row[2] = m[8 + r]; row[3] = m[12 + r]; }
            for (int c = 0; c < 4 && w < words; ++c, ++w) {
                uint32_t want, have;
                memcpy(&want, &row[c], 4);
                memcpy(&have, dst + w * 4u, 4);
                if (want != have) { memcpy(dst + w * 4u, &want, 4); changed = true; }
            }
        }
    }
    if (changed) {
        if (off < im.lo[st]) im.lo[st] = off;
        if (off + bytes > im.hi[st]) im.hi[st] = off + bytes;
    }
    return 1;
}

struct VitaPaletteProv {
    GLuint   program;
    int      slots;
    bool     useCurrent;
    u32      currentId;
    uint32_t pg[kVitaDlMaxSlots], ng[kVitaDlMaxSlots];
};
static VitaPaletteProv sVitaPalProv;

struct VitaPalProgEntry {
    uint32_t epoch;          // 0: empty
    uint16_t program;        // program ids are < kVitaMaxPrograms (16385)
    uint8_t  slots, useCurrent, currentId, pad[3];
    uint32_t pg[kVitaDlMaxSlots], ng[kVitaDlMaxSlots];
};
static constexpr uint32_t kVitaPalProgEntries = 1024;
static VitaPalProgEntry sVitaPalProgTab[kVitaPalProgEntries];
static uint32_t sVitaPalProgEpoch = 1;
static void vita_palprog_forget(GLuint prog) {
    VitaPalProgEntry& e = sVitaPalProgTab[prog & (kVitaPalProgEntries - 1u)];
    if (e.program == prog) e.epoch = 0;
}
static void vita_palprog_forget_all() {
    if (++sVitaPalProgEpoch == 0) { memset(sVitaPalProgTab, 0, sizeof(sVitaPalProgTab)); sVitaPalProgEpoch = 1; }
}
static bool vita_palprog_hit(int slots, bool useCurrentMatrix) {
    const GLuint prog = sCurrentProgram;
    if (prog == 0 || prog >= GLuint(kVitaMaxPrograms)) return false;
    const VitaPalProgEntry& e = sVitaPalProgTab[prog & (kVitaPalProgEntries - 1u)];
    const u32 cur = sCurrentPosMtxId < 64 ? sCurrentPosMtxId : 0;
    if (e.epoch != sVitaPalProgEpoch || e.program != prog || int(e.slots) != slots || (e.useCurrent != 0) != useCurrentMatrix
        || (useCurrentMatrix && e.currentId != cur)) return false;
    for (int s = 0; s < slots; ++s) {
        const u32 gx = useCurrentMatrix ? cur : u32(s) * 3;
        if (e.pg[s] != sPosMtxGen[gx] || e.ng[s] != sNrmMtxGen[gx]) return false;
    }
    return true;
}
static void vita_palprog_note() {
    const GLuint prog = sCurrentProgram;
    if (prog == 0 || prog >= GLuint(kVitaMaxPrograms)) return;
    VitaPalProgEntry& e = sVitaPalProgTab[prog & (kVitaPalProgEntries - 1u)];
    if (!sVitaPalProvValid) { if (e.program == prog) e.epoch = 0; return; }
    const int slots = sVitaPalProv.slots;
    e.epoch = sVitaPalProgEpoch;
    e.program = uint16_t(prog);
    e.slots = uint8_t(slots);
    e.useCurrent = sVitaPalProv.useCurrent ? 1 : 0;
    e.currentId = uint8_t(sVitaPalProv.currentId);
    memcpy(e.pg, sVitaPalProv.pg, sizeof(uint32_t) * size_t(slots));
    memcpy(e.ng, sVitaPalProv.ng, sizeof(uint32_t) * size_t(slots));
}

// The program already holds these rows
static bool vita_palette_prov_hit(int slots, bool useCurrentMatrix) {
    const u32 cur = sCurrentPosMtxId < 64 ? sCurrentPosMtxId : 0;
    if (sVitaPalProvValid && sVitaPalProv.program == sCurrentProgram && sVitaPalProv.slots == slots
        && sVitaPalProv.useCurrent == useCurrentMatrix && (!useCurrentMatrix || sVitaPalProv.currentId == cur)) {
        bool same = true;
        for (int s = 0; s < slots && same; ++s) {
            const u32 gx = useCurrentMatrix ? cur : u32(s) * 3;
            same = sVitaPalProv.pg[s] == sPosMtxGen[gx] && sVitaPalProv.ng[s] == sNrmMtxGen[gx];
        }
        if (same) return true;
    }
    return false;
}
static void vita_palette_prov_note(int slots, bool useCurrentMatrix, int pos, int nrm) {
    const u32 cur = sCurrentPosMtxId < 64 ? sCurrentPosMtxId : 0;
    sVitaPalProvValid = pos == 1 && nrm == 1 && slots >= 1 && slots <= kVitaDlMaxSlots;
    if (!sVitaPalProvValid) return;
    sVitaPalProv.program = sCurrentProgram;
    sVitaPalProv.slots = slots;
    sVitaPalProv.useCurrent = useCurrentMatrix;
    sVitaPalProv.currentId = cur;
    for (int s = 0; s < slots; ++s) {
        const u32 gx = useCurrentMatrix ? cur : u32(s) * 3;
        sVitaPalProv.pg[s] = sPosMtxGen[gx];
        sVitaPalProv.ng[s] = sNrmMtxGen[gx];
    }
}
static void vita_upload_palette_fused(int slots, bool useCurrentMatrix) {
    if (vita_palette_prov_hit(slots, useCurrentMatrix)) return;
    const int pos = vita_palette_image_write(sLoc.posPal, slots, useCurrentMatrix, false);
    const int nrm = sLoc.nrmPal == -1 ? 1 : vita_palette_image_write(sLoc.nrmPal, slots, useCurrentMatrix, true);
    if (pos < 0 || nrm < 0) vita_upload_palette_rows(slots, useCurrentMatrix, (pos < 0 ? 1u : 0u) | (nrm < 0 ? 2u : 0u));
    vita_palette_prov_note(slots, useCurrentMatrix, pos, nrm);
}

static void vita_upload_palette(int slots, bool useCurrentMatrix) {
    if (sLoc.posPal == -1) return;
    if (sVitaRtUnif) {
        if (vita_palette_prov_hit(slots, useCurrentMatrix)) return;
        if (vita_palprog_hit(slots, useCurrentMatrix)) return;
        const int pos = vita_palette_image_class(sLoc.posPal, slots);
        const int nrm = sLoc.nrmPal == -1 ? 1 : vita_palette_image_class(sLoc.nrmPal, slots);
        if (vita_rtgx_ship_palette(slots, useCurrentMatrix)) {
            vita_palette_prov_note(slots, useCurrentMatrix, pos, nrm);
            vita_palprog_note();
        } else {
            sVitaPalProvValid = false;   // the rows did not go out
            vita_palprog_forget(sCurrentProgram);
        }
        return;
    }
    vita_upload_palette_fused(slots, useCurrentMatrix);
}

static void vita_rebind_material_textures() {
    bool used[8] = {};
    for (u8 stage = 0; stage < sNumTevStages && stage < GX_MAXTEVSTAGE; ++stage) {
        const TevStageState& st = sTevStages[stage];
        if (st.textureEnabled && st.texMap >= GX_TEXMAP0 && st.texMap < 8) used[int(st.texMap)] = true;
    }
    int lastActiveMap = 0;
    for (int map = 0; map < 8; ++map) {
        if (!used[map]) continue;
        const GLuint wanted = sHasActiveTextures[map] ? sActiveGLTextures[map] : 0;
        if (sBoundTextures[map] != wanted) {
            glActiveTexture_ptr(GL_TEXTURE0 + map);
            glBindTexture(GL_TEXTURE_2D, wanted);
            sBoundTextures[map] = wanted;
            lastActiveMap = map;
        }
        if (wanted && !sVitaFrameFirstDraw && sVitaGxmBound[map] != wanted) {
            rec_gxm_fragment_texture(unsigned(map), wanted);
            sVitaGxmBound[map] = wanted;
        }
    }
    if (lastActiveMap != 0) glActiveTexture_ptr(GL_TEXTURE0);
}

static void vita_dl_draw_ranges(VitaDlEntry& e, u32 cullMtx);   // visible cells of a chunked list
static void vita_dl_draw(VitaDlEntry& e, u32 cullMtx) {
    pc_gfx_flush_batch();
    if (e.count == 0) return;

    // Material state
    sCurrentPrimType = GX_TRIANGLES;
    sVerticesPretransformed = true;     // matrices excluded from the key
    sVitaNeedPalette = e.usesMtxIdx && e.maxSlot > 0;
    sVitaPaletteSlots = e.usesMtxIdx ? int(e.maxSlot) + 1 : 1;
    sVitaRasAlphaMin = e.minAlpha;
    memcpy(sVitaListUvMin, e.uvMin, sizeof(sVitaListUvMin));
    memcpy(sVitaListUvMax, e.uvMax, sizeof(sVitaListUvMax));
    sVitaListUvHash = e.uvHash;
    sVitaListUvValid = true;
    const uint64_t key = compute_batch_state_key();
    if (!sVitaAppliedKeyValid || key != sVitaAppliedKey) {
        VitaApplyMemo* m = vita_apply_memo_find(key);
        if (m && vita_prog_applied_is(m->program, key)) {
            vita_apply_memo_bind(*m);
            vita_rebind_material_textures();
        } else if (m) {
            vita_apply_memo_bind(*m);
            sVitaApplyingForCache = true;
            sVitaApplyKey = key; sVitaApplyKeyValid = true;
            apply_batch_state(true);
            sVitaApplyKeyValid = false;
            sVitaApplyingForCache = false;
            vita_prog_applied_set(sCurrentProgram, key);
        } else {
            sVitaApplyingForCache = true;
            sVitaApplyKey = key; sVitaApplyKeyValid = true;
            apply_batch_state();
            sVitaApplyKeyValid = false;
            sVitaApplyingForCache = false;
            vita_apply_memo_store(key);
            vita_prog_applied_set(sCurrentProgram, key);
        }
        sVitaAppliedKey = key;
        sVitaAppliedKeyValid = true;
    } else if (sVitaFrameFirstDraw) {
        vita_rebind_material_textures();
    }
    if (sVitaDrawRejected) {   // its alpha test cannot pass
        sVitaRasAlphaMin = 0.0f;
        sVitaListUvValid = false;
        return;
    }
    vita_upload_palette(e.usesMtxIdx ? int(e.maxSlot) + 1 : 1, !e.usesMtxIdx);

    vita_dl_draw_ranges(e, cullMtx);
    sVitaRasAlphaMin = 0.0f;   // immediate-mode draws
    sVitaListUvValid = false;
}

static bool vita_dl_box_outside(const float* bmin, const float* bmax, u32 id) {
    static float mvpCache[64][16];
    static uint32_t projGen[64], posGen[64];
    static bool valid[64];
    if (id >= 64) id = 0;
    float* mvp = mvpCache[id];
    if (!valid[id] || projGen[id] != sProjMtxGen || posGen[id] != sPosMtxGen[id]) {
        const float* P = sProjMatrix;      // column-major
        const float* M = sPosMatrix[id];
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                mvp[c * 4 + r] = P[r] * M[c * 4] + P[4 + r] * M[c * 4 + 1] + P[8 + r] * M[c * 4 + 2] + P[12 + r] * M[c * 4 + 3];
        projGen[id] = sProjMtxGen; posGen[id] = sPosMtxGen[id]; valid[id] = true;
    }
    int outL = 0, outR = 0, outB = 0, outT = 0, outBehind = 0;
    for (int i = 0; i < 8; ++i) {
        const float x = (i & 1) ? bmax[0] : bmin[0];
        const float y = (i & 2) ? bmax[1] : bmin[1];
        const float z = (i & 4) ? bmax[2] : bmin[2];
        const float cx = mvp[0] * x + mvp[4] * y + mvp[8] * z + mvp[12];
        const float cy = mvp[1] * x + mvp[5] * y + mvp[9] * z + mvp[13];
        const float cw = mvp[3] * x + mvp[7] * y + mvp[11] * z + mvp[15];
        if (cw <= 0.0f) { ++outBehind; continue; }
        if (cx < -cw) ++outL;
        if (cx >  cw) ++outR;
        if (cy < -cw) ++outB;
        if (cy >  cw) ++outT;
    }
    return outBehind == 8 || outL == 8 || outR == 8 || outB == 8 || outT == 8;
}

static void vita_dl_chunk_leaf(const std::vector<VitaDlVertex>& verts, const uint16_t* tris, size_t ntris,
                               std::vector<uint16_t>& out, std::vector<VitaDlChunk>& chunks) {
    VitaDlChunk c;
    c.first = uint32_t(out.size());
    c.count = uint32_t(ntris * 3);
    c.bmin[0] = c.bmin[1] = c.bmin[2] = 3.0e38f;
    c.bmax[0] = c.bmax[1] = c.bmax[2] = -3.0e38f;
    for (size_t t = 0; t < ntris * 3; ++t) {
        const uint16_t i = tris[t];
        out.push_back(i);
        const VitaDlVertex& v = verts[i];
        if (v.x < c.bmin[0]) c.bmin[0] = v.x; if (v.x > c.bmax[0]) c.bmax[0] = v.x;
        if (v.y < c.bmin[1]) c.bmin[1] = v.y; if (v.y > c.bmax[1]) c.bmax[1] = v.y;
        if (v.z < c.bmin[2]) c.bmin[2] = v.z; if (v.z > c.bmax[2]) c.bmax[2] = v.z;
    }
    chunks.push_back(c);
}
static void vita_dl_chunk_split(const std::vector<VitaDlVertex>& verts, uint16_t* tris, size_t ntris, int depth,
                                std::vector<uint16_t>& out, std::vector<VitaDlChunk>& chunks) {
    if (ntris <= size_t(kVitaDlChunkTris) || depth >= 8) { vita_dl_chunk_leaf(verts, tris, ntris, out, chunks); return; }
    float lo[3] = { 3.0e38f, 3.0e38f, 3.0e38f }, hi[3] = { -3.0e38f, -3.0e38f, -3.0e38f };
    auto centroid = [&](size_t t, float* c) {
        const VitaDlVertex& a = verts[tris[t * 3]]; const VitaDlVertex& b = verts[tris[t * 3 + 1]]; const VitaDlVertex& d = verts[tris[t * 3 + 2]];
        c[0] = (a.x + b.x + d.x) * (1.0f / 3.0f); c[1] = (a.y + b.y + d.y) * (1.0f / 3.0f); c[2] = (a.z + b.z + d.z) * (1.0f / 3.0f);
    };
    for (size_t t = 0; t < ntris; ++t) {
        float c[3]; centroid(t, c);
        for (int k = 0; k < 3; ++k) { if (c[k] < lo[k]) lo[k] = c[k]; if (c[k] > hi[k]) hi[k] = c[k]; }
    }
    int axis = 0;
    if (hi[1] - lo[1] > hi[axis] - lo[axis]) axis = 1;
    if (hi[2] - lo[2] > hi[axis] - lo[axis]) axis = 2;
    const float mid = 0.5f * (lo[axis] + hi[axis]);
    size_t left = 0;
    for (size_t t = 0; t < ntris; ++t) {
        float c[3]; centroid(t, c);
        if (c[axis] < mid) {
            for (int k = 0; k < 3; ++k) { const uint16_t tmp = tris[left * 3 + k]; tris[left * 3 + k] = tris[t * 3 + k]; tris[t * 3 + k] = tmp; }
            ++left;
        }
    }
    if (left == 0 || left == ntris) { vita_dl_chunk_leaf(verts, tris, ntris, out, chunks); return; }
    vita_dl_chunk_split(verts, tris, left, depth + 1, out, chunks);
    vita_dl_chunk_split(verts, tris + left * 3, ntris - left, depth + 1, out, chunks);
}
static void vita_dl_build_chunks(const std::vector<VitaDlVertex>& verts, std::vector<uint16_t>& idx, std::vector<VitaDlChunk>& chunks) {
    static std::vector<uint16_t> tris, out;
    tris = idx;
    out.clear();
    out.reserve(idx.size());
    chunks.clear();
    vita_dl_chunk_split(verts, tris.data(), tris.size() / 3, 0, out, chunks);
    idx = out;
}

// The visible cells of a chunked list
static void vita_dl_draw_ranges(VitaDlEntry& e, u32 cullMtx) {
    if (e.chunks.empty()) { vita_fast_draw_indexed(e.vbo, e.ibo, 0, e.count); return; }
    uint32_t runFirst = 0, runCount = 0;
    for (const VitaDlChunk& c : e.chunks) {
        if (vita_dl_box_outside(c.bmin, c.bmax, cullMtx)) {
            if (runCount) { vita_fast_draw_indexed(e.vbo, e.ibo, GLsizei(runFirst), GLsizei(runCount)); runCount = 0; }
            continue;
        }
        if (runCount == 0) runFirst = c.first;
        runCount += c.count;   // cells are contiguous and in order
    }
    if (runCount) vita_fast_draw_indexed(e.vbo, e.ibo, GLsizei(runFirst), GLsizei(runCount));
}

struct VitaDlPubBounds {
    unsigned seq;          // odd while being rewritten
    const void* list;      // null: nothing published
    u32 nbytes;            // decoded length
    const void* posBase;   // position array decoded against
    int usesMtxIdx;        // 1: slot 0, 0: current matrix
    bool poisoned;
    float bmin[3];
    float bmax[3];
};
static VitaDlPubBounds sVitaDlPub[4096];
static inline VitaDlPubBounds& vita_dl_pub_slot(const void* list) { return sVitaDlPub[(uintptr_t(list) >> 4) & 4095u]; }
// Game thread
extern "C" int pc_gx_dl_bounds(const void* list, u32 nbytes, const void* posBase, float* bmin, float* bmax, int* usesMtxIdx) {
    const VitaDlPubBounds& b = vita_dl_pub_slot(list);
    const unsigned s1 = __atomic_load_n(&b.seq, __ATOMIC_ACQUIRE);
    if (s1 & 1u) return 0;
    const void* l = b.list;
    const u32 nb = b.nbytes;
    const void* pb = b.posBase;
    const int u = b.usesMtxIdx;
    const bool poisoned = b.poisoned;
    float lo[3], hi[3];
    for (int i = 0; i < 3; ++i) { lo[i] = b.bmin[i]; hi[i] = b.bmax[i]; }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&b.seq, __ATOMIC_RELAXED) != s1) return 0;
    if (l != list || poisoned || nb != nbytes || pb != posBase) return 0;
    for (int i = 0; i < 3; ++i) { bmin[i] = lo[i]; bmax[i] = hi[i]; }
    *usesMtxIdx = u;
    return 1;
}
// Translator thread
static void vita_dl_publish(const void* list, u32 nbytes, const VitaDlEntry& e) {
    VitaDlPubBounds& b = vita_dl_pub_slot(list);
    const bool ok = e.hasBounds && e.cacheable && (!e.usesMtxIdx || e.maxSlot == 0);
    const void* pos = sVtxArrays[GX_VA_POS].base;
    bool poison = b.list == list && b.poisoned;
    if (ok && b.list == list && !b.poisoned
        && (b.nbytes != nbytes || b.posBase != pos || b.usesMtxIdx != int(e.usesMtxIdx)
            || memcmp(b.bmin, e.bmin, sizeof(b.bmin)) != 0 || memcmp(b.bmax, e.bmax, sizeof(b.bmax)) != 0))
        poison = true;
    __atomic_store_n(&b.seq, b.seq + 1u, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    if (poison) {
        b.list = list;
        b.poisoned = true;
    } else if (!ok) {
        b.list = nullptr;
        b.poisoned = false;
    } else {
        b.list = list;
        b.nbytes = nbytes;
        b.posBase = pos;
        b.usesMtxIdx = e.usesMtxIdx ? 1 : 0;
        b.poisoned = false;
        for (int i = 0; i < 3; ++i) { b.bmin[i] = e.bmin[i]; b.bmax[i] = e.bmax[i]; }
    }
    __atomic_store_n(&b.seq, b.seq + 1u, __ATOMIC_RELEASE);
}
struct VitaDlAddrMemo { const void* list; u32 nbytes; uint64_t state; uint64_t key; VitaDlEntry* entry; };
static VitaDlAddrMemo sVitaDlAddrMemo[4096];
static bool vita_dl_try_draw(const void* list, u32 nbytes) {
    if (!list || nbytes == 0 || sLoc.posPal == -1) return false;
    if (vita_dl_arrays_volatile()) return false;   // decoded from the live arrays (pc_gfx_note_array_volatile)
    VitaDlAddrMemo& am = sVitaDlAddrMemo[(uintptr_t(list) >> 4) & 4095u];
    const uint64_t stateNow = vita_dl_state_hash();
    uint64_t key;
    std::unordered_map<uint64_t, VitaDlEntry>::iterator it;
    if (am.list == list && am.nbytes == nbytes && am.state == stateNow && am.entry && am.entry->filled) {
        key = am.key;
        it  = sVitaDlCache.end();
    } else {
        key = vita_dl_key(list, nbytes);
        it  = sVitaDlCache.find(key);
        am.list = nullptr;
    }
    const bool memoHit = (it == sVitaDlCache.end() && am.list == list);
    if (!memoHit && it != sVitaDlCache.end() && it->second.filled
        && (((sVitaDlFrame + uint32_t(key)) & 31u) == 0)) {
        // Periodic full check
        const uint64_t full = vita_dl_full_hash(list, nbytes);
        if (full != it->second.fullHash) {
            VitaDlEntry& stale = it->second;
            am.list = nullptr;   // geometry changed
            if (stale.vbo) { glDeleteBuffers(1, &stale.vbo); glDeleteBuffers(1, &stale.ibo); }
            stale.vbo = stale.ibo = 0; stale.count = 0; stale.gpu = nullptr;
            stale.cacheable = false; stale.filled = false;
            vita_dl_publish(list, nbytes, stale);   // geometry changed
        }
    }
    if (!memoHit && (it == sVitaDlCache.end() || !it->second.filled)) {
        if (it == sVitaDlCache.end()) it = sVitaDlCache.emplace(key, VitaDlEntry()).first;
        VitaDlEntry& e = it->second;
        e.filled = true;
        e.fullHash = vita_dl_full_hash(list, nbytes);
        u8 maxSlot = 0;
        bool usesMtxIdx = false;
        const bool ok = vita_dl_decode(static_cast<const u8*>(list), nbytes, sVitaDlVerts, sVitaDlIdx, maxSlot, usesMtxIdx);
        e.hasBounds = ok && !sVitaDlVerts.empty();
        if (e.hasBounds) {
            e.bmin[0] = e.bmax[0] = sVitaDlVerts[0].x; e.bmin[1] = e.bmax[1] = sVitaDlVerts[0].y; e.bmin[2] = e.bmax[2] = sVitaDlVerts[0].z;
            for (const VitaDlVertex& v : sVitaDlVerts) {
                if (v.x < e.bmin[0]) e.bmin[0] = v.x; if (v.x > e.bmax[0]) e.bmax[0] = v.x;
                if (v.y < e.bmin[1]) e.bmin[1] = v.y; if (v.y > e.bmax[1]) e.bmax[1] = v.y;
                if (v.z < e.bmin[2]) e.bmin[2] = v.z; if (v.z > e.bmax[2]) e.bmax[2] = v.z;
            }
        }
        e.minAlpha = 1.0f;
        if (ok) for (const VitaDlVertex& v : sVitaDlVerts) if (v.a < e.minAlpha) e.minAlpha = v.a;
        for (int t = 0; t < 4; ++t) for (int c = 0; c < 2; ++c) { e.uvMin[t][c] = ok ? 1.0e30f : -1.0e30f; e.uvMax[t][c] = ok ? -1.0e30f : 1.0e30f; }
        if (ok) for (const VitaDlVertex& v : sVitaDlVerts)
            for (int t = 0; t < 4; ++t) for (int c = 0; c < 2; ++c) {
                if (v.tex[t][c] < e.uvMin[t][c]) e.uvMin[t][c] = v.tex[t][c];
                if (v.tex[t][c] > e.uvMax[t][c]) e.uvMax[t][c] = v.tex[t][c];
            }
        e.uvHash = hash_bytes(hash_bytes(0x3C6EF372FE94F82Bull, e.uvMin, sizeof(e.uvMin)), e.uvMax, sizeof(e.uvMax));
        e.slotMask = 0;
        if (ok && usesMtxIdx && maxSlot > 0 && maxSlot < 10) {
            for (const VitaDlVertex& v : sVitaDlVerts) {
                int s = int(v.mtxIdx + 0.5f);
                if (s < 0 || s > int(maxSlot)) continue;
                if (!((e.slotMask >> s) & 1)) {
                    e.slotMask |= uint16_t(1u << s);
                    e.slotMin[s][0] = e.slotMax[s][0] = v.x; e.slotMin[s][1] = e.slotMax[s][1] = v.y; e.slotMin[s][2] = e.slotMax[s][2] = v.z;
                    continue;
                }
                if (v.x < e.slotMin[s][0]) e.slotMin[s][0] = v.x; if (v.x > e.slotMax[s][0]) e.slotMax[s][0] = v.x;
                if (v.y < e.slotMin[s][1]) e.slotMin[s][1] = v.y; if (v.y > e.slotMax[s][1]) e.slotMax[s][1] = v.y;
                if (v.z < e.slotMin[s][2]) e.slotMin[s][2] = v.z; if (v.z > e.slotMax[s][2]) e.slotMax[s][2] = v.z;
            }
        }
        e.chunks.clear();
        if (ok && (!usesMtxIdx || maxSlot == 0) && sVitaDlIdx.size() >= size_t(kVitaDlChunkTris) * 6)
            vita_dl_build_chunks(sVitaDlVerts, sVitaDlIdx, e.chunks);
        const size_t bytes = sVitaDlVerts.size() * sizeof(VitaDlVertex) + sVitaDlIdx.size() * sizeof(uint16_t);
        if (ok && sVitaDlBytes + bytes <= kVitaDlVramBudget) {
            e.cacheable = true;
            e.count = (GLsizei)sVitaDlIdx.size();
            e.maxSlot = maxSlot;
            e.usesMtxIdx = usesMtxIdx;
            if (e.count > 0) {
                glGenBuffers_ptr(1, &e.vbo);
                glBindBuffer_ptr(GL_ARRAY_BUFFER, e.vbo);
                rec_glBufferDataMapped(GL_ARRAY_BUFFER, GLsizeiptr(sVitaDlVerts.size() * sizeof(VitaDlVertex)),
                                       sVitaDlVerts.data(), GL_STATIC_DRAW, e.vbo);
                glGenBuffers_ptr(1, &e.ibo);
                glBindBuffer_ptr(GL_ELEMENT_ARRAY_BUFFER, e.ibo);
                glBufferData_ptr(GL_ELEMENT_ARRAY_BUFFER, sVitaDlIdx.size() * sizeof(uint16_t),
                                 sVitaDlIdx.data(), GL_STATIC_DRAW);
                sVitaDlBytes += bytes;
                e.bytes = bytes;
            }
        }
        vita_dl_publish(list, nbytes, e);
    }
    VitaDlEntry& e = memoHit ? *am.entry : it->second;
    if (!memoHit) { am.list = list; am.nbytes = nbytes; am.state = stateNow; am.key = key; am.entry = &e; }
    e.lastFrame = sVitaDlFrame;
    if (!e.cacheable) return false;
    const bool cullable = e.hasBounds && e.count > 0 && (!e.usesMtxIdx || e.maxSlot == 0);
    const u32 cullMtx = e.usesMtxIdx ? 0u : sCurrentPosMtxId;
    bool culled = false;
    if (e.count > 0) {
        if (cullable) {
            culled = vita_dl_box_outside(e.bmin, e.bmax, cullMtx);
        } else if (e.usesMtxIdx && e.slotMask) {
            culled = true;
            for (int s = 0; s <= int(e.maxSlot) && s < 10 && culled; ++s)
                if ((e.slotMask >> s) & 1) culled = vita_dl_box_outside(e.slotMin[s], e.slotMax[s], u32(s) * 3u);
        }
    }
    if (culled) return true;   // nothing of it is on screen
    vita_dl_draw(e, cullMtx);
    return true;
}

static void vita_dl_flush_all() {
    for (auto it = sVitaDlCache.begin(); it != sVitaDlCache.end(); ++it) {
        VitaDlEntry& e = it->second;
        if (e.vbo) { glDeleteBuffers(1, &e.vbo); glDeleteBuffers(1, &e.ibo); }
        sVitaDlBytes -= e.bytes;
    }
    sVitaDlCache.clear();
    for (VitaDlAddrMemo& m : sVitaDlAddrMemo) { m.list = nullptr; m.entry = nullptr; }
    for (VitaDlPubBounds& b : sVitaDlPub) {
        __atomic_store_n(&b.seq, b.seq + 1u, __ATOMIC_RELEASE);
        b.list = nullptr;
        b.poisoned = false;
        __atomic_store_n(&b.seq, b.seq + 1u, __ATOMIC_RELEASE);
    }
}

// Once per frame from pc_gfx_present
static void vita_dl_end_frame() {
    ++sVitaDlFrame;
    if ((sVitaDlFrame % 120) != 0) return;
    for (auto it = sVitaDlCache.begin(); it != sVitaDlCache.end(); ++it) {
        VitaDlEntry& e = it->second;
        if (!e.vbo || sVitaDlFrame - e.lastFrame <= kVitaDlIdleFrames) continue;
        glDeleteBuffers(1, &e.vbo); glDeleteBuffers(1, &e.ibo);
        sVitaDlBytes -= e.bytes;   // the whole charge, vertices included
        e.bytes = 0;
        e.vbo = e.ibo = 0; e.count = 0; e.gpu = nullptr;
        e.cacheable = false; e.filled = false;   // decoded again on next sight
        for (VitaDlAddrMemo& m : sVitaDlAddrMemo) if (m.entry == &e) m.list = nullptr;
    }
}
#endif // __vita__
