/* ARM64 D3D11 + DXGI live hardware verification tool for Adreno 740 (s23gpu.sys + Mesa Freedreno) */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define IMG_W 256u
#define IMG_H 256u

typedef struct _VERTEX {
    float pos[3];
    float col[4];
} VERTEX;

typedef struct _CB_DATA {
    float angle;
    float pad[3];
} CB_DATA;

static const char g_ShaderSrc[] =
    "cbuffer CB : register(b0) { float angle; float3 pad; };\n"
    "struct VS_IN  { float3 pos : POSITION; float4 col : COLOR; };\n"
    "struct VS_OUT { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
    "VS_OUT VSMain(VS_IN i) {\n"
    "  VS_OUT o;\n"
    "  float s = sin(angle), c = cos(angle);\n"
    "  o.pos = float4(i.pos.x * c - i.pos.y * s, i.pos.x * s + i.pos.y * c, i.pos.z, 1.0);\n"
    "  o.col = i.col;\n"
    "  return o;\n"
    "}\n"
    "float4 PSMain(VS_OUT i) : SV_TARGET { return i.col; }\n";

static int SaveBmp24(const char *path, const unsigned char *rgba, UINT w, UINT h, UINT pitch)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    UINT rowBytes = ((w * 3u) + 3u) & ~3u;
    UINT imgBytes = rowBytes * h;
    BITMAPFILEHEADER bfh;
    BITMAPINFOHEADER bih;
    memset(&bfh, 0, sizeof(bfh));
    memset(&bih, 0, sizeof(bih));
    bfh.bfType = 0x4D42;
    bfh.bfOffBits = sizeof(bfh) + sizeof(bih);
    bfh.bfSize = bfh.bfOffBits + imgBytes;
    bih.biSize = sizeof(bih);
    bih.biWidth = (LONG)w;
    bih.biHeight = (LONG)h; /* positive = bottom-up */
    bih.biPlanes = 1;
    bih.biBitCount = 24;
    bih.biCompression = BI_RGB;
    bih.biSizeImage = imgBytes;
    fwrite(&bfh, 1, sizeof(bfh), f);
    fwrite(&bih, 1, sizeof(bih), f);
    unsigned char *row = (unsigned char *)calloc(1, rowBytes);
    if (!row) { fclose(f); return 0; }
    for (UINT y = 0; y < h; y++) {
        const unsigned char *src = rgba + (h - 1u - y) * pitch;
        for (UINT x = 0; x < w; x++) {
            row[x * 3u + 0u] = src[x * 4u + 2u]; /* B */
            row[x * 3u + 1u] = src[x * 4u + 1u]; /* G */
            row[x * 3u + 2u] = src[x * 4u + 0u]; /* R */
        }
        fwrite(row, 1, rowBytes, f);
    }
    free(row);
    fclose(f);
    return 1;
}

static void PixelAt(const unsigned char *bits, UINT pitch, UINT x, UINT y,
                    unsigned char *r, unsigned char *g, unsigned char *b, unsigned char *a)
{
    const unsigned char *p = bits + y * pitch + x * 4u;
    *r = p[0]; *g = p[1]; *b = p[2]; *a = p[3];
}

static int ApproxEq(unsigned char v, int exp, int tol)
{
    int d = (int)v - exp;
    return d >= -tol && d <= tol;
}

