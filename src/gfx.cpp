#include "gfx.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "config.h"

Gfx g_gfx;

Color pal::accent() { return Color::Hex(AccentRGB(g_config.accent)); }

Color pal::onAccent() {
    Color a = accent();
    return 0.2126f * a.r + 0.7152f * a.g + 0.0722f * a.b > 0.55f ? Color::Hex(0x0B1A1C) : ink;
}

Color pal::cardTop() {
    if (g_config.cardStyle == L"graphite") return Color::Hex(0x24262C);
    if (g_config.cardStyle == L"black") return Color(0, 0, 0, 1);
    return Color::Hex(0x151922);
}
Color pal::cardBottom() {
    if (g_config.cardStyle == L"graphite") return Color::Hex(0x1A1C21);
    if (g_config.cardStyle == L"black") return Color(0, 0, 0, 1);
    return Color::Hex(0x090B10);
}

static bool fontExists(IDWriteFontCollection* fc, const wchar_t* name) {
    UINT32 index;
    BOOL exists = FALSE;
    return SUCCEEDED(fc->FindFamilyName(name, &index, &exists)) && exists;
}

bool Gfx::Init() {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(&dw))))
        return false;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))))
        return false;
    wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
    IDWriteFontCollection* fc = nullptr;
    dw->GetSystemFontCollection(&fc, FALSE);
    family = L"Segoe UI";
    monoFamily = L"Consolas";
    if (fc) {
        for (auto name : {L"Segoe UI Variable Text", L"Segoe UI", L"Inter", L"Noto Sans", L"Tahoma"})
            if (fontExists(fc, name)) { family = name; break; }
        for (auto name : {L"Cascadia Mono", L"Consolas", L"Noto Sans Mono", L"Courier New"})
            if (fontExists(fc, name)) { monoFamily = name; break; }
        fc->Release();
    }
    return true;
}

