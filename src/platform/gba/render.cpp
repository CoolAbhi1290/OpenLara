#include "common.h"

#if defined(_WIN32)
    uint16 fb[VRAM_WIDTH * FRAME_HEIGHT];
#elif defined(__GBA__)
    uint32 fb = MEM_VRAM;
#elif defined(__TNS__)
    uint16 fb[VRAM_WIDTH * FRAME_HEIGHT];
#elif defined(__DOS__)
    uint16 fb[VRAM_WIDTH * FRAME_HEIGHT];
#endif

#define GUARD_BAND 512

#define PAL_COLOR_TRANSP    0x0000
#define PAL_COLOR_BLACK     0x0421

#ifndef MODE_PAL
uint16 palette[256];      // IWRAM 0.5k
#endif

#if defined(__GBA__)
    #define ALIGNED_LIGHTMAP
#endif

#if defined(MODE4)
    #include "rasterizer_mode4.h"
#elif defined(MODE5)
    #include "rasterizer_mode5.h"
#elif defined(MODE13)
    #include "rasterizer_mode13.h"
#else
    #error no supported video mode set
#endif

extern uint8 lightmap[256 * 32];
extern const Texture* textures;
extern const uint8* tiles;

const uint8* tile;

uint32 gVerticesCount = 0;
int32 gFacesCount = 0; // 1 is reserved as OT terminator

EWRAM_DATA Vertex gVertices[MAX_VERTICES]; // EWRAM 16k
EWRAM_DATA Face gFaces[MAX_FACES];         // EWRAM 5k
EWRAM_DATA Face* otFaces[OT_SIZE] = { 0 };
int32 otMin = OT_SIZE - 1;
int32 otMax = 0;

bool enableAlphaTest;
bool enableClipping;

bool transformBoxRect(const Bounds* box, Rect* rect)
{
    const Matrix &m = matrixGet();

    if ((m[2][3] < VIEW_MIN_F) || (m[2][3] >= VIEW_MAX_F)) {
        return false;
    }

    const vec3i v[8] = {
        vec3i( box->minX, box->minY, box->minZ ),
        vec3i( box->maxX, box->minY, box->minZ ),
        vec3i( box->minX, box->maxY, box->minZ ),
        vec3i( box->maxX, box->maxY, box->minZ ),
        vec3i( box->minX, box->minY, box->maxZ ),
        vec3i( box->maxX, box->minY, box->maxZ ),
        vec3i( box->minX, box->maxY, box->maxZ ),
        vec3i( box->maxX, box->maxY, box->maxZ )
    };

    *rect = Rect( INT_MAX, INT_MAX, INT_MIN, INT_MIN );

    for (int32 i = 0; i < 8; i++) {
        int32 z = DP43(m[2], v[i]);

        if (z < VIEW_MIN_F || z >= VIEW_MAX_F) { // TODO znear clip
            continue;
        }

        int32 x = DP43(m[0], v[i]);
        int32 y = DP43(m[1], v[i]);

        PERSPECTIVE(x, y, z);

        if (x < rect->x0) rect->x0 = x;
        if (x > rect->x1) rect->x1 = x;
        if (y < rect->y0) rect->y0 = y;
        if (y > rect->y1) rect->y1 = y;
    }

    rect->x0 += (FRAME_WIDTH  / 2);
    rect->y0 += (FRAME_HEIGHT / 2);
    rect->x1 += (FRAME_WIDTH  / 2);
    rect->y1 += (FRAME_HEIGHT / 2);

    return true;
}

int32 rectIsVisible(const Rect* rect)
{
    if (rect->x0 > rect->x1 ||
        rect->x0 > viewport.x1  ||
        rect->x1 < viewport.x0  ||
        rect->y0 > viewport.y1  ||
        rect->y1 < viewport.y0) return 0; // not visible

    if (rect->x0 < viewport.x0 ||
        rect->x1 > viewport.x1 ||
        rect->y0 < viewport.y0 ||
        rect->y1 > viewport.y1) return -1; // clipped

    return 1; // fully visible
}

int32 boxIsVisible(const Bounds* box)
{
    Rect rect;
    if (!transformBoxRect(box, &rect))
        return 0; // not visible
    return rectIsVisible(&rect);
}

