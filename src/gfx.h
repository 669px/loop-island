// Drawing layer: Direct2D + DirectWrite + WIC.
// Canvas wraps any ID2D1RenderTarget with the handful of primitives the
// island and the control panel need. LayeredSurface renders into a DIB and
// pushes it to a WS_EX_LAYERED window with per-pixel alpha.
#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

template <class T>
void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

struct Color {
    float r = 0, g = 0, b = 0, a = 1;
    constexpr Color() = default;
    constexpr Color(float r_, float g_, float b_, float a_ = 1) : r(r_), g(g_), b(b_), a(a_) {}
    static constexpr Color Hex(unsigned rgb, float a = 1) {
        return Color(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a);
    }
    Color alpha(float na) const { return Color(r, g, b, a * na); }
    Color mix(const Color& o, float t) const {
        return Color(r + (o.r - r) * t, g + (o.g - g) * t, b + (o.b - b) * t, a + (o.a - a) * t);
    }
    D2D1_COLOR_F d2d() const { return D2D1::ColorF(r, g, b, a); }
};

// Palette shared by island and panel: "Lagoon", a cool night theme.
namespace pal {
constexpr Color ink = Color::Hex(0xF2F4F8);
constexpr Color ink2 = Color(226 / 255.f, 232 / 255.f, 240 / 255.f, 0.64f);
constexpr Color ink3 = Color(226 / 255.f, 232 / 255.f, 240 / 255.f, 0.38f);
constexpr Color raised = Color::Hex(0x1A1C22);
constexpr Color raisedHover = Color::Hex(0x262932);
constexpr Color glass = Color(1, 1, 1, 0.065f);
constexpr Color glassHi = Color(1, 1, 1, 0.12f);
constexpr Color hairline = Color(1, 1, 1, 0.08f);
constexpr Color danger = Color::Hex(0xFF5A52);
constexpr Color warn = Color::Hex(0xFFA94D);
constexpr Color ok = Color::Hex(0x3DDC84);
constexpr Color claude = Color::Hex(0xD97757);
Color accent();   // from config
Color onAccent(); // text and icons drawn on an accent fill: dark on light accents like Lagoon
// The expanded card's background, top and bottom of a vertical gradient.
Color cardTop();
Color cardBottom();
}

enum class Align { Left, Center, Right };

struct Gfx {
    ID2D1Factory* d2d = nullptr;
    IDWriteFactory* dw = nullptr;
    IWICImagingFactory* wic = nullptr;
    std::wstring family;          // UI font family that exists on this system
    std::wstring monoFamily;
    bool wine = false;            // Wine's Direct2D turns arc segments into straight lines

    bool Init();
    void Shutdown();
};
extern Gfx g_gfx;

// A decoded image kept as a WIC bitmap; per-target D2D bitmaps are cached.
struct Image {
    IWICBitmapSource* src = nullptr;
    UINT w = 0, h = 0;
    Color average;                // mean color, for tinting the player
    ID2D1Bitmap* bmp = nullptr;
    uint64_t owner = 0;           // Canvas::generation() that `bmp` was made for

    bool Decode(const std::vector<uint8_t>& bytes, UINT maxSide = 256);
    bool LoadResource(int id, const wchar_t* type = L"PNG");
    bool FromPixels(UINT w, UINT h, const std::vector<uint8_t>& pbgra);   // 32bpp premultiplied BGRA
    ID2D1Bitmap* For(ID2D1RenderTarget* rt, uint64_t generation);
    void Reset();
    ~Image() { Reset(); }
};

class Canvas {
public:
    explicit Canvas(ID2D1RenderTarget* rt = nullptr) : rt_(rt) {}
    ~Canvas();
    void Attach(ID2D1RenderTarget* rt);
    ID2D1RenderTarget* rt() const { return rt_; }
    // Changes whenever a new target is attached, even one at a reused address.
    uint64_t generation() const { return gen_; }

    void Clear(Color c = Color(0, 0, 0, 0));
    void FillRect(D2D1_RECT_F r, Color c);
    void FillRound(D2D1_RECT_F r, float radius, Color c);
    void StrokeRound(D2D1_RECT_F r, float radius, Color c, float width = 1);
    void FillCircle(float cx, float cy, float rad, Color c);
    void StrokeCircle(float cx, float cy, float rad, Color c, float width = 1);
    void Line(float x1, float y1, float x2, float y2, Color c, float width = 1, bool round = true);
    void Arc(float cx, float cy, float rad, float startDeg, float sweepDeg, Color c, float width, bool round = true);
    void FillGeometry(ID2D1Geometry* g, Color c);
    void StrokeGeometry(ID2D1Geometry* g, Color c, float width, bool round = true);
    // Notch outline: square top corners flaring out, rounded bottom.
    void FillNotch(D2D1_RECT_F r, float radius, float flare, Color c);
    void FillNotchV(D2D1_RECT_F r, float radius, float flare, Color top, Color bottom);
    void LinearV(D2D1_RECT_F r, float radius, Color top, Color bottom);