void AddArcTo(ID2D1GeometrySink* sink, D2D1_POINT_2F from, D2D1_POINT_2F to, D2D1_POINT_2F center, float radius,
              bool clockwise) {
    constexpr float kTau = 6.2831853f;
    float a0 = std::atan2(from.y - center.y, from.x - center.x);
    float a1 = std::atan2(to.y - center.y, to.x - center.x);
    float sweep = a1 - a0;   // y points down, so clockwise is a growing angle
    if (clockwise) { while (sweep <= 0) sweep += kTau; }
    else { while (sweep >= 0) sweep -= kTau; }
    if (!g_gfx.wine) {
        sink->AddArc(D2D1::ArcSegment(to, D2D1::SizeF(radius, radius), 0,
                                      clockwise ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                                      std::fabs(sweep) > kTau / 2 ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
        return;
    }
    int n = std::clamp((int)std::ceil(std::fabs(sweep) * std::sqrt(std::max(radius, 1.f)) * 2), 3, 128);
    for (int i = 1; i < n; i++) {
        float a = a0 + sweep * i / n;
        sink->AddLine(D2D1::Point2F(center.x + radius * std::cos(a), center.y + radius * std::sin(a)));
    }
    sink->AddLine(to);
}

void Gfx::Shutdown() {
    SafeRelease(wic);
    SafeRelease(dw);
    SafeRelease(d2d);
}

// ------------------------------------------------------------ Image
void Image::Reset() {
    SafeRelease(bmp);
    SafeRelease(src);
    owner = 0;
    w = h = 0;
}

static bool finishDecode(Image& img, IWICBitmapFrameDecode* frame, UINT maxSide) {
    UINT fw = 0, fh = 0;
    frame->GetSize(&fw, &fh);
    if (!fw || !fh) return false;
    IWICBitmapSource* source = frame;
    source->AddRef();
    if (std::max(fw, fh) > maxSide) {
        IWICBitmapScaler* scaler = nullptr;
        float s = (float)maxSide / (float)std::max(fw, fh);
        UINT nw = std::max(1u, (UINT)(fw * s)), nh = std::max(1u, (UINT)(fh * s));
        if (SUCCEEDED(g_gfx.wic->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(source, nw, nh, WICBitmapInterpolationModeFant))) {
            source->Release();
            source = scaler;
        } else SafeRelease(scaler);
    }
    IWICFormatConverter* conv = nullptr;
    if (FAILED(g_gfx.wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(source, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeMedianCut))) {
        SafeRelease(conv);
        source->Release();
        return false;
    }
    source->Release();
    // Keep the pixels in memory so the decoder and stream can go away.
    IWICBitmap* mem = nullptr;
    HRESULT hr = g_gfx.wic->CreateBitmapFromSource(conv, WICBitmapCacheOnLoad, &mem);
    conv->Release();
    if (FAILED(hr)) return false;
    img.src = mem;
    mem->GetSize(&img.w, &img.h);

    // average color, skipping near-black and near-white pixels
    std::vector<uint8_t> px((size_t)img.w * img.h * 4);
    WICRect all{0, 0, (INT)img.w, (INT)img.h};
    if (SUCCEEDED(mem->CopyPixels(&all, img.w * 4, (UINT)px.size(), px.data()))) {
        double r = 0, g = 0, b = 0, n = 0;
        for (size_t i = 0; i + 3 < px.size(); i += 16) {
            double a = px[i + 3] / 255.0;
            if (a < 0.5) continue;
            double pb = px[i] / a, pg = px[i + 1] / a, pr = px[i + 2] / a;
            double mx = std::max({pr, pg, pb}), mn = std::min({pr, pg, pb});
            double sat = mx > 0 ? (mx - mn) / mx : 0;
            double wgt = 0.25 + sat * 2.0;   // favour colourful pixels
            if (mx < 25 || mn > 235) wgt *= 0.15;
            r += pr * wgt; g += pg * wgt; b += pb * wgt; n += wgt;
        }
        if (n > 0) img.average = Color((float)(r / n / 255), (float)(g / n / 255), (float)(b / n / 255));
    }
    return true;
}

bool Image::Decode(const std::vector<uint8_t>& bytes, UINT maxSide) {
    Reset();
    if (bytes.empty() || !g_gfx.wic) return false;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    bool ok = false;
    if (SUCCEEDED(g_gfx.wic->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), (DWORD)bytes.size())) &&
        SUCCEEDED(g_gfx.wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)))
        ok = finishDecode(*this, frame, maxSide);
    SafeRelease(frame);
    SafeRelease(dec);
    SafeRelease(stream);
    return ok;
}

bool Image::LoadResource(int id, const wchar_t* type) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), type);
    if (!res) return false;
    HGLOBAL mem = ::LoadResource(nullptr, res);
    DWORD size = SizeofResource(nullptr, res);
    auto* data = static_cast<const uint8_t*>(LockResource(mem));
    if (!data || !size) return false;
    return Decode(std::vector<uint8_t>(data, data + size), 512);
}

bool Image::FromPixels(UINT pw, UINT ph, const std::vector<uint8_t>& pbgra) {
    Reset();
    if (!g_gfx.wic || !pw || !ph || pbgra.size() < (size_t)pw * ph * 4) return false;
    IWICBitmap* b = nullptr;
    if (FAILED(g_gfx.wic->CreateBitmapFromMemory(pw, ph, GUID_WICPixelFormat32bppPBGRA, pw * 4, (UINT)pbgra.size(),
                                                 const_cast<BYTE*>(pbgra.data()), &b)))
        return false;
    src = b;
    w = pw;
    h = ph;
    return true;
}

ID2D1Bitmap* Image::For(ID2D1RenderTarget* rt, uint64_t generation) {
    if (!src) return nullptr;
    if (bmp && owner == generation) return bmp;
    SafeRelease(bmp);
    owner = generation;
    rt->CreateBitmapFromWicBitmap(src, nullptr, &bmp);
    return bmp;
}