void transform(int32 vx, int32 vy, int32 vz, int32 vg)
{
    ASSERT(gVerticesCount < MAX_VERTICES);

    Vertex &res = gVertices[gVerticesCount++];

    const Matrix &m = matrixGet();

    int32 z = DP43c(m[2], vx, vy, vz);

    if (z < VIEW_MIN_F || z >= VIEW_MAX_F) // TODO znear clip
    {
        res.clip = 16;
        return;
    }

    int32 x = DP43c(m[0], vx, vy, vz);
    int32 y = DP43c(m[1], vx, vy, vz);

    int32 fogZ = z >> FIXED_SHIFT;
    res.z = fogZ;

    if (fogZ > FOG_MIN)
    {
        vg += (fogZ - FOG_MIN) << FOG_SHIFT;
        if (vg > 8191) {
            vg = 8191;
        }
    }

    res.g = vg >> 8;

    PERSPECTIVE(x, y, z);

    x = X_CLAMP(x, -GUARD_BAND, GUARD_BAND);
    y = X_CLAMP(y, -GUARD_BAND, GUARD_BAND);

    res.x = x + (FRAME_WIDTH  >> 1);
    res.y = y + (FRAME_HEIGHT >> 1);

    res.clip = enableClipping ? classify(res, viewport) : 0;
}

void transformRoomVertex(const RoomVertex* v)
{
    int32 vx = v->x << 10;
    int32 vz = v->z << 10;

    ASSERT(gVerticesCount < MAX_VERTICES);

    Vertex &res = gVertices[gVerticesCount++];

#if defined(MODE4) // TODO for all modes
    if (vz < frustumAABB.minZ || vz > frustumAABB.maxZ ||
        vx < frustumAABB.minX || vx > frustumAABB.maxX)
    {
        res.clip = 16;
        return;
    }
#endif

    int32 vy = v->y << 8;

    const Matrix &m = matrixGet();

    int32 z = DP43c(m[2], vx, vy, vz);

    if (z < VIEW_MIN_F || z >= VIEW_MAX_F) // TODO znear clip
    {
        res.clip = 16;
        return;
    }

    int32 fogZ = z >> FIXED_SHIFT;
    res.z = fogZ;

    int32 vg = v->g << 5;

    if (fogZ > FOG_MIN)
    {
        vg += (fogZ - FOG_MIN) << FOG_SHIFT;
        if (vg > 8191) {
            vg = 8191;
        }
    }

    res.g = vg >> 8;

    int32 x = DP43c(m[0], vx, vy, vz);
    int32 y = DP43c(m[1], vx, vy, vz);

    PERSPECTIVE(x, y, z);

    x = X_CLAMP(x, -GUARD_BAND, GUARD_BAND);
    y = X_CLAMP(y, -GUARD_BAND, GUARD_BAND);

    res.x = x + (FRAME_WIDTH  >> 1);
    res.y = y + (FRAME_HEIGHT >> 1);

    res.clip = classify(res, viewport);
}

void transformRoom(const RoomVertex* vertices, int32 vCount)
{
    for (int32 i = 0; i < vCount; i++)
    {
        transformRoomVertex(vertices);
        vertices++;
    }
}

void transformMesh(const vec3s* vertices, int32 vCount, uint16 intensity)
{
    for (int32 i = 0; i < vCount; i++)
    {
        transform(vertices->x, vertices->y, vertices->z, intensity);
        vertices++;
    }
}

VertexUV* clipPoly(VertexUV* poly, VertexUV* tmp, int32 &pCount) {
    #define LERP(a,b,t)         (b + ((a - b) * t >> 12))
    #define LERP2(a,b,ta,tb)    (b + (((a - b) * ta / tb) >> 12) )

    #define CLIP_AXIS(X, Y, edge, output) {\
        int32 ta = (edge - b->v.X) << 12;\
        int32 tb = (a->v.X - b->v.X);\
        ASSERT(tb != 0);\
        int32 t = ta / tb;\
        VertexUV* v = output + count++;\
        v->v.X = edge;\
        v->v.Y = LERP2(a->v.Y, b->v.Y, ta, tb);\
        v->v.g = LERP(a->v.g, b->v.g, t);\
        v->t.u = LERP(a->t.u, b->t.u, t);\
        v->t.v = LERP(a->t.v, b->t.v, t);\
    }

    #define CLIP_XY(X, Y, X0, X1, input, output) {\
        const VertexUV *a, *b = input + pCount - 1;\
        for (int32 i = 0; i < pCount; i++) {\
            a = b;\
            b = input + i;\
            if (a->v.X < X0) {\
                if (b->v.X < X0) continue;\
                CLIP_AXIS(X, Y, X0, output);\
            } else if (a->v.X > X1) {\
                if (b->v.X > X1) continue;\
                CLIP_AXIS(X, Y, X1, output);\
            }\
            if (b->v.X < X0) {\
                CLIP_AXIS(X, Y, X0, output);\
            } else if (b->v.X > X1) {\
                CLIP_AXIS(X, Y, X1, output);\
            } else {\
                output[count++] = *b;\
            }\
        }\
        if (count < 3) return NULL;\
    }

    int32 count = 0;

    VertexUV *in = poly;
    VertexUV *out = tmp;

    // clip x
    CLIP_XY(x, y, viewport.x0, viewport.x1, in, out);

    pCount = count;
    count = 0;

    // clip y
    CLIP_XY(y, x, viewport.y0, viewport.y1, out, in);
    pCount = count;

    return in;
}