    void Text(const std::wstring& s, D2D1_RECT_F r, float size, Color c,
              DWRITE_FONT_WEIGHT w = DWRITE_FONT_WEIGHT_NORMAL, Align a = Align::Left,
              bool vcenter = true, bool mono = false);
    float Measure(const std::wstring& s, float size, DWRITE_FONT_WEIGHT w = DWRITE_FONT_WEIGHT_NORMAL,
                  bool mono = false);
    void DrawImage(Image& img, D2D1_RECT_F r, float radius, float opacity = 1);

    void PushClipRect(D2D1_RECT_F r);
    void PopClipRect();
    void PushClipRound(D2D1_RECT_F r, float radius);
    void PopClip();
    void PushOpacity(float o);
    void PopOpacity();
    void SetTransform(const D2D1_MATRIX_3X2_F& m) { rt_->SetTransform(m); }
    D2D1_MATRIX_3X2_F Transform() const { D2D1_MATRIX_3X2_F m; rt_->GetTransform(&m); return m; }

    ID2D1SolidColorBrush* Brush(Color c);

private:
    IDWriteTextFormat* Format(float size, DWRITE_FONT_WEIGHT w, Align a, bool mono);
    ID2D1Brush* GradientV(D2D1_RECT_F r, Color top, Color bottom);
    void FillNotchBrush(D2D1_RECT_F r, float radius, float flare, ID2D1Brush* b);
    IDWriteTextLayout* Layout(const std::wstring& s, IDWriteTextFormat* f, float w, float h);
    ID2D1RenderTarget* rt_ = nullptr;
    uint64_t gen_ = 0;
    ID2D1SolidColorBrush* brush_ = nullptr;
    ID2D1StrokeStyle* roundCap_ = nullptr;
    IDWriteTypography* tabular_ = nullptr;
    std::map<std::tuple<int, int, int, int>, IDWriteTextFormat*> formats_;
    // Text widths are asked for many times a frame while the island resizes.
    std::map<std::tuple<std::wstring, int, int, int>, float> widths_;
    std::map<std::pair<uint64_t, uint64_t>, ID2D1LinearGradientBrush*> gradients_;
    std::vector<ID2D1Layer*> layers_;
    int layerDepth_ = 0;
};

// Per-pixel alpha window surface.
class LayeredSurface {
public:
    ~LayeredSurface();
    // Size in physical pixels; dpiScale maps DIPs to pixels.
    bool Begin(int w, int h, float dpiScale);
    Canvas& canvas() { return canvas_; }
    void End(HWND hwnd, POINT screenPos);
    int width() const { return w_; }
    int height() const { return h_; }

private:
    void Free();
    HDC dc_ = nullptr;
    HBITMAP bmp_ = nullptr;
    HGDIOBJ old_ = nullptr;
    void* bits_ = nullptr;
    int w_ = 0, h_ = 0;           // size of this frame
    int capW_ = 0, capH_ = 0;     // size of the bitmap, which only grows in steps
    int boundH_ = 0;              // height the render target is bound to
    ID2D1DCRenderTarget* rt_ = nullptr;
    Canvas canvas_;
};

// Icons drawn as vectors (icons.cpp), centred on (cx, cy) in a size x size box.
void DrawIcon(Canvas& c, const std::wstring& name, float cx, float cy, float size, Color color, float stroke = 1.6f);
// Claude's spark, rotated by `angle` degrees.
void DrawSpark(Canvas& c, float cx, float cy, float size, Color color, float angle = 0);
// The Windows mark (four panes).
void DrawWindowsMark(Canvas& c, float cx, float cy, float size, Color color);

// Adds an arc of the circle around `center` from the sink's current point,
// which lies on that circle, to `to`. Under Wine it is made of short lines.
void AddArcTo(ID2D1GeometrySink* sink, D2D1_POINT_2F from, D2D1_POINT_2F to, D2D1_POINT_2F center, float radius,
              bool clockwise);

inline D2D1_RECT_F Rect(float x, float y, float w, float h) { return D2D1::RectF(x, y, x + w, y + h); }
inline bool Hit(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
inline D2D1_RECT_F Inset(D2D1_RECT_F r, float dx, float dy) { return D2D1::RectF(r.left + dx, r.top + dy, r.right - dx, r.bottom - dy); }