// ------------------------------------------------------------ Canvas
Canvas::~Canvas() { Attach(nullptr); }

void Canvas::Attach(ID2D1RenderTarget* rt) {
    if (rt == rt_) return;
    SafeRelease(brush_);
    SafeRelease(roundCap_);
    SafeRelease(tabular_);
    for (auto& kv : formats_) kv.second->Release();
    formats_.clear();
    for (auto& kv : gradients_) kv.second->Release();
    gradients_.clear();
    widths_.clear();
    for (auto* l : layers_) l->Release();
    layers_.clear();
    rt_ = rt;
    static uint64_t counter = 0;
    gen_ = rt ? ++counter : 0;
    if (rt_) {
        rt_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush_);
        D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
        g_gfx.d2d->CreateStrokeStyle(sp, nullptr, 0, &roundCap_);
        // tabular figures so clocks and timers don't wobble
        if (SUCCEEDED(g_gfx.dw->CreateTypography(&tabular_)))
            tabular_->AddFontFeature({DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1});
    }
}

ID2D1SolidColorBrush* Canvas::Brush(Color c) {
    brush_->SetColor(c.d2d());
    return brush_;
}

void Canvas::Clear(Color c) { rt_->Clear(c.d2d()); }
void Canvas::FillRect(D2D1_RECT_F r, Color c) { rt_->FillRectangle(r, Brush(c)); }
void Canvas::FillRound(D2D1_RECT_F r, float radius, Color c) {
    radius = std::min(radius, std::min(r.right - r.left, r.bottom - r.top) / 2);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), Brush(c));
}
void Canvas::StrokeRound(D2D1_RECT_F r, float radius, Color c, float width) {
    radius = std::min(radius, std::min(r.right - r.left, r.bottom - r.top) / 2);
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), Brush(c), width);
}
void Canvas::FillCircle(float cx, float cy, float rad, Color c) {
    rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad), Brush(c));
}
void Canvas::StrokeCircle(float cx, float cy, float rad, Color c, float width) {
    rt_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad), Brush(c), width);
}
void Canvas::Line(float x1, float y1, float x2, float y2, Color c, float width, bool round) {
    rt_->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), Brush(c), width, round ? roundCap_ : nullptr);
}

void Canvas::Arc(float cx, float cy, float rad, float startDeg, float sweepDeg, Color c, float width, bool round) {
    if (std::fabs(sweepDeg) < 0.1f) return;
    if (std::fabs(sweepDeg) >= 359.9f) { StrokeCircle(cx, cy, rad, c, width); return; }
    auto pt = [&](float deg) {
        float a = (deg - 90) * 3.14159265f / 180.f;
        return D2D1::Point2F(cx + rad * std::cos(a), cy + rad * std::sin(a));
    };
    ID2D1PathGeometry* g = nullptr;
    ID2D1GeometrySink* s = nullptr;
    if (FAILED(g_gfx.d2d->CreatePathGeometry(&g)) || FAILED(g->Open(&s))) { SafeRelease(g); return; }
    s->BeginFigure(pt(startDeg), D2D1_FIGURE_BEGIN_HOLLOW);
    AddArcTo(s, pt(startDeg), pt(startDeg + sweepDeg), D2D1::Point2F(cx, cy), rad, sweepDeg > 0);
    s->EndFigure(D2D1_FIGURE_END_OPEN);
    s->Close();
    rt_->DrawGeometry(g, Brush(c), width, round ? roundCap_ : nullptr);
    s->Release();
    g->Release();
}

void Canvas::FillGeometry(ID2D1Geometry* g, Color c) { rt_->FillGeometry(g, Brush(c)); }
void Canvas::StrokeGeometry(ID2D1Geometry* g, Color c, float width, bool round) {
    rt_->DrawGeometry(g, Brush(c), width, round ? roundCap_ : nullptr);
}