void rasterize(const Face* face, const VertexUV *top)
{
    uint16* pixel = (uint16*)fb + top->v.y * VRAM_WIDTH;
#ifdef DEBUG_OVERDRAW
    rasterize_overdraw(pixel, top, top);
#else
    #if 0
        rasterizeG(pixel, top, top, (face->flags & FACE_TEXTURE) + 10);
        //rasterize_dummy(pixel, top, top);
        //rasterizeGT(pixel, top, top);
    #else
        if (face->flags & FACE_COLORED) {
            if (face->flags & FACE_FLAT) {
                if (face->flags & FACE_SHADOW) {
                    rasterizeS(pixel, top, top);
                } else {
                    rasterizeF(pixel, top, top, face->flags & FACE_TEXTURE);
                }
            } else {
                rasterizeG(pixel, top, top, face->flags & FACE_TEXTURE);
            }
        } else {
            if (enableAlphaTest) {
                if (face->flags & FACE_FLAT) {
                    rasterizeFTA(pixel, top, top);
                } else {
                    rasterizeGTA(pixel, top, top);
                }
            } else {
                #if 0
                    rasterizeFT(pixel, top, top);
                #else
                if (face->flags & FACE_FLAT) {
                    rasterizeFT(pixel, top, top);
                } else {
                    rasterizeGT(pixel, top, top);
                }
                #endif
            }
        }
    #endif
#endif
}

void drawTriangle(const Face* face, VertexUV *v)
{
    VertexUV *v1 = v + 0,
             *v2 = v + 1,
             *v3 = v + 2;

    v1->next = v2;
    v2->next = v3;
    v3->next = v1;
    v1->prev = v3;
    v2->prev = v1;
    v3->prev = v2;

    const VertexUV* top = v1;
    if (v1->v.y < v2->v.y) {
        if (v1->v.y < v3->v.y) {
            top = v1;
        } else {
            top = v3;
        }
    } else {
        if (v2->v.y < v3->v.y) {
            top = v2;
        } else {
            top = v3;
        }
    }

    rasterize(face, top);
}

void drawQuad(const Face* face, VertexUV *v)
{
    VertexUV *v1 = v + 0,
             *v2 = v + 1,
             *v3 = v + 2,
             *v4 = v + 3;

    v1->next = v2;
    v2->next = v3;
    v3->next = v4;
    v4->next = v1;
    v1->prev = v4;
    v2->prev = v1;
    v3->prev = v2;
    v4->prev = v3;

    VertexUV* top;

    if (v1->v.y < v2->v.y) {
        if (v1->v.y < v3->v.y) {
            top = (v1->v.y < v4->v.y) ? v1 : v4;
        } else {
            top = (v3->v.y < v4->v.y) ? v3 : v4;
        }
    } else {
        if (v2->v.y < v3->v.y) {
            top = (v2->v.y < v4->v.y) ? v2 : v4;
        } else {
            top = (v3->v.y < v4->v.y) ? v3 : v4;
        }
    }

    rasterize(face, top);
}

void drawPoly(Face* face, VertexUV* v)
{
    VertexUV tmp[16];

    int32 count = (face->flags & FACE_TRIANGLE) ? 3 : 4;

    v = clipPoly(v, tmp, count);

    if (!v) return;

    if (count <= 4)
    {
        face->indices[0] = 0;
        face->indices[1] = 1;
        face->indices[2] = 2;
        face->indices[3] = 3;

        if (count == 3) {

            if (v[0].v.y == v[1].v.y &&
                v[0].v.y == v[2].v.y)
                return;

            drawTriangle(face, v);
        } else {

            if (v[0].v.y == v[1].v.y &&
                v[0].v.y == v[2].v.y &&
                v[0].v.y == v[3].v.y)
                return;

            drawQuad(face, v);
        }
        return;
    }

    VertexUV* top = v;
    top->next = v + 1;
    top->prev = v + count - 1;

    bool skip = true;

    for (int32 i = 1; i < count; i++)
    {
        VertexUV *p = v + i;

        p->next = v + (i + 1) % count;
        p->prev = v + (i - 1 + count) % count;

        if (p->v.y != top->v.y)
        {
            if (p->v.y < top->v.y) {
                top = p;
            }
            skip = false;
        }
    }

    if (skip) {
        return; // zero height poly
    }

    rasterize(face, top);
}