static IDXGIAdapter1 *PickS23Adapter(IDXGIFactory1 *fac)
{
    IDXGIAdapter1 *best = NULL, *fallback = NULL;
    for (UINT i = 0; ; i++) {
        IDXGIAdapter1 *adp = NULL;
        DXGI_ADAPTER_DESC1 desc;
        if (IDXGIFactory1_EnumAdapters1(fac, i, &adp) != S_OK) break;
        memset(&desc, 0, sizeof(desc));
        IDXGIAdapter1_GetDesc1(adp, &desc);
        printf("  DXGI[%u]: \"%ls\" ven=0x%04x dev=0x%04x luid=%ld:%08lx flags=0x%x\n",
               i, desc.Description, desc.VendorId, desc.DeviceId,
               desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart, desc.Flags);
        if (wcsstr(desc.Description, L"Adreno") || wcsstr(desc.Description, L"S23")) {
            if (!best) { best = adp; continue; }
        } else if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
                   desc.VendorId != 0x1414u &&
                   !wcsstr(desc.Description, L"Basic Render")) {
            if (!fallback) { fallback = adp; continue; }
        }
        IDXGIAdapter1_Release(adp);
    }
    if (best) {
        if (fallback) IDXGIAdapter1_Release(fallback);
        return best;
    }
    return fallback;
}

static int RunWindowLoop(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11RenderTargetView *rtv,
                         ID3D11Texture2D *rtTex, ID3D11Texture2D *stgTex, ID3D11Buffer *cb,
                         IDXGIAdapter1 *adp)
{
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "S23GpuD3D11Win";
    RegisterClassA(&wc);

    RECT rc = { 0, 0, 640, 640 };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowA("S23GpuD3D11Win", "S23 Adreno 740 Live D3D11",
                              WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              80, 80, rc.right - rc.left, rc.bottom - rc.top,
                              NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) {
        printf("[WARN] CreateWindowA failed err=%lu\n", GetLastError());
        return 0;
    }

    IDXGIFactory *fac0 = NULL;
    IDXGIAdapter1_GetParent(adp, &IID_IDXGIFactory, (void **)&fac0);
    DXGI_SWAP_CHAIN_DESC scd;
    memset(&scd, 0, sizeof(scd));
    scd.BufferCount = 2;
    scd.BufferDesc.Width = IMG_W;
    scd.BufferDesc.Height = IMG_H;
    scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain *sc = NULL;
    HRESULT hrSc = fac0 ? IDXGIFactory_CreateSwapChain(fac0, (IUnknown *)dev, &scd, &sc) : E_FAIL;
    printf("Window: CreateSwapChain hr=0x%08lx sc=%p\n", (unsigned long)hrSc, (void *)sc);
    if (fac0) IDXGIFactory_Release(fac0);

    ID3D11Texture2D *scBack = NULL;
    ID3D11RenderTargetView *scRtv = NULL;
    if (SUCCEEDED(hrSc) && sc) {
        HRESULT hrBuf = IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&scBack);
        if (SUCCEEDED(hrBuf) && scBack) {
            hrBuf = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)scBack, NULL, &scRtv);
        }
        printf("Window: SwapChain backbuffer RTV hr=0x%08lx\n", (unsigned long)hrBuf);
    }

    HDC hdc = GetDC(hwnd);
    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = (LONG)IMG_W;
    bmi.bmiHeader.biHeight = -(LONG)IMG_H; /* top-down */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    unsigned char *bgra = (unsigned char *)malloc(IMG_W * IMG_H * 4u);

    HRESULT firstPres = S_OK;
    for (UINT f = 0; f < 180u; f++) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        D3D11_MAPPED_SUBRESOURCE mc;
        if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mc))) {
            CB_DATA *d = (CB_DATA *)mc.pData;
            d->angle = (float)f * (2.0f * 3.14159265f / 180.0f);
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)cb, 0);
        }
        const float bg[4] = { 0.05f, 0.05f, 0.15f, 1.0f };
        if (scRtv) {
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &scRtv, NULL);
            ID3D11DeviceContext_ClearRenderTargetView(ctx, scRtv, bg);
            ID3D11DeviceContext_Draw(ctx, 3, 0);
            HRESULT hp = IDXGISwapChain_Present(sc, 0, 0);
            if (f == 0u) {
                firstPres = hp;
                printf("Window: first SwapChain::Present hr=0x%08lx\n", (unsigned long)firstPres);
            }
        } else {
            ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
            ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, bg);
            ID3D11DeviceContext_Draw(ctx, 3, 0);
            ID3D11DeviceContext_Flush(ctx);
            ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)stgTex, (ID3D11Resource *)rtTex);
            D3D11_MAPPED_SUBRESOURCE mr;
            if (bgra && SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)stgTex, 0, D3D11_MAP_READ, 0, &mr))) {
                const unsigned char *s = (const unsigned char *)mr.pData;
                for (UINT y = 0; y < IMG_H; y++) {
                    const unsigned char *sr = s + y * mr.RowPitch;
                    unsigned char *dr = bgra + y * IMG_W * 4u;
                    for (UINT x = 0; x < IMG_W; x++) {
                        dr[x * 4u + 0u] = sr[x * 4u + 2u];
                        dr[x * 4u + 1u] = sr[x * 4u + 1u];
                        dr[x * 4u + 2u] = sr[x * 4u + 0u];
                        dr[x * 4u + 3u] = sr[x * 4u + 3u];
                    }
                }
                ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)stgTex, 0);
                StretchDIBits(hdc, 0, 0, 640, 640, 0, 0, IMG_W, IMG_H, bgra, &bmi, DIB_RGB_COLORS, SRCCOPY);
            }
        }
        Sleep(16);
    }

    free(bgra);
    ReleaseDC(hwnd, hdc);
    if (scRtv) ID3D11RenderTargetView_Release(scRtv);
    if (scBack) ID3D11Texture2D_Release(scBack);
    if (sc) IDXGISwapChain_Release(sc);
    DestroyWindow(hwnd);
    return 1;
}

