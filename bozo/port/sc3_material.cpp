// A rewritten builder's GS draw in the Renderer's terms: the registers it computed become a Material, its sprites and lines
// triangles.
#include "sc3_draw.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace sc3::draw
{
    using bozo::render::Material;
    using bozo::render::Vertex;

    const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    GsPrim decodePrim(uint32_t bits)
    {
        GsPrim p;
        p.type = static_cast<GSPrimType>(bits & 7u);
        p.iip = (bits >> 3) & 1u;
        p.tme = (bits >> 4) & 1u;
        p.fge = (bits >> 5) & 1u;
        p.abe = (bits >> 6) & 1u;
        p.aa1 = (bits >> 7) & 1u;
        p.fst = (bits >> 8) & 1u;
        p.ctxt = (bits >> 9) & 1u;
        return p;
    }

    GsTex0 decodeTex0(uint64_t v)
    {
        GsTex0 t;
        t.tbp = static_cast<uint32_t>(v & 0x3FFFu);
        t.tbw = static_cast<uint32_t>((v >> 14) & 0x3Fu);
        t.psm = static_cast<uint32_t>((v >> 20) & 0x3Fu);
        t.tw = static_cast<uint32_t>((v >> 26) & 0xFu);
        t.th = static_cast<uint32_t>((v >> 30) & 0xFu);
        t.tcc = static_cast<uint32_t>((v >> 34) & 1u);
        t.tfx = static_cast<uint32_t>((v >> 35) & 3u);
        t.cbp = static_cast<uint32_t>((v >> 37) & 0x3FFFu);
        t.cpsm = static_cast<uint32_t>((v >> 51) & 0xFu);
        t.csm = static_cast<uint32_t>((v >> 55) & 1u);
        t.csa = static_cast<uint32_t>((v >> 56) & 0x1Fu);
        t.cld = static_cast<uint32_t>((v >> 61) & 7u);
        return t;
    }

    namespace
    {
        struct Mapping
        {
            float scaleU = 1.0f, scaleV = 1.0f;
            bool decal = false;
        };

        Vertex toVertex(const GsVertex &g, const Mapping &map, uint32_t rgba, uint32_t z)
        {
            Vertex v;
            v.x = g.x;
            v.y = g.y;
            v.z = static_cast<float>(z);
            const float q = g.q != 0.0f ? g.q : 1.0f;
            v.u = g.s / q * map.scaleU;
            v.v = g.t / q * map.scaleV;
            v.q = q > 0.0f ? q : 1.0f;
            v.rgba = map.decal ? 0x80808080u : rgba;
            v.fog = g.fog;
            return v;
        }

        void corners(std::vector<Vertex> &out, Vertex a, Vertex b, Vertex c, Vertex d)
        {
            out.push_back(a);
            out.push_back(b);
            out.push_back(c);
            out.push_back(b);
            out.push_back(d);
            out.push_back(c);
        }

        void sprites(const GsVertex *in, uint32_t count, const Mapping &map, std::vector<Vertex> &out)
        {
            for (uint32_t i = 0; i + 1u < count; i += 2u)
            {
                const GsVertex &p = in[i], &q = in[i + 1u];
                const uint32_t rgba = q.rgba, z = q.z;
                Vertex a = toVertex(p, map, rgba, z), d = toVertex(q, map, rgba, z);
                Vertex b = a, c = a;
                b.x = d.x;
                b.u = d.u;
                c.y = d.y;
                c.v = d.v;
                corners(out, a, b, c, d);
            }
        }

        void line(const GsVertex &p, const GsVertex &q, bool shaded, const Mapping &map, std::vector<Vertex> &out)
        {
            Vertex a = toVertex(p, map, shaded ? p.rgba : q.rgba, p.z), d = toVertex(q, map, q.rgba, q.z);
            const float dx = d.x - a.x, dy = d.y - a.y;
            const bool wide = std::fabs(dx) >= std::fabs(dy);
            const float ox = wide ? 0.0f : 0.5f, oy = wide ? 0.5f : 0.0f;
            Vertex a0 = a, a1 = a, d0 = d, d1 = d;
            a0.x -= ox;
            a0.y -= oy;
            a1.x += ox;
            a1.y += oy;
            d0.x -= ox;
            d0.y -= oy;
            d1.x += ox;
            d1.y += oy;
            corners(out, a0, a1, d0, d1);
        }

        void triangle(const GsVertex &a, const GsVertex &b, const GsVertex &c, bool shaded, const Mapping &map, std::vector<Vertex> &out)
        {
            out.push_back(toVertex(a, map, shaded ? a.rgba : c.rgba, a.z));
            out.push_back(toVertex(b, map, shaded ? b.rgba : c.rgba, b.z));
            out.push_back(toVertex(c, map, c.rgba, c.z));
        }

        // TEST as the Renderer has it. AFAIL RGB_ONLY with ATST NEVER is a write mask (0x3001): RGB written, not alpha or Z.
        void fromTest(uint64_t test, Material &m)
        {
            const uint32_t ate = static_cast<uint32_t>(test & 1u), atst = static_cast<uint32_t>((test >> 1) & 7u);
            const uint32_t aref = static_cast<uint32_t>((test >> 4) & 0xFFu), afail = static_cast<uint32_t>((test >> 12) & 3u);
            if (ate != 0u && atst == 0u && afail == 3u)
            {
                m.colourMask = 7u;
                m.depthWrite = false;
            }
            else if (ate != 0u && atst != 1u && afail == 0u)
            {
                m.alphaTest = true;
                m.alphaFunc = static_cast<Material::AlphaFunc>(atst);
                m.alphaRef = static_cast<uint8_t>(aref);
            }
            if ((test >> 16) & 1u)
            {
                const uint32_t ztst = static_cast<uint32_t>((test >> 17) & 3u);
                m.depthTest = ztst >= 2u;
                m.depthGreater = ztst == 3u;
            }
        }
    }

    void toRenderer(const GsMaterial &gs, const GsVertex *in, uint32_t count, Material &m, std::vector<Vertex> &out)
    {
        Mapping map;
        const std::shared_ptr<const bozo::render::Texture> &texture = gs.texture;
        m.textured = gs.prim.tme && texture;
        m.blend = gs.prim.abe ? Material::Blend::Alpha : Material::Blend::Opaque;
        m.alphaBlend = gs.prim.abe;
        if (gs.prim.abe)
        {
            m.gsBlend = true;
            m.blendA = static_cast<uint8_t>(gs.alpha & 3u);
            m.blendB = static_cast<uint8_t>((gs.alpha >> 2) & 3u);
            m.blendC = static_cast<uint8_t>((gs.alpha >> 4) & 3u);
            m.blendD = static_cast<uint8_t>((gs.alpha >> 6) & 3u);
            m.blendFix = static_cast<uint8_t>(gs.alpha >> 32);
        }
        m.useQ = true;
        m.fog = gs.prim.fge;
        m.depthTest = false;
        m.depthWrite = false;
        if (!gs.testFromTarget)
            fromTest(gs.test, m);
        if (m.textured)
        {
            m.tex0 = texture->key;
            map.decal = gs.tex0.tfx == 1u;
            if (texture->width != 0u && texture->height != 0u)
            {
                map.scaleU = static_cast<float>(1u << std::min(gs.tex0.tw, 10u)) / static_cast<float>(texture->width);
                map.scaleV = static_cast<float>(1u << std::min(gs.tex0.th, 10u)) / static_cast<float>(texture->height);
            }
            m.nearest = ((gs.tex1 >> 5) & 1u) == 0u;
            m.clampU = (gs.clamp & 3u) != 0u;
            m.clampV = ((gs.clamp >> 2) & 3u) != 0u;
        }
        out.clear();
        const bool shaded = gs.prim.iip;
        switch (gs.prim.type)
        {
        case GS_PRIM_SPRITE:
            sprites(in, count, map, out);
            break;
        case GS_PRIM_LINE:
            for (uint32_t i = 0; i + 1u < count; i += 2u)
                line(in[i], in[i + 1u], shaded, map, out);
            break;
        case GS_PRIM_LINESTRIP:
            for (uint32_t i = 0; i + 1u < count; ++i)
                line(in[i], in[i + 1u], shaded, map, out);
            break;
        case GS_PRIM_TRISTRIP:
            for (uint32_t i = 0; i + 2u < count; ++i)
                triangle(in[i], in[i + 1u], in[i + 2u], shaded, map, out);
            break;
        case GS_PRIM_TRIFAN:
            for (uint32_t i = 1; i + 1u < count; ++i)
                triangle(in[0], in[i], in[i + 1u], shaded, map, out);
            break;
        case GS_PRIM_POINT:
            break;
        default:
            for (uint32_t i = 0; i + 2u < count; i += 3u)
                triangle(in[i], in[i + 1u], in[i + 2u], shaded, map, out);
            break;
        }
    }

    namespace
    {
        struct KeyedBatch
        {
            Material material;
            std::vector<Vertex> vertices;
            std::shared_ptr<const bozo::render::Texture> texture;
        };
        std::map<uint64_t, std::vector<KeyedBatch>> g_keyed;
    }

    void drawKeyed(uint64_t key, const GsMaterial &material, const GsVertex *vertices, uint32_t count)
    {
        KeyedBatch batch;
        toRenderer(material, vertices, count, batch.material, batch.vertices);
        batch.texture = material.texture;
        g_keyed[key].push_back(std::move(batch));
    }

    void discard(uint64_t key) { g_keyed.erase(key); }

    ot::Draw takeKeyed(uint64_t key)
    {
        const auto it = g_keyed.find(key);
        if (it == g_keyed.end())
            return {};
        auto batches = std::make_shared<std::vector<KeyedBatch>>(std::move(it->second));
        g_keyed.erase(it);
        return [batches](bozo::render::Renderer &renderer)
        {
            for (const KeyedBatch &b : *batches)
                if (!b.vertices.empty())
                {
                    if (b.texture)
                        renderer.draw(b.material, kIdentity, b.vertices.data(), static_cast<uint32_t>(b.vertices.size()), b.texture);
                    else
                        renderer.draw(b.material, kIdentity, b.vertices.data(), static_cast<uint32_t>(b.vertices.size()));
                }
        };
    }
}

namespace sc3::fp
{
    int32_t ps2FpuCvtWS(float v)
    {
        uint32_t f = 0;
        return static_cast<int32_t>(bozo::ps2f::toInt(bits(v)));
    }
}