void drawGlyph(const Sprite *sprite, int32 x, int32 y)
{
    int32 w = sprite->r - sprite->l;
    int32 h = sprite->b - sprite->t;

    w = (w >> 1) << 1; // make it even

    int32 ix = x + sprite->l;
    int32 iy = y + sprite->t;

#if defined(MODE_PAL)
    uint16* pixel = (uint16*)fb + iy * VRAM_WIDTH + (ix >> 1);
#elif defined(ROTATE90_MODE)
    uint16* pixel = (uint16*)fb + iy;
#else
    uint16* pixel = (uint16*)fb + iy * VRAM_WIDTH + ix;
#endif

    const uint8* glyphData = tiles + (sprite->tile << 16) + 256 * sprite->v + sprite->u;

    while (h--)
    {
    #if defined(MODE_PAL)
        const uint8* p = glyphData;

        for (int32 i = 0; i < (w / 2); i++)
        {
            if (p[0] || p[1])
            {
                uint16 d = pixel[i];

                if (p[0]) d = (d & 0xFF00) | p[0];
                if (p[1]) d = (d & 0x00FF) | (p[1] << 8);

                pixel[i] = d;
            }

            p += 2;
        }
    #else
        for (int32 i = 0; i < w; i++)
        {
            if (glyphData[i] == 0) continue;
        #ifdef ROTATE90_MODE
            pixel[(FRAME_HEIGHT - (ix + i) - 1) * FRAME_WIDTH] = palette[glyphData[i]];
        #else
            pixel[i] = palette[glyphData[i]];
        #endif
        }
    #endif

    #ifdef ROTATE90_MODE
        pixel += 1;
    #else
        pixel += VRAM_WIDTH;
    #endif

        glyphData += 256;
    }
}

X_INLINE void faceAddToOTable(Face* face, int32 depth)
{
    ASSERT(depth < OT_SIZE);
    face->next = otFaces[depth];
    otFaces[depth] = face;

    if (depth < otMin) otMin = depth;
    if (depth > otMax) otMax = depth;
}

void faceAddQuad(uint32 flags, const Index* indices, int32 startVertex)
{
    ASSERT(gFacesCount < MAX_FACES);

    const Vertex* v = gVertices + startVertex;
    const Vertex* v1 = v + indices[0];
    const Vertex* v2 = v + indices[1];
    const Vertex* v3 = v + indices[2];
    const Vertex* v4 = v + indices[3];

    if (v1->clip == 16 || v2->clip == 16 || v3->clip == 16 || v4->clip == 16)
        return;

    if (enableClipping)
    {
        if (v1->clip & v2->clip & v3->clip & v4->clip)
            return;

        if (v1->clip | v2->clip | v3->clip | v4->clip) {
            flags |= FACE_CLIPPED;
        }
    }

    if (v1->g == v2->g && v1->g == v3->g && v1->g == v4->g) {
        flags |= FACE_FLAT;
    }

    int32 depth = X_MAX(v1->z, X_MAX(v2->z, X_MAX(v3->z, v4->z))) >> OT_SHIFT;

    // z-bias hack for the shadow plane
    if (flags & FACE_SHADOW) {
        depth = X_MAX(0, depth - 8);
    }

    Face *f = gFaces + gFacesCount++;
    faceAddToOTable(f, depth);

    f->flags      = uint16(flags);
    f->indices[0] = startVertex + indices[0];
    f->indices[1] = startVertex + indices[1];
    f->indices[2] = startVertex + indices[2];
    f->indices[3] = startVertex + indices[3];
}