void Canvas::FillNotchBrush(D2D1_RECT_F r, float radius, float flare, ID2D1Brush* b) {
    float w = r.right - r.left, h = r.bottom - r.top;
    radius = std::min({radius, w / 2, h});
    ID2D1PathGeometry* g = nullptr;
    ID2D1GeometrySink* s = nullptr;
    if (FAILED(g_gfx.d2d->CreatePathGeometry(&g)) || FAILED(g->Open(&s))) { SafeRelease(g); return; }
    float L = r.left, R = r.right, T = r.top, B = r.bottom;
    auto P = [](float x, float y) { return D2D1::Point2F(x, y); };
    s->BeginFigure(P(L - flare, T), D2D1_FIGURE_BEGIN_FILLED);
    // concave flare into the top edge, like the MacBook notch
    AddArcTo(s, P(L - flare, T), P(L, T + flare), P(L - flare, T + flare), flare, true);
    s->AddLine(P(L, B - radius));
    AddArcTo(s, P(L, B - radius), P(L + radius, B), P(L + radius, B - radius), radius, false);
    s->AddLine(P(R - radius, B));
    AddArcTo(s, P(R - radius, B), P(R, B - radius), P(R - radius, B - radius), radius, false);
    s->AddLine(P(R, T + flare));
    AddArcTo(s, P(R, T + flare), P(R + flare, T), P(R + flare, T + flare), flare, true);
    s->EndFigure(D2D1_FIGURE_END_CLOSED);
    s->Close();
    rt_->FillGeometry(g, b);
    s->Release();
    g->Release();
}

void Canvas::FillNotch(D2D1_RECT_F r, float radius, float flare, Color c) { FillNotchBrush(r, radius, flare, Brush(c)); }
void Canvas::FillNotchV(D2D1_RECT_F r, float radius, float flare, Color top, Color bottom) {
    if (ID2D1Brush* b = GradientV(r, top, bottom)) FillNotchBrush(r, radius, flare, b);
}

static uint64_t packColor(Color c) {
    auto q = [](float v) { return (uint64_t)(std::clamp(v, 0.f, 1.f) * 65535.f + 0.5f); };
    return q(c.r) << 48 | q(c.g) << 32 | q(c.b) << 16 | q(c.a);
}

// One brush per pair of colours, moved to each rectangle it paints.
ID2D1Brush* Canvas::GradientV(D2D1_RECT_F r, Color top, Color bottom) {
    auto key = std::make_pair(packColor(top), packColor(bottom));
    auto it = gradients_.find(key);
    if (it == gradients_.end()) {
        if (gradients_.size() > 32) {
            for (auto& kv : gradients_) kv.second->Release();
            gradients_.clear();
        }
        D2D1_GRADIENT_STOP stops[2] = {{0, top.d2d()}, {1, bottom.d2d()}};
        ID2D1GradientStopCollection* col = nullptr;
        ID2D1LinearGradientBrush* br = nullptr;
        if (SUCCEEDED(rt_->CreateGradientStopCollection(stops, 2, &col)))
            rt_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(), D2D1::Point2F(0, 1)), col, &br);
        SafeRelease(col);
        if (!br) return nullptr;
        it = gradients_.emplace(key, br).first;
    }
    it->second->SetStartPoint(D2D1::Point2F(0, r.top));
    it->second->SetEndPoint(D2D1::Point2F(0, r.bottom));
    return it->second;
}

void Canvas::LinearV(D2D1_RECT_F r, float radius, Color top, Color bottom) {
    ID2D1Brush* b = GradientV(r, top, bottom);
    if (!b) return;
    radius = std::min(radius, std::min(r.right - r.left, r.bottom - r.top) / 2);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), b);
}