int main(int argc, char **argv)
{
    int doWindow = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--window") == 0) doWindow = 1;
    }

    SetEnvironmentVariableA("TOPAZGPU_ENABLE", "1");
    const char *dbgEnv = getenv("FD_MESA_DEBUG");
    printf("==== s23gpu_d3d11_test (FD_MESA_DEBUG=%s, window=%d) ====\n",
           dbgEnv ? dbgEnv : "<default-gmem>", doWindow);

    IDXGIFactory1 *fac = NULL;
    HRESULT hr = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&fac);
    if (FAILED(hr) || !fac) {
        printf("[FAIL] CreateDXGIFactory1 hr=0x%08lx\n", (unsigned long)hr);
        return 1;
    }

    IDXGIAdapter1 *adp = PickS23Adapter(fac);
    if (!adp) {
        printf("[FAIL] No s23gpu / hardware DXGI adapter found!\n");
        IDXGIFactory1_Release(fac);
        return 1;
    }
    DXGI_ADAPTER_DESC1 adesc;
    IDXGIAdapter1_GetDesc1(adp, &adesc);
    printf("Selected Adapter: \"%ls\" luid=%ld:%08lx\n",
           adesc.Description, adesc.AdapterLuid.HighPart, adesc.AdapterLuid.LowPart);

    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL gotFl = (D3D_FEATURE_LEVEL)0;
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    hr = D3D11CreateDevice((IDXGIAdapter *)adp, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0,
                           levels, 3, D3D11_SDK_VERSION, &dev, &gotFl, &ctx);
    if (FAILED(hr) || !dev || !ctx) {
        printf("[FAIL] D3D11CreateDevice hr=0x%08lx dev=%p ctx=%p\n",
               (unsigned long)hr, (void *)dev, (void *)ctx);
        IDXGIAdapter1_Release(adp);
        IDXGIFactory1_Release(fac);
        return 1;
    }
    printf("D3D11CreateDevice OK! FeatureLevel=0x%x\n", (unsigned)gotFl);

    D3D11_TEXTURE2D_DESC td;
    memset(&td, 0, sizeof(td));
    td.Width = IMG_W;
    td.Height = IMG_H;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;

    ID3D11Texture2D *rtTex = NULL, *stgTex = NULL;
    hr = ID3D11Device_CreateTexture2D(dev, &td, NULL, &rtTex);
    if (FAILED(hr)) { printf("[FAIL] CreateTexture2D RT hr=0x%08lx\n", (unsigned long)hr); return 1; }

    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = ID3D11Device_CreateTexture2D(dev, &td, NULL, &stgTex);
    if (FAILED(hr)) { printf("[FAIL] CreateTexture2D Staging hr=0x%08lx\n", (unsigned long)hr); return 1; }

    ID3D11RenderTargetView *rtv = NULL;
    hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)rtTex, NULL, &rtv);
    if (FAILED(hr)) { printf("[FAIL] CreateRTV hr=0x%08lx\n", (unsigned long)hr); return 1; }

    /* Test 1: Clear RenderTarget + Staging Readback */
    const float clearCol[4] = { 0.2f, 0.4f, 0.8f, 1.0f };
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, clearCol);
    ID3D11DeviceContext_Flush(ctx);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)stgTex, (ID3D11Resource *)rtTex);

    D3D11_MAPPED_SUBRESOURCE map;
    hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)stgTex, 0, D3D11_MAP_READ, 0, &map);
    if (FAILED(hr)) { printf("[FAIL] Test 1 Map hr=0x%08lx\n", (unsigned long)hr); return 1; }
    unsigned char r0, g0, b0, a0;
    PixelAt((const unsigned char *)map.pData, map.RowPitch, 128u, 128u, &r0, &g0, &b0, &a0);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)stgTex, 0);
    printf("Test 1 (Clear RT readback @128,128): RGBA=(%u,%u,%u,%u) expect~(51,102,204,255)\n", r0, g0, b0, a0);
    if (!ApproxEq(r0, 51, 2) || !ApproxEq(g0, 102, 2) || !ApproxEq(b0, 204, 2) || !ApproxEq(a0, 255, 2)) {
        printf("[FAIL] Test 1 pixel mismatch!\n");
        return 1;
    }
    printf("Test 1: PASS\n");

    /* Compile HLSL VS + PS */
    ID3DBlob *vsBlob = NULL, *psBlob = NULL, *errBlob = NULL;
    hr = D3DCompile(g_ShaderSrc, sizeof(g_ShaderSrc) - 1, "tri.hlsl", NULL, NULL,
                    "VSMain", "vs_4_0", 0, 0, &vsBlob, &errBlob);
    if (FAILED(hr)) {
        printf("[FAIL] D3DCompile VS hr=0x%08lx: %s\n", (unsigned long)hr,
               errBlob ? (const char *)ID3D10Blob_GetBufferPointer(errBlob) : "");
        return 1;
    }
    hr = D3DCompile(g_ShaderSrc, sizeof(g_ShaderSrc) - 1, "tri.hlsl", NULL, NULL,
                    "PSMain", "ps_4_0", 0, 0, &psBlob, &errBlob);
    if (FAILED(hr)) {
        printf("[FAIL] D3DCompile PS hr=0x%08lx: %s\n", (unsigned long)hr,
               errBlob ? (const char *)ID3D10Blob_GetBufferPointer(errBlob) : "");
        return 1;
    }

    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps = NULL;
    hr = ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsBlob),
                                         ID3D10Blob_GetBufferSize(vsBlob), NULL, &vs);
    if (FAILED(hr)) { printf("[FAIL] CreateVertexShader hr=0x%08lx\n", (unsigned long)hr); return 1; }
    hr = ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psBlob),
                                        ID3D10Blob_GetBufferSize(psBlob), NULL, &ps);
    if (FAILED(hr)) { printf("[FAIL] CreatePixelShader hr=0x%08lx\n", (unsigned long)hr); return 1; }

    D3D11_INPUT_ELEMENT_DESC ied[2] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    ID3D11InputLayout *il = NULL;
    hr = ID3D11Device_CreateInputLayout(dev, ied, 2, ID3D10Blob_GetBufferPointer(vsBlob),
                                        ID3D10Blob_GetBufferSize(vsBlob), &il);
    if (FAILED(hr)) { printf("[FAIL] CreateInputLayout hr=0x%08lx\n", (unsigned long)hr); return 1; }

    const VERTEX verts[3] = {
        { {  0.00f,  0.65f, 0.0f }, { 1.0f, 0.0f, 0.0f, 1.0f } },
        { {  0.65f, -0.65f, 0.0f }, { 0.0f, 1.0f, 0.0f, 1.0f } },
        { { -0.65f, -0.65f, 0.0f }, { 0.0f, 0.0f, 1.0f, 1.0f } },
    };
    D3D11_BUFFER_DESC vbd;
    memset(&vbd, 0, sizeof(vbd));
    vbd.ByteWidth = sizeof(verts);
    vbd.Usage = D3D11_USAGE_IMMUTABLE;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vsd = { verts, 0, 0 };
    ID3D11Buffer *vb = NULL;
    hr = ID3D11Device_CreateBuffer(dev, &vbd, &vsd, &vb);
    if (FAILED(hr)) { printf("[FAIL] CreateBuffer VB hr=0x%08lx\n", (unsigned long)hr); return 1; }

    D3D11_BUFFER_DESC cbd;
    memset(&cbd, 0, sizeof(cbd));
    cbd.ByteWidth = sizeof(CB_DATA);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    CB_DATA cbInit = { 0.0f, { 0.0f, 0.0f, 0.0f } };
    D3D11_SUBRESOURCE_DATA csd = { &cbInit, 0, 0 };
    ID3D11Buffer *cb = NULL;
    hr = ID3D11Device_CreateBuffer(dev, &cbd, &csd, &cb);
    if (FAILED(hr)) { printf("[FAIL] CreateBuffer CB hr=0x%08lx\n", (unsigned long)hr); return 1; }

    /* Pipeline setup */
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)IMG_W, (float)IMG_H, 0.0f, 1.0f };
    UINT stride = sizeof(VERTEX), offset = 0;
    ID3D11DeviceContext_IASetInputLayout(ctx, il);
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &stride, &offset);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cb);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);

    /* Test 2: HLSL Triangle Draw + Pixel Verification + BMP Export */
    const float navyBg[4] = { 0.05f, 0.05f, 0.15f, 1.0f };
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, navyBg);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    ID3D11DeviceContext_Flush(ctx);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)stgTex, (ID3D11Resource *)rtTex);

    hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)stgTex, 0, D3D11_MAP_READ, 0, &map);
    if (FAILED(hr)) { printf("[FAIL] Test 2 Map hr=0x%08lx\n", (unsigned long)hr); return 1; }
    unsigned char bgR, bgG, bgB, bgA, cR, cG, cB, cA;
    unsigned char tR, tG, tB, tA, rR, rG, rB, rA, lR, lG, lB, lA;
    const unsigned char *bits = (const unsigned char *)map.pData;
    PixelAt(bits, map.RowPitch,   4u,   4u, &bgR, &bgG, &bgB, &bgA);
    PixelAt(bits, map.RowPitch, 128u, 140u,  &cR,  &cG,  &cB,  &cA);
    PixelAt(bits, map.RowPitch, 128u,  70u,  &tR,  &tG,  &tB,  &tA);
    PixelAt(bits, map.RowPitch, 175u, 190u,  &rR,  &rG,  &rB,  &rA);
    PixelAt(bits, map.RowPitch,  81u, 190u,  &lR,  &lG,  &lB,  &lA);
    int bmpOk = SaveBmp24("C:\\Users\\Public\\s23gpu_tri.bmp", bits, IMG_W, IMG_H, map.RowPitch);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)stgTex, 0);

    printf("Test 2 (HLSL 3D Triangle): bmp=%d\n", bmpOk);
    printf("  corner(4,4)    = (%3u,%3u,%3u,%3u) expect~(13,13,38,255)\n", bgR, bgG, bgB, bgA);
    printf("  center(128,140)= (%3u,%3u,%3u,%3u) expect~(>60,>60,>60)\n", cR, cG, cB, cA);
    printf("  top(128,70)    = (%3u,%3u,%3u,%3u) expect Red-dom\n", tR, tG, tB, tA);
    printf("  right(175,190) = (%3u,%3u,%3u,%3u) expect Green-dom\n", rR, rG, rB, rA);
    printf("  left(81,190)   = (%3u,%3u,%3u,%3u) expect Blue-dom\n", lR, lG, lB, lA);

    if (!ApproxEq(bgR, 13, 3) || !ApproxEq(bgG, 13, 3) || !ApproxEq(bgB, 38, 3) ||
        !(cR > 60 && cG > 60 && cB > 60) ||
        !(tR > tG && tR > tB) ||
        !(rG > rR && rG > rB) ||
        !(lB > lR && lB > lG)) {
        printf("[FAIL] Test 2 triangle pixel check failed!\n");
        return 1;
    }
    printf("Test 2: PASS\n");

    /* Test 3: 120-Frame Rotating Triangle Benchmark */
    LARGE_INTEGER qf, t0, t1;
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&t0);
    for (UINT f = 0; f < 120u; f++) {
        D3D11_MAPPED_SUBRESOURCE mc;
        hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mc);
        if (FAILED(hr)) { printf("[FAIL] Test 3 CB Map #%u hr=0x%08lx\n", f, (unsigned long)hr); return 1; }
        CB_DATA *cd = (CB_DATA *)mc.pData;
        cd->angle = (float)f * (2.0f * 3.14159265f / 120.0f);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)cb, 0);

        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, navyBg);
        ID3D11DeviceContext_Draw(ctx, 3, 0);
        ID3D11DeviceContext_Flush(ctx);
    }
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)stgTex, (ID3D11Resource *)rtTex);
    hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)stgTex, 0, D3D11_MAP_READ, 0, &map);
    if (FAILED(hr)) { printf("[FAIL] Test 3 Final Map hr=0x%08lx\n", (unsigned long)hr); return 1; }
    PixelAt((const unsigned char *)map.pData, map.RowPitch, 128u, 140u, &cR, &cG, &cB, &cA);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)stgTex, 0);
    QueryPerformanceCounter(&t1);

    double elapsedMs = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)qf.QuadPart;
    double fps = elapsedMs > 0.0 ? (120000.0 / elapsedMs) : 0.0;
    printf("Test 3 (120-frame rotate): %.2f ms (%.1f FPS) final@128,140=(%u,%u,%u,%u)\n",
           elapsedMs, fps, cR, cG, cB, cA);
    if (!(cR > 50 && cG > 50 && cB > 50)) {
        printf("[FAIL] Test 3 final frame pixel mismatch!\n");
        return 1;
    }
    printf("Test 3: PASS\n");

    if (doWindow) {
        RunWindowLoop(dev, ctx, rtv, rtTex, stgTex, cb, adp);
    }

    ID3D11Buffer_Release(cb);
    ID3D11Buffer_Release(vb);
    ID3D11InputLayout_Release(il);
    ID3D11PixelShader_Release(ps);
    ID3D11VertexShader_Release(vs);
    ID3D10Blob_Release(psBlob);
    ID3D10Blob_Release(vsBlob);
    if (errBlob) ID3D10Blob_Release(errBlob);
    ID3D11RenderTargetView_Release(rtv);
    ID3D11Texture2D_Release(stgTex);
    ID3D11Texture2D_Release(rtTex);
    ID3D11DeviceContext_Release(ctx);
    ID3D11Device_Release(dev);
    IDXGIAdapter1_Release(adp);
    IDXGIFactory1_Release(fac);
    printf("ALL D3D11 TESTS PASSED!\n");
    return 0;
}