void faceAddTriangle(uint32 flags, const Index* indices, int32 startVertex)
{
    ASSERT(gFacesCount < MAX_FACES);

    const Vertex* v = gVertices + startVertex;
    const Vertex* v1 = v + indices[0];
    const Vertex* v2 = v + indices[1];
    const Vertex* v3 = v + indices[2];

    if (v1->clip == 16 || v2->clip == 16 || v3->clip == 16)
        return;

    if (enableClipping)
    {
        if (v1->clip & v2->clip & v3->clip)
            return;

        if (v1->clip | v2->clip | v3->clip) {
            flags |= FACE_CLIPPED;
        }
    }

    if (v1->g == v2->g && v1->g == v3->g) {
        flags |= FACE_FLAT;
    }

    int32 depth = X_MAX(v1->z, X_MAX(v2->z, v3->z)) >> OT_SHIFT;

    Face *f = gFaces + gFacesCount++;
    faceAddToOTable(f, depth);

    f->flags      = uint16(flags | FACE_TRIANGLE);
    f->indices[0] = startVertex + indices[0];
    f->indices[1] = startVertex + indices[1];
    f->indices[2] = startVertex + indices[2];
}

void faceAddRoom(const Quad* quads, int32 qCount, const Triangle* triangles, int32 tCount, int32 startVertex)
{
    for (uint16 i = 0; i < qCount; i++) {
        faceAddQuad(quads[i].flags, quads[i].indices, startVertex);
    }

    for (uint16 i = 0; i < tCount; i++) {
        faceAddTriangle(triangles[i].flags, triangles[i].indices, startVertex);
    }
}

void faceAddMesh(const Quad* rFaces, const Quad* crFaces, const Triangle* tFaces, const Triangle* ctFaces, int32 rCount, int32 crCount, int32 tCount, int32 ctCount, int32 startVertex)
{
    for (int i = 0; i < rCount; i++) {
        faceAddQuad(rFaces[i].flags, rFaces[i].indices, startVertex);
    }

    for (int i = 0; i < crCount; i++) {
        faceAddQuad(crFaces[i].flags | FACE_COLORED, crFaces[i].indices, startVertex);
    }

    for (int i = 0; i < tCount; i++) {
        faceAddTriangle(tFaces[i].flags, tFaces[i].indices, startVertex);
    }

    for (int i = 0; i < ctCount; i++) {
        faceAddTriangle(ctFaces[i].flags | FACE_COLORED, ctFaces[i].indices, startVertex);
    }
}

#ifdef DEBUG_FACES
    int32 gFacesCountMax, gVerticesCountMax;
#endif

void flush()
{
    if (gFacesCount)
    {
        PROFILE_START();
        for (int32 i = otMax; i >= otMin; i--)
        {
            if (!otFaces[i]) continue;

            Face *face = otFaces[i];
            otFaces[i] = NULL;

            do {
                VertexUV v[16];

                uint32 flags = face->flags;

                if (!(flags & FACE_COLORED))
                {
                    const Texture &tex = textures[face->flags & FACE_TEXTURE];
                    tile = tiles + (tex.tile << 16);

                    v[0].t.uv = tex.uv0;
                    v[1].t.uv = tex.uv1;
                    v[2].t.uv = tex.uv2;
                    v[3].t.uv = tex.uv3;
                    enableAlphaTest = (tex.attribute == 1);
                }

                v[0].v = gVertices[face->indices[0]];
                v[1].v = gVertices[face->indices[1]];
                v[2].v = gVertices[face->indices[2]];
                if (!(flags & FACE_TRIANGLE)) {
                    v[3].v = gVertices[face->indices[3]];
                }

                if (flags & FACE_CLIPPED) {
                    drawPoly(face, v);
                } else {
                    if (flags & FACE_TRIANGLE) {
                        drawTriangle(face, v);
                    } else {
                        drawQuad(face, v);
                    }
                };

                face = face->next;
            } while (face);
        }

        PROFILE_STOP(dbg_flush);

        otMin = OT_SIZE - 1;
        otMax = 0;
    }

#ifdef DEBUG_FACES
    if ((gFacesCount > gFacesCountMax) || (gVerticesCount > gVerticesCountMax))
    {
        if (gFacesCount > gFacesCountMax) gFacesCountMax = gFacesCount;
        if (gVerticesCount > gVerticesCountMax) gVerticesCountMax = gVerticesCount;
        printf("v: %d f: %d\n", gVerticesCountMax, gFacesCountMax);
    }
#endif

#ifdef PROFILE
    dbg_vert_count += gVerticesCount;
    dbg_poly_count += gFacesCount;
#endif

    gVerticesCount = 0;
    gFacesCount = 0;
}

void clear()
{
    dmaFill((void*)fb, 0, VRAM_WIDTH * FRAME_HEIGHT * 2);
}