IDWriteTextFormat* Canvas::Format(float size, DWRITE_FONT_WEIGHT w, Align a, bool mono) {
    auto key = std::make_tuple((int)(size * 10), (int)w, (int)a, mono ? 1 : 0);
    auto it = formats_.find(key);
    if (it != formats_.end()) return it->second;
    IDWriteTextFormat* f = nullptr;
    g_gfx.dw->CreateTextFormat(mono ? g_gfx.monoFamily.c_str() : g_gfx.family.c_str(), nullptr, w,
                               DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f);
    if (!f) return nullptr;
    f->SetTextAlignment(a == Align::Left ? DWRITE_TEXT_ALIGNMENT_LEADING
                        : a == Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER
                                             : DWRITE_TEXT_ALIGNMENT_TRAILING);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    IDWriteInlineObject* ellipsis = nullptr;
    g_gfx.dw->CreateEllipsisTrimmingSign(f, &ellipsis);
    f->SetTrimming(&trim, ellipsis);
    SafeRelease(ellipsis);
    formats_[key] = f;
    return f;
}

IDWriteTextLayout* Canvas::Layout(const std::wstring& s, IDWriteTextFormat* f, float w, float h) {
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(g_gfx.dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, std::max(1.f, w), std::max(1.f, h), &layout)))
        return nullptr;
    if (tabular_) layout->SetTypography(tabular_, {0, (UINT32)s.size()});
    return layout;
}

void Canvas::Text(const std::wstring& s, D2D1_RECT_F r, float size, Color c, DWRITE_FONT_WEIGHT w, Align a,
                  bool vcenter, bool mono) {
    if (s.empty() || c.a <= 0.002f) return;
    IDWriteTextFormat* f = Format(size, w, a, mono);
    if (!f) return;
    f->SetParagraphAlignment(vcenter ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    IDWriteTextLayout* layout = Layout(s, f, r.right - r.left, r.bottom - r.top);
    if (!layout) return;
    rt_->DrawTextLayout(D2D1::Point2F(r.left, r.top), layout, Brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    layout->Release();
}

float Canvas::Measure(const std::wstring& s, float size, DWRITE_FONT_WEIGHT w, bool mono) {
    if (s.empty()) return 0;
    auto key = std::make_tuple(s, (int)(size * 10), (int)w, mono ? 1 : 0);
    if (auto it = widths_.find(key); it != widths_.end()) return it->second;
    IDWriteTextFormat* f = Format(size, w, Align::Left, mono);
    IDWriteTextLayout* layout = f ? Layout(s, f, 4000, 200) : nullptr;
    if (!layout) return 0;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    layout->Release();
    if (widths_.size() > 512) widths_.clear();   // clocks and titles change; keep it small
    widths_.emplace(key, m.widthIncludingTrailingWhitespace);
    return m.widthIncludingTrailingWhitespace;
}

// Images are painted with a bitmap brush inside the rounded shape, so the
// edge is anti-aliased and nothing ever spills outside it (no layer needed).
void Canvas::DrawImage(Image& img, D2D1_RECT_F r, float radius, float opacity) {
    ID2D1Bitmap* b = img.For(rt_, gen_);
    if (!b) return;
    float sw = (float)img.w, sh = (float)img.h;
    float dw = r.right - r.left, dh = r.bottom - r.top;
    // cover-fit: scale so the image fills the box, centred
    float scale = std::max(dw / sw, dh / sh);
    float ox = r.left + (dw - sw * scale) / 2, oy = r.top + (dh - sh * scale) / 2;
    D2D1_SIZE_F bs = b->GetSize();   // DIPs at the bitmap's DPI
    float kx = sw / bs.width, ky = sh / bs.height;
    ID2D1BitmapBrush* brush = nullptr;
    if (FAILED(rt_->CreateBitmapBrush(b, D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
                                                                      D2D1_BITMAP_INTERPOLATION_MODE_LINEAR),
                                      D2D1::BrushProperties(opacity), &brush)))
        return;
    brush->SetTransform(D2D1::Matrix3x2F::Scale(scale * kx, scale * ky) * D2D1::Matrix3x2F::Translation(ox, oy));
    radius = std::min(radius, std::min(dw, dh) / 2);
    if (radius > 0) rt_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush);
    else rt_->FillRectangle(r, brush);
    brush->Release();
}

void Canvas::PushClipRect(D2D1_RECT_F r) { rt_->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE); }
void Canvas::PopClipRect() { rt_->PopAxisAlignedClip(); }

void Canvas::PushClipRound(D2D1_RECT_F r, float radius) {
    ID2D1RoundedRectangleGeometry* g = nullptr;
    g_gfx.d2d->CreateRoundedRectangleGeometry(D2D1::RoundedRect(r, radius, radius), &g);
    if (layerDepth_ >= (int)layers_.size()) {
        ID2D1Layer* l = nullptr;
        rt_->CreateLayer(&l);
        layers_.push_back(l);
    }
    rt_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), g, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                   layers_[(size_t)layerDepth_++]);
    SafeRelease(g);
}
void Canvas::PopClip() {
    rt_->PopLayer();
    layerDepth_--;
}
void Canvas::PushOpacity(float o) {
    if (layerDepth_ >= (int)layers_.size()) {
        ID2D1Layer* l = nullptr;
        rt_->CreateLayer(&l);
        layers_.push_back(l);
    }
    rt_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                         D2D1::IdentityMatrix(), std::clamp(o, 0.f, 1.f)),
                   layers_[(size_t)layerDepth_++]);
}
void Canvas::PopOpacity() { PopClip(); }

// ------------------------------------------------------------ LayeredSurface
LayeredSurface::~LayeredSurface() { Free(); }

void LayeredSurface::Free() {
    canvas_.Attach(nullptr);
    SafeRelease(rt_);
    if (dc_) {
        if (old_) SelectObject(dc_, old_);
        DeleteDC(dc_);
        dc_ = nullptr;
    }
    if (bmp_) { DeleteObject(bmp_); bmp_ = nullptr; }
    w_ = h_ = capW_ = capH_ = boundH_ = 0;
}

// The island changes height on every frame while the card opens. Rebuilding
// the bitmap and render target each time would also throw away every cached
// brush, text format and image, so the bitmap grows in 128 px steps and the
// target is bound to a 64 px band that covers the frame.
bool LayeredSurface::Begin(int w, int h, float dpiScale) {
    w = std::max(w, 1);
    h = std::max(h, 1);
    if (w != capW_ || h > capH_ || !rt_) {
        int capH = std::max(h, capH_);
        capH = (capH + 127) / 128 * 128;
        Free();
        HDC screen = GetDC(nullptr);
        dc_ = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -capH;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        bmp_ = CreateDIBSection(dc_, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
        if (!bmp_) { Free(); return false; }
        old_ = SelectObject(dc_, bmp_);
        capW_ = w;
        capH_ = capH;
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 0, 0,
            D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
        if (FAILED(g_gfx.d2d->CreateDCRenderTarget(&props, &rt_))) { Free(); return false; }
        rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        canvas_.Attach(rt_);
    }
    w_ = w;
    h_ = h;
    int band = std::min(capH_, (h + 63) / 64 * 64);
    if (band != boundH_) {
        RECT rc{0, 0, w_, band};
        rt_->BindDC(dc_, &rc);
        boundH_ = band;
    }
    rt_->SetDpi(96.f * dpiScale, 96.f * dpiScale);
    rt_->BeginDraw();
    rt_->SetTransform(D2D1::IdentityMatrix());
    rt_->Clear(D2D1::ColorF(0, 0, 0, 0));
    return true;
}

void LayeredSurface::End(HWND hwnd, POINT screenPos) {
    if (!rt_) return;
    HRESULT hr = rt_->EndDraw();
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) { Free(); return; }
    if (FAILED(hr)) {
        char msg[64];
        snprintf(msg, sizeof msg, "LoopIsland: EndDraw failed 0x%08lx\n", (unsigned long)hr);
        OutputDebugStringA(msg);
    }
    SIZE size{w_, h_};
    POINT src{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd, nullptr, &screenPos, &size, dc_, &src, 0, &blend, ULW_ALPHA);
}
