#include "App.hpp"
#include "AppInternal.hpp"
#include "FilePersistence.hpp"
#include "single_instance_test_support.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace quaddeck {
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// A Matroska file holding one subtitle stream of one second and nothing
// else: all the folder list reads of a file is what opening it says.
void writeSubtitleOnlyMatroska(const std::filesystem::path& path, AVCodecID codec) {
    const std::string utf8 = wideToUtf8Text(path.wstring());
    AVFormatContext* format = nullptr;
    require(avformat_alloc_output_context2(&format, nullptr, "matroska", utf8.c_str()) >= 0 && format,
            "Cannot make a Matroska writer");
    AVStream* stream = avformat_new_stream(format, nullptr);
    require(stream != nullptr, "Cannot add a subtitle stream");
    stream->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
    stream->codecpar->codec_id = codec;
    stream->time_base = {1, 1000};
    AVPacket* packet = av_packet_alloc();
    require(packet != nullptr && avio_open(&format->pb, utf8.c_str(), AVIO_FLAG_WRITE) >= 0 &&
                avformat_write_header(format, nullptr) >= 0 && av_new_packet(packet, 5) >= 0,
            "Cannot start the Matroska file");
    std::memcpy(packet->data, "Hello", 5);
    packet->stream_index = stream->index;
    packet->pts = packet->dts = 0;
    packet->duration = 1000;
    av_packet_rescale_ts(packet, {1, 1000}, stream->time_base);
    const bool written = av_interleaved_write_frame(format, packet) >= 0 && av_write_trailer(format) >= 0;
    av_packet_free(&packet);
    avio_closep(&format->pb);
    avformat_free_context(format);
    require(written, "Cannot finish the Matroska file");
}

// Native geometry without the App window procedure: no timer, renderer,
// audio device, foreground activation or settings read/write is started.
class HiddenWindow {
public:
    HiddenWindow() {
        window = CreateWindowExW(0, L"STATIC", L"QuadDeck regression",
            WS_POPUP, 0, 0, 400, 300, nullptr, nullptr, nullptr, nullptr);
        require(window != nullptr, "Cannot create hidden geometry window");
        menu = CreateWindowExW(0, L"STATIC", L"", WS_CHILD,
            0, 0, 1, 1, window, nullptr, nullptr, nullptr);
        require(menu != nullptr, "Cannot create hidden child");
    }
    ~HiddenWindow() { DestroyWindow(window); }
    POINT screenPoint(int x, int y) const {
        POINT point{x, y};
        require(ClientToScreen(window, &point) != FALSE, "Cannot map test point");
        return point;
    }
    HWND window{};
    HWND menu{};
};

void requireReplacementLayout(const std::vector<PanelRow>& rows, const PanelLayout& layout) {
    std::array<bool, kMaxPanes> choices{};
    bool cancel = false;
    float previousBottom = layout.content.y - layout.scroll;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& geometry = layout.rows[i];
        require(geometry.row.y >= previousBottom - 0.01F, "Replacement rows overlap after wrapping the note");
        previousBottom = geometry.row.bottom();
        const bool isCancel = rows[i].id == SettingId::EmbyReplaceCancel || rows[i].id == SettingId::LocalReplaceCancel;
        const bool isChoice = rows[i].id == SettingId::EmbyReplaceChoice || rows[i].id == SettingId::LocalReplaceChoice;
        if (!isCancel && !isChoice) continue;
        for (std::size_t part = 0; part < geometry.parts.size(); ++part) {
            const auto& box = geometry.parts[part];
            require(box.visible() && box.x >= geometry.row.x && box.right() <= geometry.row.right() + 0.01F &&
                    box.y >= geometry.row.y && box.bottom() <= geometry.row.bottom() + 0.01F,
                    "Replacement action escaped its drawn row");
            const auto hit = settingsPanelHitTest(box.x + box.width * 0.5F, box.y + box.height * 0.5F, layout, rows);
            require(hit.row == static_cast<int>(i) && hit.part == static_cast<int>(part),
                    "The drawn replacement action and its hit target disagree");
            if (isCancel) {
                require(hit.kind == PanelHitKind::Button, "The replacement Cancel button is not hittable");
                cancel = true;
            } else {
                const int pane = panelRowParam(rows[i], hit.part);
                require(hit.kind == PanelHitKind::Tile && pane >= 0 && pane < static_cast<int>(kMaxPanes) && !choices[pane],
                        "A replacement tile is not hittable or names the wrong pane");
                choices[pane] = true;
            }
        }
    }
    require(cancel && std::all_of(choices.begin(), choices.end(), [](bool present) { return present; }),
            "Wrapped replacement rows lost Cancel or one of the five pane choices");
}

// Optional visual evidence uses the production Direct2D/DirectWrite painter
// on an offscreen WARP texture. It never creates the App window, starts media
// or loads/saves account settings. Every requested image must render and save.
void renderFixtureBmp(OverlayScene scene, const std::filesystem::path& path) {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            device.GetAddressOf(), nullptr, context.GetAddressOf())), "Cannot create the offscreen WARP device");
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(scene.width);
    desc.Height = static_cast<UINT>(scene.height);
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> target;
    require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, target.GetAddressOf())),
            "Cannot create the offscreen fixture texture");
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> targetView;
    require(SUCCEEDED(device->CreateRenderTargetView(target.Get(), nullptr, targetView.GetAddressOf())),
            "Cannot create the fixture render target");
    const float background[]{0.025F, 0.03F, 0.04F, 1.0F};
    context->ClearRenderTargetView(targetView.Get(), background);
    Microsoft::WRL::ComPtr<IDXGISurface> surface;
    require(SUCCEEDED(target.As(&surface)), "Cannot expose the fixture texture to Direct2D");
    Overlay overlay;
    require(overlay.initialize(device.Get()), "Cannot initialize the production overlay for the fixture");
    bool measuredDetail = false;
    bool measuredNote = false;
    for (auto& row : scene.panel.rows) {
        const float width = std::max(0.0F, scene.panel.layout.sheet.width - 40.0F * scene.scale);
        if (row.kind == PanelRowKind::MediaDetail) {
            overlay.measureMediaDetail(row, width, scene.scale);
            measuredDetail = true;
        }
        if (row.kind == PanelRowKind::Note && row.wrapNote) {
            const float fallbackHeight = panelRowHeight(row, scene.scale, {}, width);
            overlay.measureWrappedNote(row, width, scene.scale);
            require(row.measuredNoteHeight > 0.0F && panelRowHeight(row, scene.scale, {}, width) <= fallbackHeight,
                    "The font-free wrapped-note fallback cannot contain the actual DirectWrite text");
            measuredNote = true;
        }
    }
    if (measuredDetail || measuredNote) {
        scene.panel.layout = settingsPanelLayout(scene.width, scene.height, scene.scale, scene.panel.rows,
            scene.panel.layout.scroll, 1.0F, {}, scene.panel.layout.sheet.width);
    }
    if (measuredNote) {
        const bool replacement = std::any_of(scene.panel.rows.begin(), scene.panel.rows.end(), [](const PanelRow& row) {
            return row.id == SettingId::EmbyReplaceCancel || row.id == SettingId::LocalReplaceCancel;
        });
        if (replacement) requireReplacementLayout(scene.panel.rows, scene.panel.layout);
        Microsoft::WRL::ComPtr<IDWriteFactory> dwrite;
        require(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(dwrite.GetAddressOf()))), "Cannot measure the wrapped-note fixture");
        Microsoft::WRL::ComPtr<IDWriteTextFormat> noteFormat;
        require(SUCCEEDED(dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0F * scene.scale, L"", &noteFormat)),
                "Cannot create the fixture's Small note font");
        for (std::size_t i = 0; i < scene.panel.rows.size(); ++i) {
            const auto& row = scene.panel.rows[i];
            if (!row.wrapNote) continue;
            const auto box = panelWrappedNoteTextBox(scene.panel.layout.rows[i].row, scene.scale);
            Microsoft::WRL::ComPtr<IDWriteTextLayout> text;
            require(SUCCEEDED(dwrite->CreateTextLayout(row.label.c_str(), static_cast<UINT32>(row.label.size()),
                    noteFormat.Get(), box.width, box.height, &text)), "Cannot lay out the complete replacement note");
            text->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            text->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            text->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            text->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, kPanelWrappedNoteLineHeight * scene.scale,
                                 kPanelWrappedNoteLineHeight * scene.scale * 0.8F);
            DWRITE_TEXT_METRICS metrics{};
            DWRITE_OVERHANG_METRICS ink{};
            require(SUCCEEDED(text->GetMetrics(&metrics)) && SUCCEEDED(text->GetOverhangMetrics(&ink)) &&
                    std::ceil(metrics.height) == row.measuredNoteHeight && metrics.height <= box.height &&
                    ink.top <= 0.5F && ink.bottom <= 0.5F,
                    "The actual DirectWrite replacement note exceeds its drawing clip");
        }
    }
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf())),
            "Cannot create the fixture readback texture");
    const std::size_t rowBytes = static_cast<std::size_t>(desc.Width) * 4;
    std::vector<unsigned char> syntheticFrames;
    if (scene.activePaneCount > 1) {
        // Synthetic colour fields stand in for decoded pictures. All pane
        // cells, chrome, target borders, transport and panel remain App-built.
        syntheticFrames.assign(rowBytes * desc.Height, 0);
        for (UINT y = 0; y < desc.Height; ++y) {
            for (UINT x = 0; x < desc.Width; ++x) {
                const std::size_t at = static_cast<std::size_t>(y) * rowBytes + x * 4;
                syntheticFrames[at + 3] = 255;
                for (std::size_t pane = 0; pane < scene.panes.size(); ++pane) {
                    const auto& picture = scene.panes[pane];
                    if (!picture.active || x < picture.cell.x || y < picture.cell.y ||
                        x >= picture.cell.x + picture.cell.width || y >= picture.cell.y + picture.cell.height) continue;
                    const unsigned stripe = (x + y / 2) / 100 % 2;
                    syntheticFrames[at] = static_cast<unsigned char>(pane % 2 ? 42 + stripe * 12 : 74 + stripe * 18);
                    syntheticFrames[at + 1] = static_cast<unsigned char>(pane % 2 ? 54 + stripe * 15 : 56 + stripe * 12);
                    syntheticFrames[at + 2] = static_cast<unsigned char>(pane % 2 ? 94 + stripe * 24 : 32 + stripe * 10);
                    break;
                }
            }
        }
    }
    const auto drawAndRead = [&](const OverlayScene& drawn) {
        context->ClearRenderTargetView(targetView.Get(), background);
        if (!syntheticFrames.empty())
            context->UpdateSubresource(target.Get(), 0, nullptr, syntheticFrames.data(), static_cast<UINT>(rowBytes), 0);
        overlay.draw(surface.Get(), drawn);
        require(overlay.ready(), "The overlay lost its device while drawing the fixture");
        context->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        require(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)),
                "Cannot read back the drawn fixture");
        std::vector<unsigned char> pixels(rowBytes * desc.Height);
        for (UINT row = 0; row < desc.Height; ++row) {
            std::memcpy(pixels.data() + rowBytes * row,
                        static_cast<const unsigned char*>(mapped.pData) + static_cast<std::size_t>(mapped.RowPitch) * row,
                        rowBytes);
        }
        context->Unmap(staging.Get(), 0);
        return pixels;
    };
    std::vector<unsigned char> withoutArtwork;
    if (measuredDetail) {
        auto plain = scene;
        for (auto& row : plain.panel.rows) {
            row.mediaDetail.posterKey.clear();
            row.mediaDetail.backdropKey.clear();
        }
        withoutArtwork = drawAndRead(plain);
    }
    std::vector<unsigned char> withoutTileArtwork;
    const bool tileArtwork = scene.panel.thumbnails && std::any_of(scene.panel.rows.begin(), scene.panel.rows.end(),
        [&](const PanelRow& row) {
            return row.kind == PanelRowKind::Tiles && std::any_of(row.tileKeys.begin(), row.tileKeys.end(),
                [&](const std::string& key) {
                    const auto bytes = scene.panel.thumbnails->find(key);
                    return bytes && !bytes->empty();
                });
        });
    if (tileArtwork) {
        auto plain = scene;
        plain.panel.thumbnails = nullptr;
        withoutTileArtwork = drawAndRead(plain);
    }
    const auto pixels = drawAndRead(scene);
    if (measuredNote) {
        auto spacious = scene;
        for (std::size_t i = 0; i < spacious.panel.rows.size(); ++i) {
            if (spacious.panel.rows[i].wrapNote) spacious.panel.layout.rows[i].row.height += 40.0F * scene.scale;
        }
        require(pixels == drawAndRead(spacious), "Increasing the replacement-note clip exposed missing glyph pixels");
    }
    if (!withoutTileArtwork.empty()) {
        std::size_t changed = 0;
        for (std::size_t at = 0; at < pixels.size(); at += 4) {
            if (std::abs(static_cast<int>(pixels[at]) - withoutTileArtwork[at]) > 4 ||
                std::abs(static_cast<int>(pixels[at + 1]) - withoutTileArtwork[at + 1]) > 4 ||
                std::abs(static_cast<int>(pixels[at + 2]) - withoutTileArtwork[at + 2]) > 4) ++changed;
        }
        require(changed > 1000, "Synthetic cached bitmap bytes did not reach the native tile renderer");
    }
    if (!withoutArtwork.empty()) {
        for (std::size_t row = 0; row < scene.panel.rows.size(); ++row) {
            const auto& detail = scene.panel.rows[row];
            if (detail.kind != PanelRowKind::MediaDetail) continue;
            const auto& geometry = scene.panel.layout.rows[row];
            std::size_t posterChanged = 0, backdropChanged = 0;
            for (UINT y = 0; y < desc.Height; ++y) {
                for (UINT x = 0; x < desc.Width; ++x) {
                    if (!geometry.row.contains(static_cast<float>(x), static_cast<float>(y))) continue;
                    const std::size_t at = static_cast<std::size_t>(y) * rowBytes + x * 4;
                    const bool changed = std::abs(static_cast<int>(pixels[at]) - withoutArtwork[at]) > 4 ||
                        std::abs(static_cast<int>(pixels[at + 1]) - withoutArtwork[at + 1]) > 4 ||
                        std::abs(static_cast<int>(pixels[at + 2]) - withoutArtwork[at + 2]) > 4;
                    if (!changed) continue;
                    if (geometry.mediaDetail.poster.contains(static_cast<float>(x), static_cast<float>(y))) ++posterChanged;
                    else ++backdropChanged;
                }
            }
            if (!detail.mediaDetail.posterKey.empty())
                require(posterChanged > 1000, "Synthetic poster bytes did not change the production-rendered poster");
            if (!detail.mediaDetail.backdropKey.empty())
                require(backdropChanged > 1000, "Synthetic backdrop bytes did not change the production-rendered background");
        }
    }
    std::size_t brightPixels = 0;
    for (std::size_t offset = 0; offset < pixels.size(); offset += 4) {
        if (pixels[offset] > 160 && pixels[offset + 1] > 160 && pixels[offset + 2] > 160) ++brightPixels;
    }
    require(brightPixels > 500, "The requested fixture rendered blank or without visible text");
    BITMAPFILEHEADER fileHeader{};
    BITMAPINFOHEADER imageHeader{};
    fileHeader.bfType = 0x4d42;
    fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(imageHeader);
    fileHeader.bfSize = fileHeader.bfOffBits + static_cast<DWORD>(pixels.size());
    imageHeader.biSize = sizeof(imageHeader);
    imageHeader.biWidth = static_cast<LONG>(desc.Width);
    imageHeader.biHeight = -static_cast<LONG>(desc.Height);  // top row first
    imageHeader.biPlanes = 1;
    imageHeader.biBitCount = 32;
    imageHeader.biCompression = BI_RGB;
    imageHeader.biSizeImage = static_cast<DWORD>(pixels.size());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "Cannot open the fixture BMP output");
    output.write(reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));
    output.write(reinterpret_cast<const char*>(&imageHeader), sizeof(imageHeader));
    output.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    output.close();
    require(static_cast<bool>(output), "Cannot save the complete fixture BMP");
    std::cout << "Rendered native UI fixture: " << path.filename().string() << '\n';
}

// Hand-made synthetic bitmap bytes, decoded by the same WIC path as a
// server cover. The geometric artwork is test data, never a user's media.
std::string syntheticDetailBitmap(int width, int height, bool poster) {
    const std::size_t pixelBytes = static_cast<std::size_t>(width) * height * 4;
    BITMAPFILEHEADER fileHeader{};
    BITMAPINFOHEADER imageHeader{};
    fileHeader.bfType = 0x4d42;
    fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(imageHeader);
    fileHeader.bfSize = fileHeader.bfOffBits + static_cast<DWORD>(pixelBytes);
    imageHeader.biSize = sizeof(imageHeader);
    imageHeader.biWidth = width;
    imageHeader.biHeight = -height;
    imageHeader.biPlanes = 1;
    imageHeader.biBitCount = 32;
    imageHeader.biCompression = BI_RGB;
    imageHeader.biSizeImage = static_cast<DWORD>(pixelBytes);
    std::string bytes(fileHeader.bfSize, '\0');
    std::memcpy(bytes.data(), &fileHeader, sizeof(fileHeader));
    std::memcpy(bytes.data() + sizeof(fileHeader), &imageHeader, sizeof(imageHeader));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float u = static_cast<float>(x) / width;
            const float v = static_cast<float>(y) / height;
            unsigned char red = static_cast<unsigned char>((poster ? 220 : 28) + (poster ? -85 : 45) * v);
            unsigned char green = static_cast<unsigned char>((poster ? 116 : 95) + (poster ? -72 : 30) * u);
            unsigned char blue = static_cast<unsigned char>((poster ? 54 : 170) + (poster ? 22 : 50) * v);
            const float peak = 0.82F - 0.50F * (1.0F - std::min(1.0F, std::abs(u - 0.52F) * 2.8F));
            if (v > peak) {
                red = poster ? 96 : 32;
                green = poster ? 30 : 62;
                blue = poster ? 42 : 112;
            }
            const float sunX = u - (poster ? 0.70F : 0.78F);
            const float sunY = (v - 0.23F) * height / width;
            if (sunX * sunX + sunY * sunY < 0.055F * 0.055F) {
                red = 250; green = 212; blue = 105;
            }
            const std::size_t at = fileHeader.bfOffBits + (static_cast<std::size_t>(y) * width + x) * 4;
            bytes[at] = static_cast<char>(blue);
            bytes[at + 1] = static_cast<char>(green);
            bytes[at + 2] = static_cast<char>(red);
            bytes[at + 3] = static_cast<char>(255);
        }
    }
    return bytes;
}
}

// Existing VideoSource friend seam: ready/length/audio metadata without an
// input file, decoder worker, device or real audio samples.
struct AudioSeekTestAccess {
    static void seed(VideoSource& source, double duration, AudioDecodeState audio = AudioDecodeState::Primed) {
        source.duration_.store(duration);
        source.ready_.store(true);
        source.audioStreamAvailability_.store(1);
        source.setAudioDecodeState(source.audioDecodeStatus().generation, audio);
    }
    static std::uint64_t videoGeneration(const VideoSource& source) { return source.videoSeekGeneration_; }
};

struct AppRegressionTests {
    // Hands the fixture window's WM_CAPTURECHANGED to the App, as the video
    // window's procedure does, so a test sees what ReleaseCapture sends.
    struct CaptureRelay {
        CaptureRelay(HWND window, App& app) : window(window), app(&app) {
            current = this;
            previous = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
                window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&CaptureRelay::proc)));
            require(previous != nullptr, "Cannot subclass the fixture window");
        }
        ~CaptureRelay() {
            SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previous));
            current = nullptr;
        }
        static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
            if (message == WM_CAPTURECHANGED && current && hwnd == current->window) {
                ++current->captureChanges;
                return current->app->handleVideoMessage(message, wParam, lParam);
            }
            return CallWindowProcW(current ? current->previous : DefWindowProcW, hwnd, message, wParam, lParam);
        }
        HWND window{};
        App* app{};
        WNDPROC previous{};
        int captureChanges{};
        static inline CaptureRelay* current{};
    };


    static std::vector<LocalEntry> syntheticLocalQueue(const std::wstring& folder = L"C:\\QuadDeckSynthetic\\Folder A") {
        return {{folder + L"\\01 Synthetic Dawn.mp4", 125'000'000, 1, 120.0},
                {folder + L"\\02 Synthetic Signal.mkv", 250'000'000, 2, 240.0},
                {folder + L"\\03 Synthetic Return.mp4", 375'000'000, 3, 360.0}};
    }

    static std::vector<std::wstring> localQueuePaths(const std::vector<LocalEntry>& queue) {
        std::vector<std::wstring> paths;
        for (const auto& entry : queue) paths.push_back(entry.path);
        return paths;
    }

    static void prepareLocalBrowser(App& app, const HiddenWindow& parent,
                                    const std::vector<LocalEntry>& queue = syntheticLocalQueue()) {
        app.window_ = app.videoWindow_ = parent.window;
        app.instance_ = GetModuleHandleW(nullptr);
        app.deviceRecoveryPending_ = true;
        app.settingsOpen_ = app.embyBrowserOpen_ = true;
        app.settingsAlpha_ = 1.0F;
        app.browserSource_ = App::BrowserSource::Local;
        app.localEntries_ = app.localList_ = queue;
        app.localDirectory_ = queue.empty() ? std::wstring() : localDirectory(queue.front().path);
        app.embyBrowser_.view = 0;
        app.embyBrowser_.sort = static_cast<int>(emby::SortKey::Name);
        app.embyBrowser_.descending = false;
    }

    static void seedLiveLocalPane(App& app, std::size_t pane, const std::vector<LocalEntry>& queue,
                                  std::size_t chosen = 0, bool addedMuted = false) {
        app.paths_[pane] = queue.at(chosen).path;
        app.sources_[pane] = std::make_unique<VideoSource>();
        AudioSeekTestAccess::seed(*app.sources_[pane], queue[chosen].duration);
        App::LocalPanePlayback state;
        state.queue = queue;
        state.addedMuted = addedMuted;
        app.localPanes_[pane] = std::move(state);
        app.subtitleSerial_[pane] = ++app.subtitleSerialCounter_;
    }

    static void externalOpensPreservePlaybackAndDeferInteractions() {
        App app;
        HiddenWindow parent;
        auto queue = syntheticLocalQueue();
        queue.push_back({localDirectory(queue[0].path) + L"\\04 \x65e5\x672c\x8a9e.mp4", 4, 4, 400.0});
        prepareLocalBrowser(app, parent, queue);
        seedLiveLocalPane(app, 0, queue);
        app.audioMask_ = 1U;
        app.clock_.seek(73.0);
        app.clock_.pause();
        app.deviceRecoveryResume_ = false;
        app.syncAdjustments_[0] = 4.0;
        app.startDelays_[0] = 2.0;
        app.playbackRates_[0] = 1.25;
        app.sourcePaused_[0] = true;
        app.sourcePausedTimes_[0] = 13.75;
        app.audio_.setPaneVolume(0, 0.4F);
        auto* original = app.sources_[0].get();
        const auto serial = app.subtitleSerial_[0];
        const auto videoGeneration = original->requestSeek(13.75);
        const auto audioGeneration = original->audioDecodeStatus().generation;
        const auto unchanged = [&] {
            require(app.sources_[0].get() == original && app.subtitleSerial_[0] == serial &&
                    app.paths_[0] == queue[0].path && localQueuePaths(app.localPanes_[0]->queue) == localQueuePaths(queue) &&
                    app.syncAdjustments_[0] == 4.0 && app.startDelays_[0] == 2.0 && app.playbackRates_[0] == 1.25 &&
                    app.sourcePaused_[0] && app.sourcePausedTimes_[0] == 13.75 && app.currentSourceTime(0) == 13.75 &&
                    app.clock_.position() == 73.0 && !app.clock_.isPlaying() && app.audioMask_ == 1U &&
                    std::abs(app.audio_.paneVolume(0) - 0.4F) < 0.0001F &&
                    original->audioDecodeStatus().generation == audioGeneration &&
                    AudioSeekTestAccess::videoGeneration(*original) == videoGeneration,
                    "External Add changed first media serial, master/local timeline, pause, audio, decode tickets or queue");
        };
        app.enqueueExternalFiles({queue[1].path});
        const auto blocked = [&] {
            app.processExternalFiles();
            require(app.paths_[1].empty() && app.externalOpenBatches_.size() == 1 &&
                    app.externalOpenBatches_.front().next == 0, "External request ran during a modal/pressed/drag interaction");
            unchanged();
        };
        app.contextMenuOpen_ = true; blocked(); app.contextMenuOpen_ = false;
        EnableWindow(parent.window, FALSE); blocked(); EnableWindow(parent.window, TRUE);
        app.controlDrag_ = App::ControlDrag::MasterSeek; blocked(); app.controlDrag_ = App::ControlDrag::None;
        app.panelPressArmed_ = true; blocked(); app.panelPressArmed_ = false;
        app.draggingPane_ = true; blocked(); app.draggingPane_ = false;
        app.pressedBarItem_ = 0; blocked(); app.pressedBarItem_ = -1;
        app.pressedChipPane_ = 0; blocked(); app.pressedChipPane_ = -1;
        app.processExternalFiles();
        require(app.paths_[1] == queue[1].path && app.localPanes_[1] && app.localPanes_[1]->addedMuted &&
                app.currentSourceTime(1) == 0.0 && app.externalOpenBatches_.empty(),
                "External Add did not open its own zero-origin muted pane and complete its batch");
        unchanged();
        const auto secondSerial = app.subtitleSerial_[1];
        app.soloPane_ = 0;
        app.expandedPane_ = 0;
        const auto alias = localDirectory(queue[1].path) + L"\\.\\" + localFileName(queue[1].path);
        app.enqueueExternalFiles({alias, queue[1].path});
        app.processExternalFiles();
        require(app.subtitleSerial_[1] == secondSerial && app.paths_[2].empty() &&
                app.embyPlaybackTarget() == 1 && app.soloPane_ == -1 && app.expandedPane_ == -1,
                "External duplicate did not focus its existing pane without reopening or hiding it");
        unchanged();
        app.enqueueExternalFiles({L"C:\\QuadDeckSynthetic\\forwarded.QDECK"});
        app.processExternalFiles();
        require(app.paths_[1] == queue[1].path && app.subtitleSerial_[1] == secondSerial && app.externalOpenBatches_.empty(),
                "Forwarded session file reset a running deck");
        unchanged();

        // F6 Add retains intentional duplicate-file support.
        app.activateLocalItem(1, true);
        require(app.paths_[2] == queue[1].path && app.subtitleSerial_[2] != secondSerial,
                "Explorer deduplication leaked into manual local browser Add");
        unchanged();
    }

    static void externalReplacementKeepsBatchOrderAndCapturedQueue() {
        App app;
        HiddenWindow parent;
        auto queue = syntheticLocalQueue();
        for (unsigned index = 3; index < 8; ++index)
            queue.push_back({localDirectory(queue[0].path) + L"\\Synthetic " + std::to_wstring(index) + L".mp4", index, index, 600.0});
        prepareLocalBrowser(app, parent, queue);
        for (std::size_t pane = 0; pane < kMaxPanes; ++pane) seedLiveLocalPane(app, pane, queue, pane);
        app.audioMask_ = 5U;
        app.clock_.seek(83.0);
        app.clock_.pause();
        app.deviceRecoveryResume_ = false;
        app.settingsScroll_ = 123.0F;
        const auto paths = app.paths_;
        const auto serials = app.subtitleSerial_;
        app.enqueueExternalFiles({queue[5].path, queue[6].path});
        app.processExternalFiles();
        require(app.localPendingPlay_ && app.localPendingPlay_->external && app.localPendingPlay_->path == queue[5].path &&
                app.paths_ == paths && app.subtitleSerial_ == serials && app.audioMask_ == 5U && app.clock_.position() == 83.0,
                "Full-deck external open mutated playback before an explicit choice");
        const auto rows = app.buildLocalReplacementRows(320.0F);
        require(rows[1].wrapNote && rows[1].label.find(L"2 files remain") != std::wstring::npos &&
                rows[1].label.find(L"Later launches stay queued") != std::wstring::npos,
                "External replacement note omitted its remaining batch count or cancel scope");
        // A browser reorder after the chooser appears cannot alter the queue
        // captured for the requested media.
        app.embyBrowser_.sort = static_cast<int>(emby::SortKey::Size);
        app.embyBrowser_.descending = true;
        app.orderLocalList();
        app.localConfirmReplacement(4);
        require(app.paths_[4] == queue[5].path && localQueuePaths(app.localPanes_[4]->queue) == localQueuePaths(queue) &&
                app.audioMask_ == 5U && app.clock_.position() == 83.0 && !app.clock_.isPlaying(),
                "External chooser confirmation recaptured reordered browser state or changed transport/audio");
        app.processExternalFiles();
        require(app.localPendingPlay_ && app.localPendingPlay_->path == queue[6].path,
                "Confirmed external replacement dropped the rest of its batch");
        const auto replaced = app.paths_[4];
        ++app.subtitleSerial_[4]; // Media changed after the snapshot.
        app.localConfirmReplacement(4);
        require(app.paths_[4] == replaced && !app.localPendingPlay_ && app.externalOpenBatches_.front().next == 1,
                "Stale external chooser overwrote changed media or silently consumed the request");
        app.processExternalFiles();
        require(app.localPendingPlay_ && app.localPendingPlay_->path == queue[6].path &&
                app.localPendingPlay_->mediaSerials[4] == app.subtitleSerial_[4],
                "Stale external chooser did not re-prompt using the current pane snapshot");
        app.enqueueExternalFiles({queue[7].path});
        app.localCancelReplacement();
        require(app.externalOpenBatches_.size() == 1 && app.externalOpenBatches_.front().files[0] == queue[7].path &&
                app.settingsScroll_ == 123.0F && app.paths_[4] == replaced,
                "Cancel did not discard only its current launch and restore the earlier sheet");
        app.processExternalFiles();
        require(app.localPendingPlay_ && app.localPendingPlay_->path == queue[7].path,
                "Cancel discarded the later independent launch");
        app.closeSettingsPanel();
        require(!app.localPendingPlay_ && app.externalOpenBatches_.empty() && app.paths_[4] == replaced,
                "Closing an external replacement sheet changed playback or left its launch queued");

        // An existing manual chooser keeps ownership until it is dismissed.
        app.settingsOpen_ = app.embyBrowserOpen_ = true;
        app.activateLocalItem(0, true);
        require(app.localPendingPlay_ && !app.localPendingPlay_->external, "Cannot prepare manual full-deck chooser");
        const auto manualPath = app.localPendingPlay_->path;
        app.enqueueExternalFiles({queue[6].path});
        app.processExternalFiles();
        require(app.localPendingPlay_->path == manualPath && !app.localPendingPlay_->external,
                "External launch stole an existing manual chooser");
        app.localCancelReplacement();
        require(app.externalOpenBatches_.size() == 1, "Manual cancel discarded an independent external launch");
        app.processExternalFiles();
        require(app.localPendingPlay_ && app.localPendingPlay_->external && app.localPendingPlay_->path == queue[6].path,
                "External launch did not resume after manual chooser cancellation");
        app.localCancelReplacement();

        // An uncached folder is enumerated off the window thread using the
        // ordering captured when its chooser appeared, even if F6 changes.
        std::wstring temporaryPath(32768, L'\0');
        const DWORD temporaryLength = GetTempPathW(static_cast<DWORD>(temporaryPath.size()), temporaryPath.data());
        require(temporaryLength && temporaryLength < temporaryPath.size(), "Cannot find isolated queue fixture directory");
        temporaryPath.resize(temporaryLength);
        const auto folder = std::filesystem::path(temporaryPath) /
            (L"QuadDeck-external-order-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        require(std::filesystem::create_directory(folder), "Cannot create isolated uncached queue fixture");
        struct FolderLifetime {
            std::filesystem::path value;
            ~FolderLifetime() { std::error_code ignored; std::filesystem::remove_all(value, ignored); }
        } folderLifetime{folder};
        const auto dawn = folder / L"01 \x65e5\x672c\x8a9e Dawn.mp4";
        const auto returnFile = folder / L"02 Return.mp4";
        { std::ofstream file(dawn, std::ios::binary); file << "synthetic"; }
        { std::ofstream file(returnFile, std::ios::binary); file << "synthetic longer placeholder"; }
        app.embyBrowser_.sort = static_cast<int>(emby::SortKey::Name);
        app.embyBrowser_.descending = false;
        app.enqueueExternalFiles({dawn.wstring()});
        app.processExternalFiles();
        require(app.localPendingPlay_ && app.localPendingPlay_->queueNeedsLoad &&
                app.localPendingPlay_->queueSort == emby::SortKey::Name && !app.localPendingPlay_->queueDescending,
                "Uncached chooser did not capture its initial folder ordering");
        app.embyBrowser_.sort = static_cast<int>(emby::SortKey::Size);
        app.embyBrowser_.descending = true;
        app.localConfirmReplacement(4);
        for (unsigned attempt = 0; attempt < 300 && app.localPanes_[4]->queueLoadSerial; ++attempt) {
            app.mainQueue_->drain();
            Sleep(10);
        }
        require(!app.localPanes_[4]->queueLoadSerial &&
                localQueuePaths(app.localPanes_[4]->queue) == std::vector<std::wstring>{dawn.wstring(), returnFile.wstring()},
                "Late uncached folder enumeration adopted the changed browser ordering");
        app.processExternalFiles();
    }

    static void externalLaunchIntoAnEmptyDeckOpensOneDeck() {
        App app;
        HiddenWindow parent;
        auto queue = syntheticLocalQueue();
        for (unsigned index = 4; index < 8; ++index)
            queue.push_back({localDirectory(queue[0].path) + L"\\0" + std::to_wstring(index) + L" Synthetic.mp4",
                             index, index, 600.0});
        prepareLocalBrowser(app, parent, queue);
        app.seekMode_ = SeekMode::Linked;
        app.audioMask_ = 3U;
        app.deviceRecoveryResume_ = false;
        std::vector<std::wstring> files;
        for (const auto& entry : queue) files.push_back(entry.path);
        files.insert(files.begin() + 1, L"C:\\QuadDeckSynthetic\\saved.qdeck");
        // Several files given to one launch, forwarded into a running but
        // empty window: one compared deck, not muted Adds.
        SingleInstance::RequestId id{};
        id[0] = 1;
        app.enqueueExternalFiles(files, id);
        app.processExternalFiles();
        for (std::size_t pane = 0; pane < kMaxPanes; ++pane) {
            require(app.paths_[pane] == queue[pane].path && !app.localPanes_[pane],
                    "An empty deck did not open the launch's first five videos as one deck");
        }
        require(app.activeSeekMode() == SeekMode::Linked && !app.perPaneTimelines() &&
                app.audioMask_ == 3U && app.deviceRecoveryResume_ && app.externalOpenBatches_.empty() &&
                !app.localPendingPlay_,
                "An empty-deck launch lost the saved seek mode or audio panes, did not start, or stayed queued");
        // Once the deck shows something, a later launch is Add again.
        app.enqueueExternalFiles({queue[5].path});
        app.processExternalFiles();
        require(app.localPendingPlay_ && app.localPendingPlay_->external && app.paths_[0] == queue[0].path,
                "A launch into a full deck did not ask which pane to replace");
        app.localCancelReplacement();
        require(app.externalOpenBatches_.empty() && app.paths_[4] == queue[4].path,
                "Cancelling the replacement changed the deck or kept the launch queued");
    }

    // A subtitle file opened from Explorer: on the video it belongs to, or
    // on the pane subtitle keys act on; into an empty deck, with the video
    // beside it that carries its name, found off the window thread. One
    // whose video is on its way -- later in its launch, or in a launch that
    // arrives shortly after (Explorer starts one per selected file) -- waits
    // for that video.
    static void externalSubtitleFilesFindTheirVideo() {
        const auto isFile = [](const App& app, std::size_t pane, const std::wstring& file) {
            return app.subtitleSelection_[pane].kind == App::SubtitleKind::File &&
                   app.subtitleSelection_[pane].file == std::filesystem::path(file) &&
                   app.subtitleChosenByViewer_[pane];
        };
        // Launches queued long enough ago that no video is still expected.
        const auto aged = [](App& app) {
            for (auto& batch : app.externalOpenBatches_) batch.arrived -= 60'000;
        };
        {
            // A deck that shows videos: the subtitles go on theirs, muted Add
            // and the replacement chooser stay out of it.
            App app;
            HiddenWindow parent;
            const auto queue = syntheticLocalQueue();
            prepareLocalBrowser(app, parent, queue);
            seedLiveLocalPane(app, 0, queue, 0);
            seedLiveLocalPane(app, 1, queue, 1);
            const auto paths = app.paths_;
            const std::wstring own = localDirectory(queue[1].path) + L"\\02 synthetic signal.CHS.ass";
            app.enqueueExternalFiles({own});
            app.processExternalFiles();
            require(isFile(app, 1, own) && app.subtitleSelection_[0].kind == App::SubtitleKind::None &&
                    app.paths_ == paths && !app.localPendingPlay_ && app.externalOpenBatches_.empty(),
                    "An opened subtitle file did not go on the video it belongs to");
            // Of no open video: it waits a moment for a launch bringing
            // one, then goes on the pane subtitle keys act on.
            const int target = app.subtitlePane();
            require(target >= 0, "No pane for subtitle keys");
            const auto held = app.subtitleSelection_;
            const std::wstring other = L"C:\\QuadDeckSynthetic\\Elsewhere\\02 Synthetic Signal.srt";
            app.enqueueExternalFiles({other});
            app.processExternalFiles();
            require(app.subtitleSelection_ == held && app.externalOpenBatches_.size() == 1 &&
                    app.externalOpenBatches_.front().next == 0,
                    "A subtitle file of no open video did not wait for its video's launch");
            aged(app);
            app.processExternalFiles();
            require(isFile(app, static_cast<std::size_t>(target), other) && app.paths_ == paths &&
                    app.externalOpenBatches_.empty(),
                    "A subtitle file of no open video did not go on the pane subtitle keys act on");
            // Ahead of its own video in one launch, or in the launch before
            // the video's: it goes on that video, not on the playing ones.
            const auto before0 = app.subtitleSelection_[0];
            const auto before1 = app.subtitleSelection_[1];
            const std::wstring third = localDirectory(queue[2].path) + L"\\03 Synthetic Return.ass";
            app.enqueueExternalFiles({third, queue[2].path});
            app.processExternalFiles();
            require(app.paths_[2] == queue[2].path && isFile(app, 2, third) &&
                    app.subtitleSelection_[0] == before0 && app.subtitleSelection_[1] == before1 &&
                    app.externalOpenBatches_.empty(),
                    "A subtitle ahead of its video in one launch did not wait for that video");
            const std::wstring late = localDirectory(queue[0].path) + L"\\04 Synthetic Late.mp4";
            const std::wstring lateSubtitle = localDirectory(queue[0].path) + L"\\04 synthetic late.en.srt";
            app.enqueueExternalFiles({lateSubtitle});
            app.processExternalFiles();
            require(app.paths_[3].empty() && app.externalOpenBatches_.size() == 1 &&
                    app.subtitleSelection_[0] == before0 && app.subtitleSelection_[1] == before1,
                    "A subtitle launched just before its video did not wait for it");
            app.enqueueExternalFiles({late});
            app.processExternalFiles();
            require(app.paths_[3] == late && isFile(app, 3, lateSubtitle) &&
                    app.subtitleSelection_[0] == before0 && app.subtitleSelection_[1] == before1 &&
                    isFile(app, 2, third) && app.externalOpenBatches_.empty(),
                    "A subtitle launched just before its video did not go on it when it came");
        }
        {
            // A better name still to come wins over a shorter one open, and
            // several subtitles waiting for one video keep their order.
            App app;
            HiddenWindow parent;
            const std::wstring folder = L"C:\\QuadDeckSynthetic\\Folder B";
            const std::vector<LocalEntry> queue{{folder + L"\\Show.mkv", 1, 1, 600.0}};
            prepareLocalBrowser(app, parent, queue);
            seedLiveLocalPane(app, 0, queue, 0);
            const std::wstring episode = folder + L"\\Show.S01E01.mkv";
            const std::wstring chs = folder + L"\\Show.S01E01.chs.ass";
            const std::wstring eng = folder + L"\\Show.S01E01.eng.ass";
            app.enqueueExternalFiles({chs, eng, episode});
            app.processExternalFiles();
            require(app.paths_[1] == episode && isFile(app, 1, eng) && app.subtitleFiles_[1].size() == 2 &&
                    app.subtitleFiles_[1][0] == std::filesystem::path(chs) &&
                    app.subtitleSelection_[0].kind == App::SubtitleKind::None && app.externalOpenBatches_.empty(),
                    "Subtitles of a queued episode went on the open show, or lost their order");
            // With that episode open, the show's own subtitles still go on it.
            const std::wstring showSubtitle = folder + L"\\Show.ass";
            app.enqueueExternalFiles({showSubtitle});
            app.processExternalFiles();
            require(isFile(app, 0, showSubtitle) && isFile(app, 1, eng) && app.externalOpenBatches_.empty(),
                    "A subtitle of an open video waited or went elsewhere");
            // Only the show fits it, but its episode may be a launch behind:
            // it waits, and the episode takes it.
            const std::wstring second = folder + L"\\Show.S01E02.chs.ass";
            app.enqueueExternalFiles({second});
            app.processExternalFiles();
            require(isFile(app, 0, showSubtitle) && app.paths_[2].empty() && app.externalOpenBatches_.size() == 1,
                    "A subtitle with a better name possible than the open one did not wait");
            app.enqueueExternalFiles({folder + L"\\Show.S01E02.mkv"});
            app.processExternalFiles();
            require(app.paths_[2] == folder + L"\\Show.S01E02.mkv" && isFile(app, 2, second) &&
                    isFile(app, 0, showSubtitle) && app.externalOpenBatches_.empty(),
                    "An episode launched after its subtitle did not take it from the show");
            // A subtitle ahead of its video and one after it in one launch,
            // and one launch ahead of a video launched with another: in the
            // order they came, the last shown.
            const std::wstring movie = folder + L"\\Movie.mkv";
            const std::wstring movieChs = folder + L"\\Movie.chs.ass";
            const std::wstring movieEng = folder + L"\\Movie.eng.ass";
            app.enqueueExternalFiles({movieChs, movie, movieEng});
            app.processExternalFiles();
            require(app.paths_[3] == movie && isFile(app, 3, movieEng) && app.subtitleFiles_[3].size() == 2 &&
                    app.subtitleFiles_[3][0] == std::filesystem::path(movieChs) && app.externalOpenBatches_.empty(),
                    "Subtitles around their video in one launch lost their order");
            const std::wstring film = folder + L"\\Film.mkv";
            const std::wstring filmChs = folder + L"\\Film.chs.ass";
            const std::wstring filmEng = folder + L"\\Film.eng.ass";
            app.enqueueExternalFiles({filmChs});
            app.enqueueExternalFiles({film, filmEng});
            app.processExternalFiles();
            require(app.paths_[4] == film && isFile(app, 4, filmEng) && app.subtitleFiles_[4].size() == 2 &&
                    app.subtitleFiles_[4][0] == std::filesystem::path(filmChs) && app.externalOpenBatches_.empty(),
                    "A subtitle launched ahead of its video's launch came after that launch's own");
        }
        {
            // Into an empty deck, a launch of another video and a subtitle
            // whose own video is a launch behind: the subtitle waits for it
            // rather than going on the deck's video.
            App app;
            HiddenWindow parent;
            prepareLocalBrowser(app, parent, {});
            const std::wstring folder = L"C:\\QuadDeckSynthetic\\Folder C";
            app.enqueueExternalFiles({folder + L"\\Other.mkv", folder + L"\\Ep.chs.ass"});
            app.enqueueExternalFiles({folder + L"\\Ep.mkv"});
            app.processExternalFiles();
            require(app.paths_[0] == folder + L"\\Other.mkv" && app.paths_[1] == folder + L"\\Ep.mkv" &&
                    app.subtitleSelection_[0].kind == App::SubtitleKind::None &&
                    isFile(app, 1, folder + L"\\Ep.chs.ass") && app.externalOpenBatches_.empty(),
                    "A subtitle opened with another video into an empty deck did not wait for its own");
            // One launch ahead of its video's, which brings another of its
            // own, into an empty deck: in the order they came, as above.
            App empty;
            HiddenWindow emptyParent;
            prepareLocalBrowser(empty, emptyParent, {});
            const std::wstring film = folder + L"\\Film.mkv";
            const std::wstring filmChs = folder + L"\\Film.chs.ass";
            const std::wstring filmEng = folder + L"\\Film.eng.ass";
            empty.enqueueExternalFiles({filmChs});
            empty.enqueueExternalFiles({filmEng, film});
            empty.processExternalFiles();
            require(empty.paths_[0] == film && isFile(empty, 0, filmEng) && empty.subtitleFiles_[0].size() == 2 &&
                    empty.subtitleFiles_[0][0] == std::filesystem::path(filmChs) &&
                    empty.externalOpenBatches_.empty(),
                    "Into an empty deck, a subtitle from an earlier launch came after its video's launch's own");
        }
        {
            // The pane subtitle keys act on is an item still resolving: it
            // has nothing to show them on, no other pane takes them, and the
            // player says so.
            App app;
            HiddenWindow parent;
            const auto queue = syntheticLocalQueue();
            prepareLocalBrowser(app, parent, queue);
            seedLiveLocalPane(app, 0, queue, 0);
            app.deviceRecoveryPending_ = false;
            app.paths_[1] = L"C:\\QuadDeckSynthetic\\Folder A\\Still opening.mkv";
            app.embySelectTarget(1);
            require(app.paneLogicallyLoaded(0) && !app.paneLogicallyLoaded(1) && app.subtitlePane() == 1,
                    "Cannot stage a selected pane that is still opening");
            app.enqueueExternalFiles({L"C:\\QuadDeckSynthetic\\Elsewhere\\x.ass"});
            aged(app);
            app.processExternalFiles();
            require(app.subtitleSelection_[0].kind == App::SubtitleKind::None &&
                    app.subtitleSelection_[1].kind == App::SubtitleKind::None && app.externalOpenBatches_.empty() &&
                    app.noticeText_.find(L"still opening") != std::wstring::npos,
                    "A subtitle file for a pane still opening went elsewhere or was dropped without a word");
        }
        {
            // A launch of a video and its subtitles into an empty deck.
            App app;
            HiddenWindow parent;
            prepareLocalBrowser(app, parent, {});
            app.deviceRecoveryResume_ = false;
            const auto queue = syntheticLocalQueue();
            const std::wstring subtitle = localDirectory(queue[0].path) + L"\\01 Synthetic Dawn.srt";
            app.enqueueExternalFiles({subtitle, queue[0].path});
            app.processExternalFiles();
            require(app.paths_[0] == queue[0].path && app.paths_[1].empty() && isFile(app, 0, subtitle) &&
                    app.deviceRecoveryResume_ && app.externalOpenBatches_.empty() && !app.externalSubtitleLookup_,
                    "A video launched with its subtitles did not open with them");
        }

        std::wstring temporaryPath(32768, L'\0');
        const DWORD temporaryLength = GetTempPathW(static_cast<DWORD>(temporaryPath.size()), temporaryPath.data());
        require(temporaryLength && temporaryLength < temporaryPath.size(), "Cannot find the fixture directory");
        temporaryPath.resize(temporaryLength);
        const auto folder = std::filesystem::path(temporaryPath) /
            (L"QuadDeck-external-subtitle-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(GetTickCount64()));
        require(std::filesystem::create_directory(folder), "Cannot create the fixture directory");
        struct FolderLifetime {
            std::filesystem::path value;
            ~FolderLifetime() { std::error_code ignored; std::filesystem::remove_all(value, ignored); }
        } folderLifetime{folder};
        for (const wchar_t* name : {L"Clip.mkv", L"Clip One.mkv", L"Clip One.mp4", L"Unrelated.mp4"}) {
            std::ofstream file(folder / name, std::ios::binary);
            file << "synthetic";
        }
        const auto subtitle = (folder / L"Clip One.chs.srt").wstring();
        {
            std::ofstream srt(subtitle, std::ios::binary);
            srt << "1\r\n00:00:01,000 --> 00:00:02,000\r\nHello\r\n";
        }
        const auto settle = [](App& app, const std::function<bool()>& done) {
            for (int attempt = 0; attempt < 300 && !done(); ++attempt) {
                app.mainQueue_->drain();
                app.processExternalFiles();
                Sleep(10);
            }
        };
        {
            // Opened alone into an empty deck: the folder is read off this
            // thread, later launches wait, and the video opens with them.
            App app;
            HiddenWindow parent;
            prepareLocalBrowser(app, parent, {});
            app.deviceRecoveryResume_ = false;
            const std::wstring later = (folder / L"Unrelated.mp4").wstring();
            app.enqueueExternalFiles({subtitle});
            app.enqueueExternalFiles({later});
            aged(app);
            app.processExternalFiles();
            require(app.externalSubtitleLookup_ && app.paths_[0].empty() && app.externalOpenBatches_.size() == 2,
                    "A subtitle file's folder was not read off the window thread, or a later launch overtook it");
            settle(app, [&] { return app.externalOpenBatches_.empty(); });
            require(app.paths_[0] == (folder / L"Clip One.mkv").wstring() && isFile(app, 0, subtitle) &&
                    app.deviceRecoveryResume_ && !app.externalSubtitleLookup_,
                    "A subtitle file into an empty deck did not open its video with it");
            require(app.paths_[1] == later && app.localPanes_[1] && app.localPanes_[1]->addedMuted,
                    "The launch behind a subtitle file was not added after it");
            settle(app, [&] { return app.subtitles_[0] != nullptr; });
            require(app.subtitles_[0] && subtitleTextAt(*app.subtitles_[0], 1.5) == L"Hello",
                    "The subtitle file opened with its video was not read");
        }
        {
            // Into an empty deck with the video's own launch queued behind
            // it: that video, not the folder's first of the name.
            App app;
            HiddenWindow parent;
            prepareLocalBrowser(app, parent, {});
            const std::wstring chosen = (folder / L"Clip One.mp4").wstring();
            app.enqueueExternalFiles({subtitle});
            app.enqueueExternalFiles({chosen});
            app.processExternalFiles();
            require(app.paths_[0] == chosen && app.paths_[1].empty() && isFile(app, 0, subtitle) &&
                    app.externalOpenBatches_.empty() && !app.externalSubtitleLookup_,
                    "A subtitle launched into an empty deck ahead of its video did not open with that video");
            // And when that launch arrives while the folder is being read.
            App reading;
            HiddenWindow readingParent;
            prepareLocalBrowser(reading, readingParent, {});
            reading.enqueueExternalFiles({subtitle});
            aged(reading);
            reading.processExternalFiles();
            require(reading.externalSubtitleLookup_, "The folder read did not start");
            reading.enqueueExternalFiles({chosen});
            settle(reading, [&] { return reading.externalOpenBatches_.empty(); });
            require(reading.paths_[0] == chosen && reading.paths_[1].empty() && isFile(reading, 0, subtitle),
                    "A video launched during the folder read was not the one opened with the subtitle");
        }
        {
            // No video carries its name: the deck stays empty and says so.
            App app;
            HiddenWindow parent;
            prepareLocalBrowser(app, parent, {});
            const auto lonely = (folder / L"Lonely.ass").wstring();
            { std::ofstream file(lonely, std::ios::binary); file << "synthetic"; }
            app.enqueueExternalFiles({lonely});
            aged(app);
            app.processExternalFiles();
            settle(app, [&] { return app.externalOpenBatches_.empty(); });
            require(app.externalOpenBatches_.empty() && !app.anyPaneLoaded() && !app.externalSubtitleLookup_ &&
                    app.noticeText_.find(L"Lonely.ass") != std::wstring::npos,
                    "A subtitle file without a video did not say so, or opened something");
        }
        {
            // A video opened while the folder was read takes the subtitles.
            App app;
            HiddenWindow parent;
            prepareLocalBrowser(app, parent, {});
            app.enqueueExternalFiles({subtitle});
            aged(app);
            app.processExternalFiles();
            require(app.externalSubtitleLookup_, "The folder read did not start");
            const std::wstring opened = (folder / L"Clip.mkv").wstring();
            app.loadFiles({opened});
            settle(app, [&] { return app.externalOpenBatches_.empty(); });
            require(app.paths_[0] == opened && app.paths_[1].empty() && isFile(app, 0, subtitle),
                    "A video opened during the folder read did not take the subtitles");
        }
    }

    static int receiveExternalProcess(SingleInstance& instance, const SingleInstance::Request& initial,
                                      const std::wstring& testNamespace, const std::filesystem::path& directory) {
        const HWND window = createSingleInstanceTestWindow(testNamespace);
        struct WindowLifetime { HWND value; ~WindowLifetime() { DestroyWindow(value); } } windowLifetime{window};
        App app;
        app.window_ = app.videoWindow_ = window;
        app.instance_ = GetModuleHandleW(nullptr);
        app.singleInstance_ = &instance;
        app.deviceRecoveryPending_ = true;
        app.localList_ = app.localEntries_ = App::scanLocalFolder(localDirectory(initial.files.at(0)));
        app.localDirectory_ = localDirectory(initial.files[0]);
        app.embyBrowser_.sort = static_cast<int>(emby::SortKey::Name);
        app.enqueueExternalFiles(initial.files, {}, true);
        app.processExternalFiles();
        const auto seedDeferredSources = [&] {
            for (std::size_t pane = 0; pane < kMaxPanes; ++pane) {
                if (app.paths_[pane].empty() || app.sources_[pane]) continue;
                app.sources_[pane] = std::make_unique<VideoSource>();
                AudioSeekTestAccess::seed(*app.sources_[pane], 600.0);
            }
        };
        seedDeferredSources();
        // A fresh launch opens as an ordinary deck, exactly as the command
        // line did before single-instance; the first forwarded Add adopts it
        // into a folder queue without touching its source or timing.
        require(!app.paths_[0].empty() && !app.localPanes_[0] && app.deviceRecoveryResume_,
                "Cross-process receiver did not open and start primary startup media as a deck");
        app.clock_.seek(73.0);
        app.clock_.pause();
        app.deviceRecoveryResume_ = false;
        app.syncAdjustments_[0] = 4.0;
        app.startDelays_[0] = 2.0;
        app.playbackRates_[0] = 1.25;
        app.sourcePaused_[0] = true;
        app.sourcePausedTimes_[0] = 13.75;
        app.audio_.setPaneVolume(0, 0.4F);
        const auto originalPath = app.paths_[0];
        auto* original = app.sources_[0].get();
        const auto originalSerial = app.subtitleSerial_[0];
        const auto originalQueue = localQueuePaths(app.localList_);
        const auto videoGeneration = original->requestSeek(13.75);
        const auto audioGeneration = original->audioDecodeStatus().generation;
        unsigned received = 0, activations = 0, command = 0;
        bool iconic = false;
        std::set<SingleInstance::RequestId> seen;
        nlohmann::json receivedFiles = nlohmann::json::array();
        for (const auto& file : initial.files) receivedFiles.push_back(wideToUtf8Text(file));
        app.externalActivationOverride_ = [&] { ++activations; iconic = false; };
        instance.setWindow(window);
        const ULONGLONG deadline = GetTickCount64() + 60000;
        try {
            bool running = true;
            while (running && GetTickCount64() < deadline) {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    if (message.message == kSingleInstanceTestControl) {
                        command = static_cast<unsigned>(message.lParam);
                        switch (static_cast<SingleInstanceTestControl>(message.wParam)) {
                        case SingleInstanceTestControl::Stop: running = false; break;
                        case SingleInstanceTestControl::Cancel: app.localCancelReplacement(); break;
                        case SingleInstanceTestControl::ReplaceLast: app.localConfirmReplacement(kMaxPanes - 1); break;
                        case SingleInstanceTestControl::Minimize: iconic = true; break;
                        case SingleInstanceTestControl::CloseSecond: app.closePane(1); break;
                        case SingleInstanceTestControl::StopAccepting: instance.stopAccepting(); break;
                        case SingleInstanceTestControl::AbruptExit: ExitProcess(0); break;
                        }
                    } else { TranslateMessage(&message); DispatchMessageW(&message); }
                }
                if (!running) break;
                app.drainExternalRequests();
                for (const auto& batch : app.externalOpenBatches_) {
                    if (batch.id == SingleInstance::RequestId{} || !seen.insert(batch.id).second) continue;
                    ++received;
                    for (const auto& file : batch.files) receivedFiles.push_back(wideToUtf8Text(file));
                }
                app.processExternalFiles();
                seedDeferredSources();
                const bool preserved = app.sources_[0].get() == original && app.paths_[0] == originalPath &&
                    app.subtitleSerial_[0] == originalSerial &&
                    (!app.localPanes_[0] || localQueuePaths(app.localPanes_[0]->queue) == originalQueue) &&
                    app.clock_.position() == 73.0 &&
                    !app.clock_.isPlaying() && app.audioMask_ == 1U && app.sourcePaused_[0] &&
                    app.sourcePausedTimes_[0] == 13.75 && app.currentSourceTime(0) == 13.75 &&
                    app.syncAdjustments_[0] == 4.0 && app.startDelays_[0] == 2.0 && app.playbackRates_[0] == 1.25 &&
                    std::abs(app.audio_.paneVolume(0) - 0.4F) < 0.0001F &&
                    original->audioDecodeStatus().generation == audioGeneration &&
                    AudioSeekTestAccess::videoGeneration(*original) == videoGeneration;
                require(preserved, "Cross-process Add changed first source serial, timeline, audio, pause, decode tickets or queue");
                nlohmann::json state{{"pid", GetCurrentProcessId()}, {"received", received}, {"activations", activations},
                    {"command", command}, {"preserved", preserved}, {"iconic", iconic}, {"visible", IsWindowVisible(window) != FALSE},
                    {"playing", app.clock_.isPlaying()}, {"audioMask", app.audioMask_}, {"queued", app.externalOpenBatches_.size()},
                    {"pending", app.localPendingPlay_.has_value()}, {"pendingPath", app.localPendingPlay_ ? wideToUtf8Text(app.localPendingPlay_->path) : ""},
                    {"receivedFiles", receivedFiles}, {"paths", nlohmann::json::array()}, {"serials", nlohmann::json::array()},
                    {"addedMuted", nlohmann::json::array()}, {"sourceTimes", nlohmann::json::array()}};
                unsigned panes = 0;
                for (std::size_t pane = 0; pane < kMaxPanes; ++pane) {
                    if (!app.paths_[pane].empty()) ++panes;
                    state["paths"].push_back(wideToUtf8Text(app.paths_[pane]));
                    state["serials"].push_back(app.subtitleSerial_[pane]);
                    state["addedMuted"].push_back(app.localPanes_[pane] && app.localPanes_[pane]->addedMuted);
                    state["sourceTimes"].push_back(app.currentSourceTime(pane));
                }
                state["panes"] = panes;
                writeSingleInstanceTestState(directory, state);
                Sleep(10);
            }
            require(!running, "Isolated App receiver lifetime timed out");
        } catch (const std::exception& error) {
            writeSingleInstanceTestState(directory, nlohmann::json{{"failure", error.what()}});
            throw;
        }
        instance.stopAccepting();
        app.singleInstance_ = nullptr;
        KillTimer(window, app_internal::kSettingsSaveTimer);
        app.settingsSavePending_ = false;
        return 0;
    }

    static const emby::PlaybackQueue& playbackQueue(const App& app, std::size_t pane = 0) {
        static const emby::PlaybackQueue empty;
        return app.embyPanes_[pane] ? app.embyPanes_[pane]->queue : empty;
    }

    // Explicit fixture setup; observing an empty pane never creates one.
    static emby::PlaybackQueue& mutablePlaybackQueue(App& app, std::size_t pane = 0) {
        if (!app.embyPanes_[pane]) {
            app.embyPanes_[pane].emplace();
            bindEmbyAccount(app, *app.embyPanes_[pane]);
        }
        return app.embyPanes_[pane]->queue;
    }

    static void bindEmbyAccount(const App& app, App::EmbyPane& pane) {
        const auto& session = app.emby_.session();
        pane.accountSerial = app.embyAccountSerial_;
        pane.serverId = session.serverId;
        pane.serverUrl = session.serverUrl;
        pane.userId = session.userId;
    }

    static void simulateEmbyEnd(App& app, std::size_t pane = 0) {
        // Represents a newly presented source reaching its end. The latch
        // regression separately calls embyFinishPane twice without resetting.
        app.embyPanes_[pane]->endHandled = false;
        app.sourcePaused_[pane] = false;
        app.embyFinishPane(pane);
    }

    // Set QUADDECK_EMBY_FIXTURE_DIR to run only this explicit visual probe.
    // Ordinary CTest remains a geometry/state test without a graphics device.
    static bool renderEmbyFixturesWhenRequested() {
        std::wstring destination(32768, L'\0');
        const DWORD size = GetEnvironmentVariableW(L"QUADDECK_EMBY_FIXTURE_DIR", destination.data(),
                                                  static_cast<DWORD>(destination.size()));
        if (size == 0) return false;
        require(size < destination.size(), "The fixture output directory is too long");
        destination.resize(size);
        const std::filesystem::path directory(destination);
        require(directory.is_absolute(), "The fixture output directory must be absolute");
        std::error_code code;
        std::filesystem::create_directories(directory, code);
        require(!code, "Cannot create the requested fixture output directory");
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        require(SUCCEEDED(com) || com == RPC_E_CHANGED_MODE, "Cannot initialize COM for the UI fixture");
        struct ComLifetime {
            bool initialized;
            ~ComLifetime() { if (initialized) CoUninitialize(); }
        } comLifetime{SUCCEEDED(com)};
        App app;
        emby::Session session;
        session.serverUrl = "synthetic fixture address";
        session.serverId = "fixture-server";
        session.userId = "fixture-user";
        session.token = "fixture-token";
        session.deviceId = "fixture-device";
        app.embyConfigure(session);
        app.embyPages_.push_back(App::EmbyPage{});
        App::EmbyPage page;
        page.kind = App::EmbyPage::Kind::Search;
        page.term = "Aurora";
        app.embyPages_.push_back(page);
        const auto items = emby::parseItems(R"({"Items":[
            {"Id":"movie","Name":"Aurora","Type":"Movie","ProductionYear":2020,"RunTimeTicks":72000000000,
             "UserData":{"PlaybackPositionTicks":18000000000}},
            {"Id":"series","Name":"Aurora","Type":"Series","IsFolder":true,"ProductionYear":2021,
             "UserData":{"UnplayedItemCount":7}},
            {"Id":"episode","Name":"The Long Return","Type":"Episode","SeriesName":"Star Trek: The Next Generation","ParentIndexNumber":2,
             "IndexNumber":3,"IndexNumberEnd":4,"RunTimeTicks":18000000000,
             "UserData":{"PlaybackPositionTicks":4500000000}},
            {"Id":"season","Name":"","Type":"Season","IndexNumber":2,"ChildCount":8,
             "UserData":{"UnplayedItemCount":3}},
            {"Id":"watched","Name":"Completed episode","Type":"Episode","SeriesName":"Star Trek: The Next Generation","ParentIndexNumber":1,
             "IndexNumber":12,"RunTimeTicks":18000000000,"UserData":{"Played":true}},
            {"Id":"legacy","Name":"Legacy film","Type":"Movie","ProductionYear":null},
            {"Id":"missing","Name":"","Type":"Season"}
        ],"TotalRecordCount":7})");
        require(items && items->items.size() == 7, "Cannot parse the rendering fixture");
        app.embyItems_ = items->items;
        App::EmbyPane playing;
        playing.itemId = "episode";
        bindEmbyAccount(app, playing);
        app.embyPanes_[0] = playing;
        struct Fixture {
            const wchar_t* name;
            int view;
            float scale;
            bool docked;
            bool season;
        };
        const Fixture fixtures[]{
            {L"full-list-1x.bmp", 0, 1.0F, false, false},
            {L"full-posters-1x.bmp", 1, 1.0F, false, false},
            {L"full-thumbnails-2x.bmp", 2, 2.0F, false, false},
            {L"dock-list-320dip-1x.bmp", 0, 1.0F, true, false},
            {L"dock-posters-320dip-1x.bmp", 1, 1.0F, true, false},
            {L"dock-thumbnails-320dip-2x.bmp", 2, 2.0F, true, false},
            {L"season-list-1x.bmp", 0, 1.0F, false, true},
            {L"season-posters-2x.bmp", 1, 2.0F, false, true}
        };
        for (const auto& fixture : fixtures) {
            app.embyBrowser_.view = fixture.view;
            app.uiScale_ = fixture.scale;
            app.embyPages_.back().kind = fixture.season ? App::EmbyPage::Kind::Season : App::EmbyPage::Kind::Search;
            app.embyPages_.back().title = "Aurora / Season 2";
            const float width = 1280.0F * fixture.scale;
            const float height = 1000.0F * fixture.scale;
            const float sheetWidth = (fixture.docked ? 320.0F : 1280.0F) * fixture.scale;
            OverlayScene scene;
            scene.width = width;
            scene.height = height;
            scene.scale = fixture.scale;
            scene.panel.alpha = 1.0F;
            scene.panel.title = L"Emby";
            scene.panel.rows = app.buildEmbyRows(sheetWidth);
            scene.panel.layout = settingsPanelLayout(width, height, fixture.scale, scene.panel.rows,
                                                     0.0F, 1.0F, {}, sheetWidth);
            renderFixtureBmp(scene, directory / fixture.name);
        }
        renderEmbyDetailFixtures(directory);
        renderEmbyMultiPaneFixtures(directory);
        return true;
    }

    static void renderEmbyDetailFixtures(const std::filesystem::path& directory) {
        App app;
        emby::Session session;
        session.serverUrl = "synthetic fixture address";
        session.serverId = "synthetic-server";
        session.serverName = "Synthetic server";
        session.userId = "synthetic-user";
        session.userName = "Synthetic fixture";
        session.token = "synthetic-token";
        session.deviceId = "synthetic-device";
        app.embyConfigure(session);
        const auto movie = emby::parseItem(R"({
            "Id":"synthetic-film","Name":"Aurora: Synthetic Test Film","Type":"Movie",
            "ProductionYear":2024,"RunTimeTicks":72000000000,"OfficialRating":"PG-13",
            "CommunityRating":8.2,"CriticRating":92,"Genres":["Adventure","Science Fiction"],
            "ImageTags":{"Primary":"synthetic-poster-tag"},"BackdropImageTags":["synthetic-backdrop-tag"],
            "Studios":[{"Name":"Synthetic Studio"}],
            "People":[{"Name":"Synthetic Director","Type":"Director"},{"Name":"Synthetic Actor","Type":"Actor"}],
            "MediaStreams":[{"Type":"Video","Codec":"hevc","Width":3840,"Height":2160,"VideoRange":"HDR10"},
                            {"Type":"Audio","Codec":"aac","Channels":6,"SampleRate":48000,"Language":"eng","IsDefault":true}],
            "UserData":{"PlaybackPositionTicks":18000000000}
        })");
        require(movie.has_value(), "Cannot parse the production detail fixture DTO");
        const std::string overview = "Synthetic fixture: this artwork and story were created for a rendering test. "
            "When an unexpected signal crosses the winter sky, a small observatory team follows it beyond the city "
            "and into the mountains. Their journey becomes a search for the people who first heard the same sound. "
            "The backdrop, portrait poster, metadata, playback choices and watch progress all use the production "
            "native Windows renderer. This fixture does not represent media in the user's library. "
            "The longer synopsis checks wrapped text and the More / Less control across narrow layouts. "
            "A second expedition follows the trail at dawn, carrying the recording to a remote station, "
            "where the oldest instrument still remembers a message nobody else can hear.";
        ThumbnailCache images;
        struct Fixture {
            const wchar_t* name;
            float width;
            float height;
            float scale;
            bool expanded;
            bool missing;
            bool series;
        };
        const Fixture fixtures[]{
            {L"synthetic-movie-detail-wide-1x.bmp", 1440.0F, 960.0F, 1.0F, false, false, false},
            {L"synthetic-movie-detail-narrow-320dip-1x.bmp", 320.0F, 1180.0F, 1.0F, false, false, false},
            {L"synthetic-movie-detail-narrow-320dip-2x.bmp", 320.0F, 1180.0F, 2.0F, false, false, false},
            {L"synthetic-movie-detail-expanded-1x.bmp", 1024.0F, 1180.0F, 1.0F, true, false, false},
            {L"synthetic-movie-detail-missing-data-1x.bmp", 1024.0F, 760.0F, 1.0F, false, true, false},
            {L"synthetic-series-detail-wide-1x.bmp", 1440.0F, 960.0F, 1.0F, false, false, true}
        };
        for (const auto& fixture : fixtures) {
            App::EmbyDetails detail;
            detail.item = *movie;
            detail.item.overview = overview;
            detail.expanded = fixture.expanded;
            if (fixture.series) {
                detail.item.id = "synthetic-series";
                detail.item.name = "Aurora: Synthetic Series";
                detail.item.type = "Series";
                detail.item.isFolder = true;
                detail.item.runTimeTicks = detail.item.positionTicks = 0;
                detail.item.videoInfo.clear();
                detail.item.audioInfo.clear();
                const auto seasons = emby::parseItems(R"({"Items":[
                    {"Id":"synthetic-specials","Type":"Season","IndexNumber":0},
                    {"Id":"synthetic-season-one","Type":"Season","IndexNumber":1},
                    {"Id":"synthetic-season-two","Type":"Season","IndexNumber":2}
                ]})");
                const auto episodes = emby::parseItems(R"({"Items":[
                    {"Id":"synthetic-e1","Name":"Synthetic Signal","Type":"Episode","IndexNumber":1,
                     "ParentIndexNumber":1,"RunTimeTicks":18000000000,"UserData":{"PlaybackPositionTicks":4500000000}},
                    {"Id":"synthetic-e2","Name":"Synthetic Expedition","Type":"Episode","IndexNumber":2,
                     "ParentIndexNumber":1,"RunTimeTicks":18000000000}
                ]})");
                require(seasons && episodes, "Cannot parse the synthetic season/episode fixture");
                detail.seasons = seasons->items;
                detail.episodes = episodes->items;
                detail.seasonId = "synthetic-season-one";
            } else if (fixture.missing) {
                detail.item = emby::Item{};
                detail.item.id = "synthetic-missing";
                detail.item.name = "Synthetic Film: Missing Optional Metadata";
                detail.item.type = "Movie";
            }
            if (!fixture.missing) {
                images.insert(emby::imageKey(detail.item, 600), syntheticDetailBitmap(400, 600, true));
                for (const int width : {1280, 1920})
                    images.insert(emby::backdropImageKey(detail.item, width), syntheticDetailBitmap(1280, 720, false));
            }
            app.uiScale_ = fixture.scale;
            app.embyDetails_ = std::move(detail);
            OverlayScene scene;
            scene.width = fixture.width * fixture.scale;
            scene.height = fixture.height * fixture.scale;
            scene.scale = fixture.scale;
            scene.panel.alpha = 1.0F;
            scene.panel.title = L"Emby \u00B7 synthetic renderer fixture";
            scene.panel.thumbnails = &images;
            scene.panel.rows = app.buildEmbyDetailRows(scene.width);
            scene.panel.layout = settingsPanelLayout(scene.width, scene.height, scene.scale, scene.panel.rows,
                                                     0.0F, 1.0F, {}, scene.width);
            renderFixtureBmp(scene, directory / fixture.name);
        }
        app.embyDetails_.reset();
        app.uiScale_ = 1.0F;
        const auto libraries = emby::parseItems(R"({"Items":[
            {"Id":"synthetic-movies","Name":"Synthetic Movies","Type":"CollectionFolder","CollectionType":"movies"},
            {"Id":"synthetic-tv","Name":"Synthetic TV","Type":"CollectionFolder","CollectionType":"tvshows"}
        ]})");
        require(libraries.has_value(), "Cannot parse the synthetic F5 library switches");
        app.embyViews_ = libraries->items;
        OverlayScene settings;
        settings.width = 980.0F;
        settings.height = 920.0F;
        settings.panel.alpha = 1.0F;
        settings.panel.title = L"Settings \u00B7 synthetic renderer fixture";
        app.settingsTab_ = SettingsTab::General;
        for (int tab = 0; tab < kSettingsTabCount; ++tab) {
            settings.panel.tabs.emplace_back(settingsTabName(static_cast<SettingsTab>(tab)));
        }
        settings.panel.tab = static_cast<int>(app.settingsTab_);
        settings.panel.switchCaption = L"Emby \x203A";
        settings.panel.rows = app.buildSettingsRows(420.0F);
        settings.panel.layout = settingsPanelLayout(settings.width, settings.height, settings.scale,
                                                    settings.panel.rows, 0.0F, 1.0F, {}, 0.0F, 0.0F,
                                                    kSettingsTabCount, true);
        renderFixtureBmp(settings, directory / L"synthetic-f5-general-tab-1x.bmp");
    }

    static void nasCachePreferenceIsVisibleAndDeferred() {
        App app;
        HiddenWindow parent;
        app.window_ = app.videoWindow_ = parent.window;
        app.instance_ = GetModuleHandleW(nullptr);
        app.sources_[0] = std::make_unique<VideoSource>();
        auto* retainedSource = app.sources_[0].get();
        // Open the sheet as the frame loop would: slid fully in, geometry
        // built for the hidden window's client area.
        app.settingsOpen_ = true;
        app.settingsAlpha_ = 1.0F;
        app.settingsTab_ = SettingsTab::General;
        const RECT client{0, 0, 400, 300};
        app.refreshPanelGeometry(client);
        int nasRow = -1;
        for (std::size_t i = 0; i < app.panelRows_.size(); ++i) {
            if (app.panelRows_[i].id == SettingId::NasCache) nasRow = static_cast<int>(i);
        }
        require(nasRow >= 0, "NAS cache row missing from the settings sheet");
        require(app.panelRows_[static_cast<std::size_t>(nasRow)].on, "NAS cache should default on");
        // Scroll the row into the sheet, then hit it where the frame would.
        const auto& geometry = app.panelLayout_.rows[static_cast<std::size_t>(nasRow)];
        app.settingsScroll_ = std::max(0.0F, geometry.row.y - app.panelLayout_.content.y - 40.0F);
        app.refreshPanelGeometry(client);
        const auto& scrolled = app.panelLayout_.rows[static_cast<std::size_t>(nasRow)];
        require(app.panelLayout_.content.contains(scrolled.row.x + 1.0F, scrolled.row.y + 1.0F),
                "NAS cache row did not scroll into view");
        const PanelHit hit = app.panelHitAt(
            {static_cast<LONG>(scrolled.row.x + 1.0F), static_cast<LONG>(scrolled.row.y + 1.0F)});
        require(hit.kind == PanelHitKind::Toggle && hit.row == nasRow, "NAS cache row not hit");
        app.activatePanelHit(hit);
        require(!app.captureAppSettings().nasCache, "NAS cache option did not reach persistence state");
        require(app.sources_[0].get() == retainedSource, "NAS setting unexpectedly reopened playing sources");
        // A click outside the sheet closes it.
        app.activatePanelHit(app.panelHitAt({1, 1}));
        require(!app.settingsOpen_, "Click outside the sheet did not close it");
    }

    // Explicit CLI mode; all paths, thumbnail bytes and displayed frames are
    // synthetic. Only native App scene/layout and Overlay painting are real.
    static bool renderLocalMultiPaneFixturesWhenRequested(int argc, char** argv) {
        if (argc < 2 || std::string(argv[1]) != "--render-local-multipane-fixtures") return false;
        require(argc == 3, "Usage: --render-local-multipane-fixtures <absolute output directory>");
        const std::filesystem::path directory(utf8ToWideText(argv[2]));
        require(directory.is_absolute(), "The local fixture output directory must be absolute");
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        require(!error, "Cannot create the local fixture output directory");
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        require(SUCCEEDED(com) || com == RPC_E_CHANGED_MODE, "Cannot initialize COM for the local fixture");
        struct ComLifetime {
            bool initialized;
            ~ComLifetime() { if (initialized) CoUninitialize(); }
        } comLifetime{SUCCEEDED(com)};
        App app;
        HiddenWindow parent;
        auto queue = syntheticLocalQueue();
        queue.push_back({L"C:\\QuadDeckSynthetic\\Folder A\\04 Synthetic Journey Across the Winter Observatory "
                         L"and the Uncharted Mountains with a Complete Long Title for Narrow Replacement "
                         L"Notes and High DPI Text Wrapping Validation.mp4", 500'000'000, 4, 480.0});
        queue.push_back({L"C:\\QuadDeckSynthetic\\Folder A\\05 Synthetic Last Light.mkv", 625'000'000, 5, 600.0});
        prepareLocalBrowser(app, parent, queue);
        for (std::size_t pane = 0; pane < kMaxPanes; ++pane) seedLiveLocalPane(app, pane, queue, pane);
        app.audioMask_ = 5U;
        app.clock_.seek(42.0);
        app.clock_.pause();
        app.embySelectTarget(1);
        app.activateLocalItem(3, true);
        require(app.localPendingPlay_.has_value(), "The full local fixture did not open its native chooser");
        struct ChoiceFixture { const wchar_t* name; float width; float scale; };
        const ChoiceFixture chooser[]{
            {L"local-multipane-replacement-wrapped-wide-1x.bmp", 1120.0F, 1.0F},
            {L"local-multipane-replacement-300dip-1x.bmp", 300.0F, 1.0F},
            {L"local-multipane-replacement-320dip-1x.bmp", 320.0F, 1.0F},
            {L"local-multipane-replacement-340dip-1x.bmp", 340.0F, 1.0F},
            {L"local-multipane-replacement-320dip-2x.bmp", 320.0F, 2.0F}
        };
        for (const auto& fixture : chooser) {
            app.uiScale_ = fixture.scale;
            OverlayScene scene;
            scene.width = fixture.width * fixture.scale;
            scene.height = (fixture.width > 340.0F ? 900.0F : 2000.0F) * fixture.scale;
            scene.scale = fixture.scale;
            scene.panel.alpha = 1.0F;
            scene.panel.title = L"Local · synthetic full-deck replacement";
            scene.panel.rows = app.buildLocalRows(scene.width);
            for (const auto& row : scene.panel.rows) {
                for (std::size_t part = 0; part < row.tileKeys.size(); ++part)
                    app.thumbnails_.insert(row.tileKeys[part], syntheticDetailBitmap(640, 360, part % 2 == 0));
            }
            scene.panel.thumbnails = &app.thumbnails_;
            scene.panel.layout = settingsPanelLayout(scene.width, scene.height, scene.scale, scene.panel.rows,
                                                     0.0F, 1.0F, {}, scene.width);
            renderFixtureBmp(scene, directory / fixture.name);
        }
        app.localCancelReplacement();
        auto externalPath = queue[3].path;
        externalPath.insert(externalPath.size() - 4, L" External launch");
        app.enqueueExternalFiles({externalPath, L"C:\\QuadDeckSynthetic\\Folder A\\Later file.mp4"});
        app.processExternalFiles();
        require(app.localPendingPlay_ && app.localPendingPlay_->external,
                "The external fixture did not open its batch replacement chooser");
        for (const float scale : {1.0F, 2.0F}) {
            app.uiScale_ = scale;
            OverlayScene scene;
            scene.width = 320.0F * scale;
            scene.height = 2200.0F * scale;
            scene.scale = scale;
            scene.panel.alpha = 1.0F;
            scene.panel.title = L"Local \x2014 synthetic external launch";
            scene.panel.rows = app.buildLocalReplacementRows(scene.width);
            require(scene.panel.rows[1].label.find(L"2 files remain") != std::wstring::npos,
                    "External fixture lost the batch remaining count");
            scene.panel.thumbnails = &app.thumbnails_;
            scene.panel.layout = settingsPanelLayout(scene.width, scene.height, scene.scale, scene.panel.rows,
                                                     0.0F, 1.0F, {}, scene.width);
            renderFixtureBmp(scene, directory / (scale == 1.0F
                ? L"local-external-replacement-320dip-1x.bmp" : L"local-external-replacement-320dip-2x.bmp"));
        }
        app.localCancelReplacement();
        for (std::size_t pane = 3; pane < kMaxPanes; ++pane) {
            app.sources_[pane].reset(); app.paths_[pane].clear(); app.localPanes_[pane].reset();
        }
        app.uiScale_ = 1.0F;
        app.layoutMode_ = LayoutMode::Column4;
        app.controlsPinned_ = true;
        app.dockProgress_ = 1.0F;
        app.paneChromeAlpha_.fill(1.0F);
        app.hoverPane_ = 1;
        app.pressedChipPane_ = 0;
        app.duration_ = 600.0;
        app.noticeText_ = L"Synthetic local frames / artwork · native App target, audio, All and F6 browser";
        app.noticeUntil_ = GetTickCount64() + 60'000;
        require(SetWindowPos(parent.window, nullptr, 0, 0, 1600, 1400,
                             SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOZORDER) != FALSE,
                "Cannot size the hidden local panoramic fixture window");
        const RECT client{0, 0, 1600, 1400};
        for (const int view : {0, 2}) {
            app.embyBrowser_.view = view;
            app.settingsScroll_ = 0.0F;
            app.refreshChromeGeometry(client);
            app.refreshPanelGeometry(client);
            // Cache the production local keys before asking App for its scene.
            for (const auto& row : app.panelRows_) {
                for (std::size_t part = 0; part < row.tileKeys.size(); ++part)
                    app.thumbnails_.insert(row.tileKeys[part], syntheticDetailBitmap(640, 360, part % 2 == 0));
            }
            app.buildOverlayScene();
            require(app.scene_.activePaneCount == 3 && app.scene_.panes[1].targeted &&
                    app.scene_.panes[0].audioOn && !app.scene_.panes[1].audioOn && app.scene_.panes[2].audioOn &&
                    app.scene_.bar.scopeLabel.visible() && app.scene_.panel.layout.sheet.x > 0.0F &&
                    std::any_of(app.scene_.panel.rows.begin(), app.scene_.panel.rows.end(), [](const PanelRow& row) {
                        return row.id == SettingId::LocalItem;
                    }), "The native local panorama lost its three panes, target, audio or F6 browser");
            renderFixtureBmp(app.scene_, directory / (view == 0 ?
                L"local-multipane-three-pane-list-1x.bmp" : L"local-multipane-three-pane-tiles-1x.bmp"));
        }
        return true;
    }

    static void renderEmbyMultiPaneFixtures(const std::filesystem::path& directory) {
        App app;
        HiddenWindow parent;
        prepareDetailBrowser(app, parent);
        ThumbnailCache images;
        for (std::size_t pane = 0; pane < kMaxPanes; ++pane) {
            auto item = multiPaneItem("fixture-" + std::to_string(pane + 1));
            item.primaryImageTag = "synthetic-poster";
            seedLiveEmbyPane(app, pane, item);
            auto& state = *app.embyPanes_[pane];
            state.title = L"Synthetic clip " + std::to_wstring(pane + 1);
            state.imageKey = emby::imageKey(item, 400);
            images.insert(state.imageKey, syntheticDetailBitmap(640, 360, pane % 2 == 0));
        }
        app.audioMask_ = 1U;
        const auto sixth = multiPaneItem("sixth-fixture");
        app.embyRequestPlay(sixth, emby::takeQueue({sixth}, 0, "synthetic-chooser"), true);
        require(app.embyPendingPlay_.has_value(), "The production full-deck fixture did not open its chooser");
        for (const int width : {1120, 360}) {
            OverlayScene scene;
            scene.width = static_cast<float>(width);
            scene.height = width == 1120 ? 820.0F : 1440.0F;
            scene.panel.alpha = 1.0F;
            scene.panel.title = L"Emby \u00B7 synthetic five-pane chooser";
            scene.panel.thumbnails = &images;
            scene.panel.rows = app.buildEmbyRows(scene.width);
            scene.panel.layout = settingsPanelLayout(scene.width, scene.height, scene.scale, scene.panel.rows,
                                                     0.0F, 1.0F, {}, scene.width);
            renderFixtureBmp(scene, directory / (width == 1120 ?
                L"synthetic-five-pane-replacement-wide-1x.bmp" : L"synthetic-five-pane-replacement-narrow-1x.bmp"));
        }
        app.embyCancelReplacement();
        for (std::size_t pane = 2; pane < kMaxPanes; ++pane) {
            app.sources_[pane].reset(); app.paths_[pane].clear(); app.embyPanes_[pane].reset();
        }
        app.embySelectTarget(1);
        App::EmbyDetails detail;
        detail.item = multiPaneItem("target-fixture");
        detail.item.type = "Movie";
        detail.item.name = "Synthetic Film: Two-Pane Playback Target";
        detail.item.overview = "Synthetic renderer fixture. Play and Resume replace the selected V2 pane. "
            "Add opens another pane muted; saved progress and From Beginning are explicit choices. "
            "The two existing synthetic panes and their sound remain independently owned.";
        detail.item.primaryImageTag = "synthetic-target-poster";
        detail.item.backdropImageTags = {"synthetic-target-backdrop"};
        images.insert(emby::imageKey(detail.item, 600), syntheticDetailBitmap(400, 600, true));
        images.insert(emby::backdropImageKey(detail.item, 1280), syntheticDetailBitmap(1280, 720, false));
        app.embyDetails_ = std::move(detail);
        OverlayScene scene;
        scene.width = 1120.0F; scene.height = 940.0F;
        scene.panel.alpha = 1.0F;
        scene.panel.title = L"Emby \u00B7 synthetic two-pane target";
        scene.panel.thumbnails = &images;
        scene.panel.rows = app.buildEmbyRows(scene.width);
        scene.panel.layout = settingsPanelLayout(scene.width, scene.height, scene.scale, scene.panel.rows,
                                                 0.0F, 1.0F, {}, scene.width);
        renderFixtureBmp(scene, directory / L"synthetic-two-pane-detail-target-1x.bmp");
        require(SetWindowPos(parent.window, nullptr, 0, 0, 1600, 1100,
                             SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOZORDER) != FALSE,
                "Cannot size the hidden panoramic fixture window");
        app.thumbnails_ = images;
        app.layoutMode_ = LayoutMode::Column4;
        app.controlsPinned_ = true;
        app.dockProgress_ = 1.0F;
        app.paneChromeAlpha_.fill(1.0F);
        app.hoverPane_ = 1;
        app.pressedChipPane_ = 0;
        app.clock_.seek(42.0);
        app.clock_.pause();
        app.duration_ = 600.0;
        app.noticeText_ = L"Synthetic frames \u00B7 native App layout / target / audio / All controls";
        app.noticeUntil_ = GetTickCount64() + 60'000;
        const RECT deckClient{0, 0, 1600, 1100};
        app.refreshChromeGeometry(deckClient);
        app.refreshPanelGeometry(deckClient);
        app.buildOverlayScene();
        require(app.scene_.activePaneCount == 2 && app.scene_.panes[1].targeted &&
                app.scene_.panes[0].audioOn && !app.scene_.panes[1].audioOn &&
                app.scene_.bar.scopeLabel.visible() && app.scene_.panel.layout.sheet.x > 0.0F,
                "The production panorama lost multi-pane target/audio/All state or the docked detail");
        renderFixtureBmp(app.scene_, directory / L"synthetic-multipane-deck-browser-1x.bmp");
    }

    // The two RTX Video toggles sit in the Picture section, default off, and
    // a hit on either reaches the settings that will be saved. There is no
    // renderer here, so the note under them reports whatever an empty status
    // says; the renderer-side behaviour is a runtime scenario.
    static void rtxVideoTogglesReachPersistence() {
        App app;
        HiddenWindow parent;
        app.window_ = app.videoWindow_ = parent.window;
        app.instance_ = GetModuleHandleW(nullptr);
        app.settingsOpen_ = true;
        app.settingsAlpha_ = 1.0F;
        app.settingsTab_ = SettingsTab::Picture;
        const RECT client{0, 0, 400, 300};
        app.refreshPanelGeometry(client);
        for (const SettingId id : {SettingId::SuperResolution, SettingId::RtxHdr}) {
            // Row positions are read from the unscrolled sheet.
            app.settingsScroll_ = 0.0F;
            app.refreshPanelGeometry(client);
            int row = -1;
            for (std::size_t i = 0; i < app.panelRows_.size(); ++i) {
                if (app.panelRows_[i].id == id) row = static_cast<int>(i);
            }
            require(row >= 0, "RTX Video row missing from the settings sheet");
            require(!app.panelRows_[static_cast<std::size_t>(row)].on, "RTX Video should default off");
            const auto& geometry = app.panelLayout_.rows[static_cast<std::size_t>(row)];
            app.settingsScroll_ = std::max(0.0F, geometry.row.y - app.panelLayout_.content.y - 40.0F);
            app.refreshPanelGeometry(client);
            const auto& scrolled = app.panelLayout_.rows[static_cast<std::size_t>(row)];
            const PanelHit hit = app.panelHitAt(
                {static_cast<LONG>(scrolled.row.x + 1.0F), static_cast<LONG>(scrolled.row.y + 1.0F)});
            require(hit.kind == PanelHitKind::Toggle && hit.row == row, "RTX Video row not hit");
            app.activatePanelHit(hit);
        }
        const auto saved = app.captureAppSettings();
        require(saved.rtxVideo.superResolution && saved.rtxVideo.rtxHdr,
                "RTX Video toggles did not reach persistence state");
        app.refreshPanelGeometry(client);
        for (const auto& row : app.panelRows_) {
            if (row.id == SettingId::SuperResolution || row.id == SettingId::RtxHdr)
                require(row.on, "RTX Video row does not show its new state");
        }
    }

    // An Emby item replaces only the selected slot, stores its locator and
    // retains the other panes and their audio selection, and the
    // browser's rows appear when the sheet is in browser mode. No server is
    // reachable here, so the resolve request fails on its worker thread and
    // is never pumped; the state set before it was queued is what is checked.
    static void embyItemPreservesTheOtherPanes() {
        App app;
        HiddenWindow parent;
        app.window_ = app.videoWindow_ = parent.window;
        app.instance_ = GetModuleHandleW(nullptr);
        app.paths_[1] = L"C:\\Videos\\two.mp4";
        app.sources_[1] = std::make_unique<VideoSource>();
        const auto* otherSource = app.sources_[1].get();
        app.audioMask_ = 1U << 1;
        emby::Session session;
        session.serverUrl = "http://127.0.0.1:9";
        session.serverId = "srv";
        session.serverName = "Test";
        session.userId = "u1";
        session.userName = "guest";
        session.token = "token";
        session.deviceId = "dev";
        app.emby_.configure(session);
        require(app.emby_.session().signedIn(), "Test session should count as signed in");
        // The sheet has no account rows of its own: its header switch is
        // the way to the browser, and back.
        app.settingsOpen_ = true;
        app.settingsAlpha_ = 1.0F;
        const RECT client{0, 0, 400, 300};
        app.refreshPanelGeometry(client);
        for (const auto& row : app.panelRows_) {
            require(row.id != SettingId::EmbyAccount, "The settings sheet still carries an Emby account row");
        }
        require(app.panelLayout_.sheetSwitch.visible() &&
                app.panelHitAt({static_cast<LONG>(app.panelLayout_.sheetSwitch.x + 1.0F),
                                static_cast<LONG>(app.panelLayout_.sheetSwitch.y + 1.0F)}).kind == PanelHitKind::Switch,
                "The sheet's header has no switch to the browser");
        app.switchSheet();
        require(app.settingsOpen_ && app.embyBrowserOpen_, "The header switch did not open the browser");
        app.refreshPanelGeometry(client);
        require(app.panelLayout_.sheetSwitch.visible(), "The browser's header lost the switch back");
        // The home page's row has no Settings button of its own any more:
        // Search, Sign out, Refresh.
        bool homeRow = false;
        for (const auto& row : app.panelRows_) {
            if (row.id != SettingId::EmbyNav) continue;
            homeRow = row.options.size() == 3 && row.options[0] == L"Search\x2026" &&
                      row.options[1] == L"Sign out" && row.options[2] == L"Refresh";
        }
        require(homeRow && app.embyPages_.size() == 1, "The browser's home row is not Search / Sign out / Refresh");
        app.switchSheet();
        require(app.settingsOpen_ && !app.embyBrowserOpen_, "The header switch did not return to the settings");
        emby::Item item;
        item.id = "42";
        item.name = "Clip";
        item.type = "Video";
        item.mediaType = "Video";
        item.parentId = "7";
        app.embyOpenBrowser();
        require(app.settingsOpen_ && app.embyBrowserOpen_, "Opening Emby did not show the browser");
        App::EmbyPage folder;
        folder.kind = App::EmbyPage::Kind::Folder;
        folder.id = "7";
        folder.title = "Folder";
        app.embyPages_.push_back(folder);
        app.embyPlayItem(item, 0);
        require(app.paths_[1] == L"C:\\Videos\\two.mp4" && app.sources_[1].get() == otherSource,
                "Choosing an Emby item changed another pane");
        require(app.paths_[0] == L"emby://srv/42", "The locator was not stored as the pane path");
        require(app.embyPanes_[0].has_value(), "The pane has no Emby state");
        require(app.embyPanes_[0]->itemId == "42" && app.embyPanes_[0]->autoplay &&
                app.embyPanes_[0]->parentId == "7", "The pane's Emby state is wrong");
        require(app.audioMask_ == 2U, "Choosing an Emby item changed the existing audio selection");
        // A video was loaded when the browser opened, so it was docked
        // beside it and stays there after the choice, like a playlist.
        require(app.settingsOpen_ && app.embyBrowserOpen_ && app.embyBrowserDocked(),
                "Choosing an item from the docked browser closed it");
        app.closeSettingsPanel();
        // A session keeps the locator, so reopening it resolves again.
        require(app.captureSession().panes[0].path == L"emby://srv/42", "Session lost the locator");
        const auto requireMode = [&](bool browser) {
            require(app.settingsOpen_ && app.embyBrowserOpen_ == browser, "Wrong settings sheet mode");
            // The browser's pages carry a nav row, its sign-in page an
            // account row; the normal sheet has neither and a Playback row.
            app.settingsTab_ = SettingsTab::Playback;
            app.refreshPanelGeometry(client);
            bool browserRow = false;
            bool playback = false;
            for (const auto& row : app.panelRows_) {
                browserRow = browserRow || row.id == SettingId::EmbyNav || row.id == SettingId::EmbyAccount;
                playback = playback || row.id == SettingId::Decoder;
            }
            require(browserRow == browser && playback != browser, "The sheet shows rows for the wrong mode");
        };
        VideoSource* retainedSource = nullptr;
        std::uint64_t retainedSerial = 0;
        const auto requirePlaybackRetained = [&] {
            require(app.sources_[0].get() == retainedSource && app.clock_.isPlaying() &&
                        app.clock_.position() >= 42.0 && app.audioMask_ == 2U,
                    "Changing the sheet mode changed playback");
            require(app.embyPanes_[0] && app.embyPanes_[0]->serial == retainedSerial &&
                        app.paths_[0] == L"emby://srv/42",
                    "Changing the sheet mode cleared the Emby item");
            require(app.embyPages_.size() == 2 && app.embyPages_.back().id == "7",
                    "Changing the sheet mode cleared the browser location");
        };
        // Each real Settings entry point must work after choosing an item.
        for (int entry = 0; entry < 3; ++entry) {
            if (entry > 0) {
                app.embyOpenBrowser();
                requireMode(true);
                app.embyPlayItem(item, 0);
                require(app.settingsOpen_ && app.embyBrowserDocked(), "The docked browser did not stay");
                app.closeSettingsPanel();
            }
            require(!app.settingsOpen_, "Premise: the sheet is closed");
            // Preserve a running clock and an unopened source without real media.
            app.sources_[0] = std::make_unique<VideoSource>();
            retainedSource = app.sources_[0].get();
            retainedSerial = app.embyPanes_[0]->serial;
            app.clock_.seek(42.0);
            app.clock_.play();
            if (entry == 0) app.handleMessage(WM_KEYDOWN, VK_F5, 0);
            else if (entry == 1) app.activateBarItem(BarItem::Settings);
            else {
                app.handleContextCommand(app_internal::CmdSettings, -1);
                // The STATIC fixture has no App timer handler/message pump.
                // Cancel the deferred save rather than dispatching it.
                KillTimer(parent.window, app_internal::kSettingsSaveTimer);
                app.settingsSavePending_ = false;
            }
            requireMode(false);
            requirePlaybackRetained();
            app.toggleSettingsPanel();
            require(!app.settingsOpen_, "Toggling ordinary settings did not close the sheet");
        }
        app.embyOpenBrowser();
        requireMode(true);
        app.settingsScroll_ = 150.0F;
        app.pressedPanelKind_ = PanelHitKind::Item;
        app.pressedPanelRow_ = 3;
        app.handleMessage(WM_KEYDOWN, VK_F5, 0);
        require(app.settingsScroll_ == 0.0F && app.pressedPanelKind_ == PanelHitKind::None &&
                    app.pressedPanelRow_ == -1,
                "Switching from Emby retained its scroll or pressed row");
        requireMode(false);
        requirePlaybackRetained();
        app.embyOpenBrowser();
        requireMode(true);
        app.closeSettingsPanel();
        app.toggleSettingsPanel();
        requireMode(false);
        requirePlaybackRetained();
        // Closing the pane drops the Emby state with it.
        app.closePane(0);
        require(!app.embyPanes_[0].has_value() && app.paths_[0].empty(), "closePane kept Emby state");
        // The sign-in path also opens a closed sheet in browser mode. Its
        // localhost request is never pumped, so no auth file is read or saved.
        app.closeSettingsPanel();
        app.embySignIn(session.serverUrl, session.userName, "");
        requireMode(true);
        require(app.embyLoading_, "Sign-in did not enter the loading browser");
    }

    // Board thread 10, E01: the HTTP client's worker must not touch members
    // constructed after it, and a client destroyed with a request in flight
    // to a port nothing listens on joins its worker cleanly.
    static void embyClientStartsAndStopsCleanly() {
        for (int round = 0; round < 12; ++round) {
            EmbyClient client;
            emby::Session session;
            session.serverUrl = "http://127.0.0.1:9";
            session.deviceId = "dev";
            client.configure(session);
            int replies = 0;
            client.request("GET", emby::publicInfoPath(), "", [&replies](const EmbyClient::Response& response) {
                ++replies;
                require(!response.ok && !response.error.empty(), "Nothing listens on port 9");
            });
            if (round % 2 == 0) {
                client.drain(5000);
                client.pump();
                require(replies == 1, "The queued request did not complete");
            }
            // Odd rounds destroy the client with the request possibly in flight.
        }
    }

    // E03: a pane whose Emby item is still being resolved counts as an
    // opening source. E06: signing out closes both resolving and playing
    // Emby panes, preserving local media and retiring serials so a queued
    // reply finds no pane. E02: a new session leaves no Emby state on the
    // panes it empties.
    static void embyStateFollowsSignOutAndSessions() {
        App app;
        HiddenWindow parent;
        app.window_ = app.videoWindow_ = parent.window;
        app.instance_ = GetModuleHandleW(nullptr);
        emby::Session session;
        session.serverUrl = "http://127.0.0.1:9";
        session.serverId = "srv";
        session.userId = "u1";
        session.userName = "guest";
        session.token = "token";
        session.deviceId = "dev";
        app.emby_.configure(session);
        // Sign-out saves the sign-in file; never the real one from a test.
        const auto authDirectory = std::filesystem::temp_directory_path() / L"QuadDeckEmbyAuthTest";
        std::error_code code;
        std::filesystem::create_directories(authDirectory, code);
        app.embyAuthPathOverride_ = authDirectory / L"emby.qauth";
        emby::Item item;
        item.id = "42";
        item.name = "Clip";
        item.type = "Video";
        item.mediaType = "Video";
        app.embyPlayItem(item, 0);
        require(app.embyPanes_[0].has_value() && app.embyPanes_[0]->resolving, "A chosen item should be resolving");
        require(app.paneOpening(0), "A resolving Emby pane must count as an opening source");
        app.embySignOut();
        require(!app.embyPanes_[0].has_value() && app.paths_[0].empty(), "Sign-out left a resolving pane behind");
        require(!app.emby_.session().signedIn(), "Sign-out kept the token");
        // Account-owned playback closes on sign-out; a local picture stays.
        app.emby_.configure(session);
        app.paths_[2] = L"C:\\Synthetic\\account-local.mp4";
        app.sources_[2] = std::make_unique<VideoSource>();
        auto* localSource = app.sources_[2].get();
        app.paths_[0] = L"emby://srv/42";
        App::EmbyPane playing;
        playing.serial = ++app.embyPaneSerial_;
        playing.itemId = "42";
        playing.mediaSourceId = "ms";
        playing.playSessionId = "ps";
        playing.started = true;
        bindEmbyAccount(app, playing);
        app.embyPanes_[0] = playing;
        const auto serialBefore = playing.serial;
        require(app.embyPaneWithSerial(serialBefore) == std::optional<std::size_t>{0}, "Serial lookup failed");
        app.embySignOut();
        require(!app.embyPanes_[0] && app.paths_[0].empty() && !app.sources_[0],
                "Sign-out retained old-account playback");
        require(app.paths_[2] == L"C:\\Synthetic\\account-local.mp4" && app.sources_[2].get() == localSource,
                "Sign-out changed local playback");
        require(!app.embyPaneWithSerial(serialBefore).has_value(), "A retired serial must find no pane");
        require(!app.paneOpening(0), "A detached pane is not opening");
        app.embyPanes_[1] = App::EmbyPane{};
        app.embyPanes_[1]->itemId = "old";
        app.embyPanes_[1]->resolving = true;
        app.subtitles_[1] = std::make_shared<const SubtitleTrack>();
        // As the recovery test does: defers the shader and the decoders.
        app.deviceRecoveryPending_ = true;
        SessionState fresh;
        app.applySession(fresh);
        require(!app.embyPanes_[0].has_value() && !app.embyPanes_[1].has_value() && !app.subtitles_[1],
                "applySession kept stale media state");
        require(std::filesystem::exists(app.embyAuthPathOverride_), "Sign-out did not write the redirected sign-in file");
        std::filesystem::remove_all(authDirectory, code);
    }

    // E04: swapping panes moves subtitles and Emby state with the media,
    // and a reply still in flight finds the moved pane by serial.
    static void swapPanesMovesMediaState() {
        App app;
        HiddenWindow parent;
        app.window_ = app.videoWindow_ = parent.window;
        app.instance_ = GetModuleHandleW(nullptr);
        emby::Session account;
        account.serverUrl = "synthetic fixture address";
        account.serverId = "srv";
        account.userId = "synthetic-user";
        account.token = "synthetic-token";
        account.deviceId = "synthetic-device";
        app.embyConfigure(account);
        app.sources_[0] = std::make_unique<VideoSource>();
        app.sources_[1] = std::make_unique<VideoSource>();
        app.paths_[0] = L"C:\\Videos\\one.mp4";
        app.paths_[1] = L"emby://srv/7";
        SubtitleTrack cues;
        cues.cues.push_back({1.0, 3.0, L"A"});
        const auto track = std::make_shared<const SubtitleTrack>(std::move(cues));
        const std::filesystem::path sidecar(L"C:\\Videos\\one.srt");
        app.subtitles_[0] = track;
        app.subtitleSelection_[0] = {App::SubtitleKind::File, -1, sidecar};
        app.subtitleFiles_[0] = {sidecar};
        app.subtitleDelay_[0] = 1.5;
        app.subtitleChosenByViewer_[0] = true;
        app.subtitleSerial_[0] = 41;
        app.subtitleSerial_[1] = 42;
        app.embyPanes_[1] = App::EmbyPane{};
        bindEmbyAccount(app, *app.embyPanes_[1]);
        app.embyPanes_[1]->itemId = "7";
        app.embyPanes_[1]->serial = 99;
        app.swapPanes(0, 1);
        require(app.paths_[0] == L"emby://srv/7" && app.paths_[1] == L"C:\\Videos\\one.mp4", "Paths did not swap");
        require(app.subtitles_[1] == track && app.subtitleSelection_[1].kind == App::SubtitleKind::File &&
                app.subtitleSelection_[1].file == sidecar && app.subtitleFiles_[1].size() == 1 &&
                !app.subtitles_[0] && app.subtitleSelection_[0].kind == App::SubtitleKind::None &&
                app.subtitleFiles_[0].empty(),
                "Subtitles did not follow the swap");
        require(app.subtitleDelay_[1] == 1.5 && app.subtitleDelay_[0] == 0.0 &&
                app.subtitleChosenByViewer_[1] && !app.subtitleChosenByViewer_[0],
                "A subtitle's timing and the viewer's choice did not follow the swap");
        // A file still being read finds the pane where it now is.
        require(app.subtitlePaneWithSerial(41) == std::optional<std::size_t>{1} &&
                app.subtitlePaneWithSerial(42) == std::optional<std::size_t>{0},
                "A subtitle read must find the moved pane");
        require(app.embyPanes_[0].has_value() && app.embyPanes_[0]->itemId == "7" && !app.embyPanes_[1].has_value(),
                "Emby state did not follow the swap");
        require(app.embyPaneWithSerial(99) == std::optional<std::size_t>{0}, "A reply must find the moved pane");
    }

    // E05: a press on a browser row is released on the same row only while
    // it is still the same row. A reply that inserts rows above it between
    // press and release must not turn the release into another item.
    static void panelPressSurvivesRowInsertion() {
        App app;
        HiddenWindow parent;
        app.window_ = app.videoWindow_ = parent.window;
        app.instance_ = GetModuleHandleW(nullptr);
        emby::Session session;
        session.serverUrl = "http://127.0.0.1:9";
        session.serverId = "srv";
        session.userId = "u1";
        session.token = "token";
        session.deviceId = "dev";
        app.emby_.configure(session);
        app.settingsOpen_ = true;
        app.settingsAlpha_ = 1.0F;
        app.embyBrowserOpen_ = true;
        app.embyPages_.push_back(App::EmbyPage{});
        emby::Item library;
        library.id = "79571";
        library.name = "anime";
        library.type = "CollectionFolder";
        library.collectionType = "tvshows";
        app.embyItems_.push_back(library);
        // The list view: one row per item, which is what this press names.
        app.embyBrowser_.view = 0;
        const RECT client{0, 0, 900, 700};
        app.refreshPanelGeometry(client);
        const auto rowOf = [&](int param) {
            for (std::size_t i = 0; i < app.panelRows_.size(); ++i) {
                if (app.panelRows_[i].id == SettingId::EmbyItem && app.panelRows_[i].param == param) return static_cast<int>(i);
            }
            return -1;
        };
        const auto press = [&](int row) {
            app.pressedPanelKind_ = PanelHitKind::Item;
            app.pressedPanelRow_ = row;
            app.pressedPanelPart_ = -1;
            app.pressedPanelId_ = app.panelRows_[static_cast<std::size_t>(row)].id;
            app.pressedPanelParam_ = app.panelRows_[static_cast<std::size_t>(row)].param;
        };
        const auto pointOn = [&](int row) {
            const auto& box = app.panelLayout_.rows[static_cast<std::size_t>(row)].row;
            return POINT{static_cast<LONG>(box.x + 2.0F), static_cast<LONG>(box.y + 2.0F)};
        };
        const int libraryRow = rowOf(0);
        require(libraryRow >= 0, "Library row missing from the browser");
        press(libraryRow);
        // The Resume reply lands between press and release.
        emby::Item resume;
        resume.id = "1";
        resume.name = "Ep";
        resume.type = "Episode";
        resume.mediaType = "Video";
        app.embyResume_.push_back(resume);
        app.refreshPanelGeometry(client);
        require(app.panelRows_[static_cast<std::size_t>(libraryRow)].id == SettingId::EmbyItem &&
                app.panelRows_[static_cast<std::size_t>(libraryRow)].param == -1,
                "Premise: the pressed row number now names the resume item");
        app.releasePanelPress(pointOn(libraryRow));
        require(app.paths_[0].empty() && app.embyPages_.size() == 1, "The release acted on a row that changed identity");
        // With the rows unchanged, the same press opens the library.
        const int shifted = rowOf(0);
        require(shifted > libraryRow, "Premise: the library row moved down");
        press(shifted);
        app.releasePanelPress(pointOn(shifted));
        require(app.embyPages_.size() == 2 && app.embyPages_.back().kind == App::EmbyPage::Kind::Library,
                "A row that kept its identity did not activate");
    }

    // The browser as a grid: the tiles carry their items, a press on one is
    // matched by identity at release, and the list an item was chosen from
    // is what Page Up/Down walk -- a search's results, in the order shown.
    static void embyTilesAndPlaylistFollowTheList() {
        App app;
        HiddenWindow parent;
        app.window_ = app.videoWindow_ = parent.window;
        app.instance_ = GetModuleHandleW(nullptr);
        emby::Session session;
        session.serverUrl = "http://127.0.0.1:9";
        session.serverId = "srv";
        session.userId = "u1";
        session.token = "token";
        session.deviceId = "dev";
        app.embyConfigure(session);
        // Signing out at the end saves the sign-in file; never the real one.
        const auto authDirectory = std::filesystem::temp_directory_path() / L"QuadDeckEmbyTilesTest";
        std::error_code code;
        std::filesystem::create_directories(authDirectory, code);
        app.embyAuthPathOverride_ = authDirectory / L"emby.qauth";
        app.settingsOpen_ = true;
        app.settingsAlpha_ = 1.0F;
        app.embyBrowserOpen_ = true;
        app.embyPages_.push_back(App::EmbyPage{});
        App::EmbyPage search;
        search.kind = App::EmbyPage::Kind::Search;
        search.term = "trip";
        search.title = "trip";
        app.embyPages_.push_back(search);
        const auto video = [](const char* id, const char* name, std::int64_t size = 0) {
            emby::Item item;
            item.id = id;
            item.name = name;
            item.type = "Video";
            item.mediaType = "Video";
            item.primaryImageTag = "t";
            item.size = size;
            item.runTimeTicks = 14200000000;   // 23:40
            return item;
        };
        emby::Item folder;
        folder.id = "f";
        folder.name = "Trips";
        folder.type = "Folder";
        folder.isFolder = true;
        app.embyItems_ = {video("a", "Trip A", 300), folder, video("b", "Trip B", 100), video("c", "Trip C", 200)};
        app.embyItems_[2].positionTicks = 300000000;   // 30 s in: "▶ 2%"
        app.embyBrowser_.view = 2;
        const RECT client{0, 0, 1280, 720};
        app.refreshPanelGeometry(client);
        require(app.panelLayout_.sheet.x == 0.0F && app.panelLayout_.sheet.width == 1280.0F,
                "The browser does not take the whole window");
        int tilesRow = -1;
        for (std::size_t i = 0; i < app.panelRows_.size(); ++i) {
            if (app.panelRows_[i].kind == PanelRowKind::Tiles) { tilesRow = static_cast<int>(i); break; }
        }
        require(tilesRow >= 0, "No tiles row in the thumbnail view");
        const auto& tiles = app.panelRows_[static_cast<std::size_t>(tilesRow)];
        require(tiles.options.size() == 4 && tiles.tileParams == std::vector<int>{0, 1, 2, 3},
                "The tiles do not name the items in order");
        // Five fit 1240 px; four items on one line, each stretched to a fifth.
        require(std::abs(tiles.tileWidth - 238.4F) < 0.01F, "The tiles do not fill the line");
        require(tiles.tileKeys[0] == "a|t|500" && tiles.tileKeys[1].empty(), "Tile picture keys are wrong");
        require(tiles.tileBadges[1] == L"Folder", "A folder tile does not say so");
        // Watched state and length are both there, separately, and nothing
        // is being played yet.
        require(tiles.tileBadges[2] == L"23:40" && tiles.tileMarks[2] == L"\x25B6 2%" &&
                tiles.tileProgress[2] > 0.02F && tiles.tileProgress[2] < 0.022F,
                "A started video's tile does not show its progress beside its length");
        require(tiles.tileBadges[0] == L"23:40" && tiles.tileMarks[0].empty() && tiles.tileProgress[0] == 0.0F,
                "An unwatched video's tile shows a watched state");
        require(tiles.selected == -1, "A tile is marked as playing before anything plays");
        require(app.embyImagesInFlight_.count("a|t|500") == 1 && app.embyImagesInFlight_.count("c|t|500") == 1,
                "Visible tiles did not ask for their pictures");
        // The replies -- here failures from a port nobody listens on, or
        // cancellations -- come back through the frame tick; a request that
        // never answers would leave its tile blank for good.
        app.embyImages_.clearPending();
        for (int i = 0; i < 500 && !app.embyImagesInFlight_.empty(); ++i) {
            app.embyTick();
            Sleep(10);
        }
        require(app.embyImagesInFlight_.empty(), "Picture replies never reached the window thread");
        require(!app.thumbnails_.contains("zz"), "Premise");
        // Sort and filter rows are offered on a search page; the view row everywhere.
        bool viewRow = false, sortRow = false, unplayedRow = false, flatRow = false;
        for (const auto& row : app.panelRows_) {
            viewRow = viewRow || row.id == SettingId::EmbyView;
            sortRow = sortRow || row.id == SettingId::EmbySort;
            unplayedRow = unplayedRow || row.id == SettingId::EmbyUnplayed;
            flatRow = flatRow || row.id == SettingId::EmbyFlat;
        }
        require(viewRow && sortRow && unplayedRow && !flatRow, "The search page offers the wrong arrangement rows");
        // A press on the third tile, released on it, plays that item.
        const auto& box = app.panelLayout_.rows[static_cast<std::size_t>(tilesRow)].parts[2];
        const POINT onTile{static_cast<LONG>(box.x + 2.0F), static_cast<LONG>(box.y + 2.0F)};
        const PanelHit hit = app.panelHitAt(onTile);
        require(hit.kind == PanelHitKind::Tile && hit.row == tilesRow && hit.part == 2, "The tile is not hit");
        app.pressedPanelKind_ = hit.kind;
        app.pressedPanelRow_ = hit.row;
        app.pressedPanelPart_ = hit.part;
        app.pressedPanelId_ = tiles.id;
        app.pressedPanelParam_ = panelRowParam(tiles, hit.part);
        require(app.pressedPanelParam_ == 2, "The press did not take the tile's item");
        app.releasePanelPress(onTile);
        require(app.paths_[0] == L"emby://srv/b", "Releasing on the tile did not play its item");
        // The playing item is marked in the grid (now docked beside the
        // video, so fewer tiles per line) and in the list.
        app.refreshPanelGeometry(client);
        int markedTiles = 0;
        for (const auto& row : app.panelRows_) {
            if (row.kind != PanelRowKind::Tiles || row.selected < 0) continue;
            ++markedTiles;
            require(panelRowParam(row, row.selected) == 2, "The wrong tile is marked as playing");
        }
        require(markedTiles == 1, "Exactly one tile must be marked as playing");
        app.embySetView(0);
        app.refreshPanelGeometry(client);
        int marked = 0;
        for (const auto& row : app.panelRows_) {
            if (row.id != SettingId::EmbyItem) continue;
            if (row.current) {
                ++marked;
                require(row.param == 2 && row.value == L"\x25B6 2%   23:40", "The wrong row is marked, or its value lost the length");
            }
        }
        require(marked == 1, "Exactly one list row must be the playing one");
        app.embySetView(2);
        require(playbackQueue(app).items.size() == 3 && playbackQueue(app).items[1].id == "b", "The playlist is not the shown list");
        // Page Down steps to the next shown item without asking the server;
        // the list's end is the end.
        require(app.embyPanes_[0] && app.embyPanes_[0]->itemId == "b", "The pane does not hold the item");
        app.embyOpenAdjacent(0, 1);
        require(app.paths_[0] == L"emby://srv/c", "Page Down did not step along the list");
        app.embyOpenAdjacent(0, 1);
        require(app.paths_[0] == L"emby://srv/c", "Page Down went past the end of the list");
        app.embyOpenAdjacent(0, -1);
        require(app.paths_[0] == L"emby://srv/b", "Page Up did not step back along the list");
        // PotPlayer's sort keys re-order the list being played at once:
        // Ctrl+6 is size (largest first), pressed again the other way,
        // Ctrl+4 back to names; the browser's own order follows too.
        const auto playlistIds = [&] {
            std::string ids;
            for (const auto& entry : playbackQueue(app).items) ids += entry.id;
            return ids;
        };
        app.sortListBy(static_cast<int>(emby::SortKey::Size));
        require(app.embyBrowser_.sort == 6 && app.embyBrowser_.descending && playlistIds() == "acb",
                "Size did not sort the playlist largest first");
        app.sortListBy(static_cast<int>(emby::SortKey::Size));
        require(!app.embyBrowser_.descending && playlistIds() == "bca", "Size again did not reverse the playlist");
        app.sortListBy(static_cast<int>(emby::SortKey::Name));
        require(app.embyBrowser_.sort == 0 && playlistIds() == "abc", "Name did not sort the playlist");
        app.sortListBy(static_cast<int>(emby::SortKey::Random));
        require(playbackQueue(app).items.size() == 3, "Random lost items");
        // A list arriving from the server that holds the playing item
        // becomes the playlist, in the server's order.
        app.embyItems_ = {video("c", "Trip C"), video("b", "Trip B"), video("a", "Trip A")};
        app.embyAdoptPlaylist();
        require(playlistIds() == "cba", "The list on screen did not become the playlist");
        app.embyItems_ = {video("x", "Other")};
        app.embyAdoptPlaylist();
        require(playlistIds() == "cba", "A list without the playing item replaced the playlist");
        app.embyItems_ = {video("a", "Trip A", 300), folder, video("b", "Trip B", 100), video("c", "Trip C", 200)};
        // The list view names the same items as rows.
        app.embySetView(0);
        app.refreshPanelGeometry(client);
        int itemRows = 0;
        for (const auto& row : app.panelRows_) {
            require(row.kind != PanelRowKind::Tiles, "The list view still shows tiles");
            if (row.id == SettingId::EmbyItem) ++itemRows;
        }
        require(itemRows == 4, "The list view does not show every item");
        // Choosing the chosen order again reverses it; another order takes
        // its natural direction.
        app.embyBrowser_.sort = 0;
        app.embyBrowser_.descending = false;
        app.embySetSort(0);
        require(app.embyBrowser_.descending, "Choosing the order again did not reverse it");
        app.embySetSort(1);
        require(app.embyBrowser_.sort == 1 && app.embyBrowser_.descending, "Added did not start newest first");
        app.embySetSort(4);
        app.embySetSort(4);
        require(app.embyBrowser_.sort == 4 && !app.embyBrowser_.descending, "Random has no direction to flip");
        // Signed out, the browser is the sign-in page.
        app.embySignOut();
        app.embyBrowserOpen_ = true;
        app.refreshPanelGeometry(client);
        bool signIn = false;
        for (const auto& row : app.panelRows_) {
            signIn = signIn || (row.id == SettingId::EmbyAccount && !row.options.empty() && row.options[0] == L"Sign in\x2026");
            require(row.kind != PanelRowKind::Tiles, "A signed-out browser still shows tiles");
        }
        require(signIn, "The signed-out browser offers no sign-in");
        require(app.thumbnails_.size() == 0 && playbackQueue(app).items.empty(), "Signing out kept the account's pictures or list");
    }

    // An Emby playlist opens in the order it was put together, whatever the
    // browser's order is, comes whole, and is what Page Up/Down and the
    // playback order walk -- by place, since it may hold a video twice.
    static void embyPlaylistPlaysInItsOwnOrder() {
        App app;
        HiddenWindow parent;
        app.window_ = app.videoWindow_ = parent.window;
        app.instance_ = GetModuleHandleW(nullptr);
        emby::Session session;
        session.serverUrl = "http://127.0.0.1:9";
        session.serverId = "srv";
        session.userId = "u1";
        session.token = "token";
        session.deviceId = "dev";
        app.embyConfigure(session);
        const auto authDirectory = std::filesystem::temp_directory_path() / L"QuadDeckEmbyPlaylistTest";
        std::error_code code;
        std::filesystem::create_directories(authDirectory, code);
        app.embyAuthPathOverride_ = authDirectory / L"emby.qauth";
        app.settingsOpen_ = true;
        app.settingsAlpha_ = 1.0F;
        app.embyBrowserOpen_ = true;
        app.embyPages_.push_back(App::EmbyPage{});
        App::EmbyPage library;
        library.kind = App::EmbyPage::Kind::Library;
        library.id = "39239";
        library.title = "Playlists";
        library.collectionType = "playlists";
        library.itemType = "CollectionFolder";
        app.embyPages_.push_back(library);
        emby::Item playlist;
        playlist.id = "119996";
        playlist.name = "Evening";
        playlist.type = "Playlist";
        playlist.isFolder = true;
        playlist.childCount = 403;
        playlist.runTimeTicks = 797109933340;   // 22:08:30
        app.embyItems_ = {playlist};
        // The browser's own arrangement, which a playlist does not take:
        // last played first, folders left out.
        app.embyBrowser_.view = 2;
        app.embyBrowser_.sort = static_cast<int>(emby::SortKey::LastPlayed);
        app.embyBrowser_.descending = true;
        app.embyBrowser_.flat = true;
        const RECT client{0, 0, 1280, 720};
        const auto rowWith = [&](SettingId id) -> const PanelRow* {
            for (const auto& row : app.panelRows_) if (row.id == id) return &row;
            return nullptr;
        };
        app.refreshPanelGeometry(client);
        require(rowWith(SettingId::EmbyFlat) == nullptr, "The library of playlists offers to leave its folders out");
        const PanelRow* tiles = nullptr;
        for (const auto& row : app.panelRows_) if (row.kind == PanelRowKind::Tiles) tiles = &row;
        require(tiles && tiles->tileBadges.size() == 1 && tiles->tileBadges[0] == L"403 items",
                "A playlist's tile does not say how much it holds");
        app.embySetView(0);
        app.refreshPanelGeometry(client);
        require(rowWith(SettingId::EmbyItem) && rowWith(SettingId::EmbyItem)->value == L"403 items   22:08:30",
                "A playlist's row does not say how much it holds and how long it is");
        app.embySetView(2);
        const std::string libraryPath = app.embyListPath(library, 0, 300);
        require(libraryPath.find("ChildCount") != std::string::npos &&
                libraryPath.find("Recursive") == std::string::npos &&
                libraryPath.find("SortBy=IsFolder%2CDatePlayed") != std::string::npos,
                "The library of playlists is not asked for as its playlists");

        // Opening the playlist asks for its entries with no order at all.
        app.embyActivateItem(0);
        require(app.embyPages_.size() == 3 && app.embyPages_.back().kind == App::EmbyPage::Kind::Playlist &&
                app.embyPages_.back().ownOrder && app.embyPages_.back().id == "119996",
                "Choosing a playlist did not open it as one");
        require(app.embyLoading_ && app.embyItems_.empty(), "The playlist's page did not ask for its entries");
        const std::string ownOrderPath = app.embyListPath(app.embyPages_.back(), 0, 1000);
        require(ownOrderPath.find("ParentId=119996") != std::string::npos &&
                ownOrderPath.find("SortBy") == std::string::npos &&
                ownOrderPath.find("SortOrder") == std::string::npos &&
                ownOrderPath.find("Recursive") == std::string::npos &&
                ownOrderPath.find("Limit=1000") != std::string::npos,
                "A playlist is not asked for in its own order");

        // Its first entries arrive; the rest follow without being asked
        // for, said under the list rather than above it.
        const auto reply = [](const std::string& items, int total) {
            EmbyClient::Response response;
            response.ok = true;
            response.status = 200;
            response.body = "{\"Items\":[" + items + "],\"TotalRecordCount\":" + std::to_string(total) + "}";
            return response;
        };
        const auto entry = [](const char* id, const char* place, const char* name) {
            return std::string("{\"Name\":\"") + name + "\",\"Id\":\"" + id + "\",\"PlaylistItemId\":\"" + place +
                   "\",\"Type\":\"Video\",\"MediaType\":\"Video\",\"IsFolder\":false,\"RunTimeTicks\":600000000}";
        };
        const std::uint64_t first = app.embyRequestSerial_;
        app.embyListArrived(first - 1, 0, reply(entry("zz", "9", "Stale"), 1));
        require(app.embyItems_.empty() && app.embyLoading_, "A reply to an earlier request was taken");
        // "a" is in the playlist twice, at its first and third places.
        app.embyListArrived(first, 0, reply(entry("a", "1", "Delta") + "," + entry("b", "2", "Charlie") + "," +
                                            entry("a", "3", "Delta"), 5));
        require(app.embyItems_.size() == 3 && app.embyTotal_ == 5, "The playlist's first entries did not arrive");
        require(app.embyRequestSerial_ == first + 1 && app.embyLoadingMore_ && !app.embyLoading_,
                "The rest of the playlist was not asked for");
        app.refreshPanelGeometry(client);
        bool loadingAbove = false;
        for (const auto& row : app.panelRows_) {
            loadingAbove = loadingAbove || (row.kind == PanelRowKind::Note && row.label == L"Loading\x2026");
        }
        require(!loadingAbove && rowWith(SettingId::EmbyMore) && !rowWith(SettingId::EmbyMore)->enabled &&
                rowWith(SettingId::EmbyMore)->options[0] == L"Loading more\x2026  (3 of 5)" &&
                &app.panelRows_.back() == rowWith(SettingId::EmbyMore),
                "A further page is not announced under the list");
        app.embyListArrived(first + 1, 3, reply(entry("c", "4", "Bravo") + "," + entry("d", "5", "Alpha"), 5));
        require(app.embyItems_.size() == 5 && !app.embyLoadingMore_ && app.embyRequestSerial_ == first + 1,
                "The playlist did not arrive whole, or asked for more than it holds");
        require(app.embyItems_[2].id == "a" && app.embyItems_[2].playlistItemId == "3" &&
                app.embyItems_[4].id == "d", "The playlist's entries are not in its order");

        // Its page: the playlist's order first among the orders and chosen,
        // the unplayed filter offered, the folders choice not.
        app.refreshPanelGeometry(client);
        const PanelRow* sort = rowWith(SettingId::EmbySort);
        require(sort && sort->options.size() == 9 && sort->options[0] == L"Playlist" &&
                sort->options[1] == L"Name" && sort->options[8] == L"Updated" && sort->selected == 0,
                "A playlist's page does not offer its own order first");
        require(rowWith(SettingId::EmbyUnplayed) != nullptr && rowWith(SettingId::EmbyFlat) == nullptr,
                "A playlist's page offers the wrong filters");
        require(rowWith(SettingId::EmbyMore) == nullptr, "A playlist that is whole still offers more");

        // The second "a" is chosen: the list is the playlist as shown and
        // the place played is the third.
        app.embyActivateItem(2);
        require(app.paths_[0] == L"emby://srv/a" && playbackQueue(app).items.size() == 5 && playbackQueue(app).cursor == 2 &&
                playbackQueue(app).page == "playlist:119996", "Choosing an entry did not take the playlist as the list");
        app.embyOpenAdjacent(0, 1);
        require(app.paths_[0] == L"emby://srv/c" && playbackQueue(app).cursor == 3,
                "Page Down after the second of two alike went to what follows the first");
        app.embyOpenAdjacent(0, -1);
        app.embyOpenAdjacent(0, -1);
        require(app.paths_[0] == L"emby://srv/b" && playbackQueue(app).cursor == 1, "Page Up did not walk the playlist back");
        app.embyOpenAdjacent(0, -1);
        app.embyOpenAdjacent(0, -1);
        require(app.paths_[0] == L"emby://srv/a" && playbackQueue(app).cursor == 0, "Page Up went past the first entry");
        // The playback order walks the same places: in order through both
        // of the two alike to the end, and round again.
        app.playOrder_ = PlayOrder::InOrder;
        std::string walked;
        for (int i = 0; i < 6; ++i) {
            require(!app.finishSingleVideo(), "An Emby pane took the whole-deck end policy");
            simulateEmbyEnd(app);
            walked += app.embyPanes_[0] ? app.embyPanes_[0]->itemId : std::string("-");
        }
        require(walked == "bacddd" && playbackQueue(app).cursor == 4, "In order did not play the playlist through");
        app.playOrder_ = PlayOrder::RepeatList;
        simulateEmbyEnd(app);
        require(playbackQueue(app).cursor == 0 && app.paths_[0] == L"emby://srv/a",
                "Repeat the list did not wrap to the playlist's first entry");

        // Another page that happens to hold the playing video is looked
        // at: the playlist stays the list.
        App::EmbyPage folder;
        folder.kind = App::EmbyPage::Kind::Folder;
        folder.id = "102534";
        folder.itemType = "Folder";
        app.embyPages_.push_back(folder);
        const auto video = [](const char* id, const char* name, const char* place = "") {
            emby::Item item;
            item.id = id;
            item.name = name;
            item.type = "Video";
            item.mediaType = "Video";
            item.playlistItemId = place;
            return item;
        };
        app.embyItems_ = {video("x", "Other"), video("a", "Delta"), video("y", "Another")};
        app.embyAdoptPlaylist();
        require(playbackQueue(app).items.size() == 5 && playbackQueue(app).page == "playlist:119996" &&
                playbackQueue(app).cursor == 0, "A folder holding the playing video replaced the playlist");
        app.embyPages_.pop_back();
        // A refreshed page may change its server order; without an explicit
        // sort, the playing queue and repeated entry's identity stay fixed.
        app.embyOpenAdjacent(0, 1);
        app.embyOpenAdjacent(0, 1);
        require(playbackQueue(app).cursor == 2 && app.paths_[0] == L"emby://srv/a", "Premise: the second of two alike plays");
        app.embyItems_ = {video("d", "Alpha", "5"), video("a", "Delta", "1"), video("b", "Charlie", "2"),
                          video("c", "Bravo", "4"), video("a", "Delta", "3")};
        app.embyAdoptPlaylist();
        require(detailItemIds(playbackQueue(app).items) == std::vector<std::string>{"a", "b", "a", "c", "d"} &&
                playbackQueue(app).cursor == 2 && playbackQueue(app).items[2].playlistItemId == "3",
                "A refreshed server order changed the queue without an explicit sort or lost the repeated entry");

        // An order of the browser's, chosen on the page, gives the
        // playlist's up: the first choice sorts by it without reversing.
        app.embyBrowser_.sort = static_cast<int>(emby::SortKey::Name);
        app.embyBrowser_.descending = false;
        app.embyChooseSort(1);
        require(!app.embyPages_.back().ownOrder && app.embyBrowser_.sort == 0 && !app.embyBrowser_.descending,
                "Choosing Name on a playlist reversed it instead of sorting by it");
        const std::string byName = app.embyListPath(app.embyPages_.back(), 0, 1000);
        require(byName.find("SortBy=SortName&SortOrder=Ascending") != std::string::npos,
                "A playlist sorted by name is not asked for by name");
        app.refreshPanelGeometry(client);
        require(rowWith(SettingId::EmbySort) && rowWith(SettingId::EmbySort)->selected == 1,
                "The order chosen on a playlist is not the one marked");
        app.embyChooseSort(1);
        require(app.embyBrowser_.descending, "Choosing the order again did not reverse it");
        app.embyChooseSort(6);
        require(app.embyBrowser_.sort == static_cast<int>(emby::SortKey::LastPlayed) && !app.embyPages_.back().ownOrder,
                "The playlist's segments do not name the browser's orders");
        app.embyChooseSort(0);
        require(app.embyPages_.back().ownOrder &&
                app.embyListPath(app.embyPages_.back(), 0, 1000).find("SortBy") == std::string::npos,
                "Choosing Playlist did not return to the playlist's own order");
        // PotPlayer's sort key on a playlist being played: the list is
        // sorted at once, the entry playing stays the one playing, and the
        // page follows.
        app.embyBrowser_.sort = static_cast<int>(emby::SortKey::LastPlayed);
        app.sortListBy(static_cast<int>(emby::SortKey::Name));
        std::string names;
        for (const auto& item : playbackQueue(app).items) names += item.id;
        require(names == "dcbaa" && !app.embyPages_.back().ownOrder && !app.embyBrowser_.descending,
                "Ctrl+4 did not sort the playlist being played by name");
        require(playbackQueue(app).cursor == 4 && playbackQueue(app).items[4].playlistItemId == "3",
                "Sorting lost which of two alike is playing");
        // Signing out forgets the list with the account.
        app.embySignOut();
        require(playbackQueue(app).items.empty() && playbackQueue(app).cursor == -1 && playbackQueue(app).page.empty(),
                "Signing out kept the account's playlist");
    }

    // A library that is walked by folder can be shown without them: every
    // video under the page as one list. The rest of a long list comes as
    // the scroll nears its end, and a failure is not asked for every frame.
    static void embyLibraryShowsWithoutItsFolders() {
        App app;
        HiddenWindow parent;
        app.window_ = app.videoWindow_ = parent.window;
        app.instance_ = GetModuleHandleW(nullptr);
        emby::Session session;
        session.serverUrl = "http://127.0.0.1:9";
        session.serverId = "srv";
        session.userId = "u1";
        session.token = "token";
        session.deviceId = "dev";
        app.embyConfigure(session);
        app.settingsOpen_ = true;
        app.settingsAlpha_ = 1.0F;
        app.embyBrowserOpen_ = true;
        app.embyPages_.push_back(App::EmbyPage{});
        emby::Item view;
        view.id = "47096";
        view.name = "Home videos";
        view.type = "CollectionFolder";
        view.collectionType = "homevideos";
        view.isFolder = true;
        app.embyItems_ = {view};
        app.embyBrowser_.view = 0;
        app.embyActivateItem(0);
        require(app.embyPages_.size() == 2 && app.embyPages_.back().kind == App::EmbyPage::Kind::Library &&
                app.embyPages_.back().itemType == "CollectionFolder", "The library did not open");
        const std::uint64_t opened = app.embyRequestSerial_;
        const auto reply = [](const std::string& items, int total) {
            EmbyClient::Response response;
            response.ok = true;
            response.status = 200;
            response.body = "{\"Items\":[" + items + "],\"TotalRecordCount\":" + std::to_string(total) + "}";
            return response;
        };
        const auto videos = [](int from, int count) {
            std::string items;
            for (int i = from; i < from + count; ++i) {
                if (!items.empty()) items += ',';
                items += "{\"Name\":\"Clip " + std::to_string(i) + "\",\"Id\":\"v" + std::to_string(i) +
                         "\",\"Type\":\"Video\",\"MediaType\":\"Video\",\"IsFolder\":false}";
            }
            return items;
        };
        app.embyListArrived(opened, 0, reply(
            "{\"Name\":\"Trips\",\"Id\":\"f1\",\"Type\":\"Folder\",\"IsFolder\":true}," + videos(0, 1), 2));
        const RECT client{0, 0, 1280, 720};
        const auto rowIndex = [&](SettingId id) {
            for (std::size_t i = 0; i < app.panelRows_.size(); ++i) {
                if (app.panelRows_[i].id == id) return static_cast<int>(i);
            }
            return -1;
        };
        app.refreshPanelGeometry(client);
        const int showRow = rowIndex(SettingId::EmbyFlat);
        require(showRow >= 0, "A library walked by folder does not offer to leave them out");
        {
            const auto& row = app.panelRows_[static_cast<std::size_t>(showRow)];
            require(row.kind == PanelRowKind::Choice && row.options.size() == 2 && row.options[0] == L"Folders" &&
                    row.options[1] == L"All videos" && row.selected == 0, "The choice is not Folders / All videos");
        }
        require(app.embyListPath(app.embyPages_.back(), 0, 300).find("SortBy=IsFolder%2CSortName") != std::string::npos,
                "By folder, the library is not asked for folders first");
        // A press on "All videos", released there, asks for the library again.
        const auto& segment = app.panelLayout_.rows[static_cast<std::size_t>(showRow)].parts[1];
        const POINT onSegment{static_cast<LONG>(segment.x + segment.width / 2.0F),
                              static_cast<LONG>(segment.y + segment.height / 2.0F)};
        const PanelHit hit = app.panelHitAt(onSegment);
        require(hit.kind == PanelHitKind::Segment && hit.row == showRow && hit.part == 1, "All videos is not hit");
        app.pressedPanelKind_ = hit.kind;
        app.pressedPanelRow_ = hit.row;
        app.pressedPanelPart_ = hit.part;
        app.pressedPanelId_ = SettingId::EmbyFlat;
        app.pressedPanelParam_ = panelRowParam(app.panelRows_[static_cast<std::size_t>(showRow)], hit.part);
        app.releasePanelPress(onSegment);
        require(app.embyBrowser_.flat && app.embyLoading_ && app.embyItems_.empty() &&
                app.embyRequestSerial_ == opened + 1, "Choosing All videos did not ask for the library again");
        const std::string flatPath = app.embyListPath(app.embyPages_.back(), 0, 300);
        require(flatPath.find("Recursive=true") != std::string::npos &&
                flatPath.find("IncludeItemTypes=Video%2CPhoto%2CMovie%2CEpisode") != std::string::npos &&
                flatPath.find("IsFolder") == std::string::npos, "All videos is not asked for as every video under it");
        require(app.captureAppSettings().embyBrowser.flat, "The choice is not saved with the settings");
        // Choosing what is chosen asks for nothing.
        app.embySetFlat(true);
        require(app.embyRequestSerial_ == opened + 1, "Choosing the same again asked the server again");

        // Its first page of many: nothing more is asked for at the top.
        app.embyListArrived(opened + 1, 0, reply(videos(0, 300), 12903));
        require(app.embyItems_.size() == 300 && app.embyTotal_ == 12903 && !app.embyLoading_, "The first page did not arrive");
        app.refreshPanelGeometry(client);
        require(app.panelLayout_.maxScroll > 720.0F, "Premise: the list is longer than the sheet");
        const int moreRow = rowIndex(SettingId::EmbyMore);
        require(!app.embyLoadingMore_ && app.embyRequestSerial_ == opened + 1 && moreRow >= 0 &&
                app.panelRows_[static_cast<std::size_t>(moreRow)].enabled &&
                app.panelRows_[static_cast<std::size_t>(moreRow)].options[0] == L"Show more  (300 of 12903)",
                "More was asked for before the list was scrolled");
        // Scrolled to within a sheet of its end, the next page is.
        app.settingsScroll_ = app.panelLayout_.maxScroll;
        app.refreshPanelGeometry(client);
        require(app.embyLoadingMore_ && !app.embyLoading_ && app.embyRequestSerial_ == opened + 2,
                "Nearing the end of the list did not ask for more");
        const float scrolled = app.settingsScroll_;
        app.refreshPanelGeometry(client);
        require(app.embyRequestSerial_ == opened + 2, "More was asked for again while it was on its way");
        require(rowIndex(SettingId::EmbyMore) == moreRow &&
                !app.panelRows_[static_cast<std::size_t>(moreRow)].enabled && app.settingsScroll_ == scrolled,
                "Asking for more moved the list or left the button to be pressed again");
        app.embyListArrived(opened + 2, 300, reply(videos(300, 300), 12903));
        require(app.embyItems_.size() == 600 && app.embyItems_[300].id == "v300" && !app.embyLoadingMore_,
                "The next page was not added to the list");
        require(app.settingsScroll_ == scrolled, "The next page arriving moved the list");
        // A page that fails is asked for again by the button, not by
        // every frame the list stays scrolled to its end.
        app.settingsScroll_ = 1.0e9F;
        app.refreshPanelGeometry(client);
        require(app.embyLoadingMore_ && app.embyRequestSerial_ == opened + 3, "The third page was not asked for");
        EmbyClient::Response failed;
        failed.error = "The server is away";
        app.embyListArrived(opened + 3, 600, failed);
        require(!app.embyLoadingMore_ && app.embyAutoMoreBlocked_ && app.embyItems_.size() == 600,
                "A failed page was not given up");
        app.refreshPanelGeometry(client);
        app.refreshPanelGeometry(client);
        require(app.embyRequestSerial_ == opened + 3 && rowIndex(SettingId::EmbyMore) >= 0 &&
                app.panelRows_[static_cast<std::size_t>(rowIndex(SettingId::EmbyMore))].enabled,
                "A failed page is asked for again every frame, or cannot be asked for by hand");
        app.embyLoadMore();
        require(app.embyLoadingMore_ && !app.embyAutoMoreBlocked_ && app.embyRequestSerial_ == opened + 4,
                "Show more did not ask again");
        app.embyListArrived(opened + 4, 600, reply(videos(600, 2), 12903));
        require(app.embyItems_.size() == 602, "Show more did not bring the page");

        // Back by folder; inside a folder the choice is offered too, in a
        // box set or the libraries of box sets and playlists it is not,
        // and being on does not reach them.
        app.embySetFlat(false);
        require(!app.embyBrowser_.flat && app.embyRequestSerial_ == opened + 5, "Folders did not ask for the library again");
        app.embyBrowser_.flat = true;
        const auto offered = [&](App::EmbyPage::Kind kind, const char* collectionType, const char* itemType) {
            App::EmbyPage page;
            page.kind = kind;
            page.id = "p";
            page.collectionType = collectionType;
            page.itemType = itemType;
            app.embyPages_.push_back(page);
            const bool flat = app.embyPageAcceptsFlat();
            const bool paged = app.embyPageIsPaged();
            app.embyPages_.pop_back();
            // Series and movies are listed recursively by their one type;
            // nothing else that does not offer the choice is flattened.
            const std::string path = paged ? app.embyListPath(page, 0, 300) : std::string();
            require(flat || path.find("IncludeItemTypes=Video") == std::string::npos,
                    "A page that does not offer the choice was flattened");
            return flat;
        };
        require(offered(App::EmbyPage::Kind::Folder, "", "Folder"), "A folder does not offer All videos");
        require(!offered(App::EmbyPage::Kind::Folder, "", "BoxSet"), "A box set offers All videos");
        require(!offered(App::EmbyPage::Kind::Library, "boxsets", "CollectionFolder"), "The box sets offer All videos");
        require(!offered(App::EmbyPage::Kind::Library, "playlists", "CollectionFolder"), "The playlists offer All videos");
        require(!offered(App::EmbyPage::Kind::Library, "movies", "CollectionFolder"), "Movies offer All videos");
        require(!offered(App::EmbyPage::Kind::Playlist, "", "Playlist"), "A playlist offers All videos");
        require(!offered(App::EmbyPage::Kind::Season, "", "Season"), "A season offers All videos");
    }

    // Movie/TV metadata remains usable in a mixed list and in Continue
    // watching, where an episode number alone does not identify its show.
    static void embyMovieAndTelevisionRowsKeepTheirContext() {
        App app;
        emby::Session session;
        // Deliberately not a URL: these fixtures cannot contact any server.
        session.serverUrl = "synthetic fixture address";
        session.serverId = "srv";
        session.userId = "fixture-user";
        session.token = "fixture-token";
        session.deviceId = "fixture-device";
        app.embyConfigure(session);
        app.embyPages_.push_back(App::EmbyPage{});
        App::EmbyPage search;
        search.kind = App::EmbyPage::Kind::Search;
        search.term = "Aurora";
        app.embyPages_.push_back(search);
        const auto fixture = emby::parseItems(R"({"Items":[
            {"Id":"movie","Name":"Aurora","Type":"Movie","MediaType":"Video","ProductionYear":2020},
            {"Id":"series","Name":"Aurora","Type":"Series","IsFolder":true,"ProductionYear":2021,
             "UserData":{"UnplayedItemCount":7}},
            {"Id":"episode","Name":"The Return","Type":"Episode","MediaType":"Video","SeriesId":"series",
             "SeriesName":"Aurora","SeasonId":"season","ParentIndexNumber":2,"IndexNumber":3,"IndexNumberEnd":4,
             "RunTimeTicks":18000000000,"UserData":{"PlaybackPositionTicks":4500000000,"Played":false}},
            {"Id":"season","Name":"","Type":"Season","IsFolder":true,"IndexNumber":2,"ChildCount":8,
             "UserData":{"UnplayedItemCount":3}},
            {"Id":"old-movie","Name":"Legacy film","Type":"Movie","ProductionYear":null},
            {"Id":"old-series","Name":"Legacy show","Type":"Series","IsFolder":true},
            {"Id":"video","Name":"Family clip","Type":"Video","MediaType":"Video"},
            {"Id":"photo","Name":"Family photo","Type":"Photo","MediaType":"Photo"}
        ],"TotalRecordCount":8})");
        require(fixture && fixture->items.size() == 8, "Cannot parse the movie/TV row fixture");
        app.embyItems_ = fixture->items;
        app.embyBrowser_.view = 0;
        auto rows = app.buildEmbyRows(1280.0F);
        const auto listRow = [&](int param) -> PanelRow {
            for (const auto& row : rows) {
                if (row.id == SettingId::EmbyItem && row.kind == PanelRowKind::Item && row.param == param) return row;
            }
            throw std::runtime_error("The mixed movie/TV list lost an item");
        };
        require(listRow(0).label == L"Aurora (2020)" && listRow(1).label == L"Aurora (2021)",
                "Movie and series rows lost their production years");
        const auto episode = listRow(2);
        require(episode.label == L"S2E3\u2013E4 \u00B7 Aurora \u00B7 The Return",
                "A mixed episode row lost its code-first show, season or title");
        require(episode.value == L"\x25B6 25%   30:00" && episode.on,
                "An episode lost its resume progress or runtime");
        require(listRow(3).label == L"Season 2" && listRow(3).value.find(L"8 episodes") != std::wstring::npos &&
                listRow(3).value.find(L"3 new") != std::wstring::npos,
                "A nameless season lost its number, episode count or unplayed count");
        require(listRow(4).label == L"Legacy film" && listRow(5).label == L"Legacy show" &&
                listRow(5).value == L"Series", "Missing or null metadata has no usable fallback");
        require(listRow(6).label == L"Family clip" && listRow(7).label == L"Family photo" &&
                listRow(7).value == L"photo", "Movie/TV labels changed home-video or photo rows");
        // Both grid arrangements carry the same title and keep watched
        // state separate from length and from a season's unplayed count.
        for (int view : {1, 2}) {
            app.embyBrowser_.view = view;
            rows = app.buildEmbyRows(1280.0F);
            bool foundEpisode = false, foundSeason = false;
            for (const auto& row : rows) {
                if (row.kind != PanelRowKind::Tiles) continue;
                for (std::size_t part = 0; part < row.tileParams.size(); ++part) {
                    if (row.tileParams[part] == 2) {
                        foundEpisode = true;
                        require(row.options[part] == episode.label && row.tileBadges[part] == L"30:00" &&
                                row.tileMarks[part] == L"\x25B6 25%" && row.tileProgress[part] == 0.25F,
                                "An episode tile lost its contextual title, runtime or progress");
                    } else if (row.tileParams[part] == 3) {
                        foundSeason = true;
                        require(row.options[part] == L"Season 2" && row.tileBadges[part] == L"3 new",
                                "A season tile still presents the season as a generic folder");
                    }
                }
            }
            require(foundEpisode && foundSeason, "A grid arrangement omitted movie/TV items");
        }
        // A long show name previously hid every episode code in a 320-DIP
        // dock. Adjacent episodes must remain distinct at the label's start.
        auto longEpisode = fixture->items[2];
        longEpisode.id = "long-episode";
        longEpisode.seriesName = "Star Trek: The Next Generation";
        auto longNext = longEpisode;
        longNext.id = "long-next";
        longNext.name = "Next chapter";
        longNext.indexNumber = 5;
        longNext.indexNumberEnd = -1;
        auto unnumberedEpisode = longEpisode;
        unnumberedEpisode.id = "unnumbered-episode";
        unnumberedEpisode.name = "Unnumbered first";
        unnumberedEpisode.indexNumber = unnumberedEpisode.indexNumberEnd = -1;
        auto unnumberedNext = unnumberedEpisode;
        unnumberedNext.id = "unnumbered-next";
        unnumberedNext.name = "Unnumbered next";
        app.embyItems_ = {longEpisode, longNext, unnumberedEpisode, unnumberedNext};
        const std::array<std::wstring, 4> longLabels{
            L"S2E3\u2013E4 \u00B7 Star Trek: The Next Generation \u00B7 The Return",
            L"S2E5 \u00B7 Star Trek: The Next Generation \u00B7 Next chapter",
            L"Unnumbered first \u00B7 Star Trek: The Next Generation",
            L"Unnumbered next \u00B7 Star Trek: The Next Generation"
        };
        for (int view : {0, 1, 2}) {
            app.embyBrowser_.view = view;
            rows = app.buildEmbyRows(320.0F);
            if (view == 0) {
                for (std::size_t index = 0; index < longLabels.size(); ++index) {
                    require(listRow(static_cast<int>(index)).label == longLabels[index],
                            "Long-series list labels hide their leading episode codes or unnumbered names");
                }
            } else {
                int found = 0;
                for (const auto& row : rows) {
                    if (row.kind != PanelRowKind::Tiles) continue;
                    for (std::size_t part = 0; part < row.tileParams.size(); ++part) {
                        const int param = row.tileParams[part];
                        if (param < 0 || static_cast<std::size_t>(param) >= longLabels.size()) continue;
                        require(row.options[part] == longLabels[static_cast<std::size_t>(param)],
                                "Long-series tile labels hide their leading episode codes or unnumbered names");
                        ++found;
                    }
                }
                require(found == 4, "A narrow movie/TV grid lost one of the long-series episodes");
            }
        }
        app.embyPages_.resize(1);
        app.embyItems_.clear();
        app.embyResume_ = {fixture->items[2]};
        app.embyBrowser_.view = 0;
        rows = app.buildEmbyRows(1280.0F);
        require(listRow(-1).label == episode.label, "Continue watching lost the episode's series and season");
        auto unnamed = fixture->items[3];
        unnamed.indexNumber = -1;
        unnamed.childCount = unnamed.unplayedCount = -1;
        app.embyItems_ = {unnamed};
        rows = app.buildEmbyRows(1280.0F);
        require(listRow(0).label == L"Season" && listRow(0).value == L"Season",
                "A season with no name, number or counts has no fallback");
        unnamed.played = true;
        unnamed.positionTicks = 4500000000;
        unnamed.runTimeTicks = 18000000000;
        app.embyItems_ = {unnamed};
        for (int view : {1, 2}) {
            app.embyBrowser_.view = view;
            rows = app.buildEmbyRows(1280.0F);
            for (const auto& row : rows) {
                if (row.kind != PanelRowKind::Tiles) continue;
                for (std::size_t part = 0; part < row.tileParams.size(); ++part) {
                    if (row.tileParams[part] != 0) continue;
                    require(row.tileBadges[part] == L"Season" && row.tileMarks[part] == L"\x2713" &&
                            row.tileProgress[part] == 0.0F,
                            "A watched season lost its checkmark or drew a video's resume bar");
                }
            }
        }
    }

    // The show/season endpoints page in server order, and a Season whose
    // SeriesId was omitted still opens using its known parent or its own ID.
    static void embyTelevisionNavigationAndSearchReachEpisodes() {
        App app;
        HiddenWindow parent;
        app.window_ = app.videoWindow_ = parent.window;
        app.instance_ = GetModuleHandleW(nullptr);
        emby::Session session;
        session.serverUrl = "synthetic fixture address";
        session.serverId = "srv";
        session.userId = "fixture-user";
        session.token = "fixture-token";
        session.deviceId = "fixture-device";
        app.embyConfigure(session);
        app.embyPages_.push_back(App::EmbyPage{});
        app.embyBrowser_.sort = static_cast<int>(emby::SortKey::Random);
        app.embyBrowser_.flat = app.embyBrowser_.unplayed = true;
        emby::Item show;
        show.id = "show-a";
        show.name = "Aurora";
        show.type = "Series";
        show.isFolder = true;
        app.embyItems_ = {show};
        app.embyActivateItem(0);
        require(app.embyPages_.back().kind == App::EmbyPage::Kind::Series &&
                app.embyPages_.back().id == "show-a" && app.embyPageIsPaged(),
                "A series did not open as a paged season list");
        const auto requireTelevisionPage = [&] {
            require(!app.embyPageAcceptsFilters() && !app.embyPageAcceptsFlat(),
                    "A television page offers folder arrangement controls");
            for (const auto& row : app.buildEmbyRows(1280.0F)) {
                require(row.id != SettingId::EmbySort && row.id != SettingId::EmbyFlat &&
                        row.id != SettingId::EmbyUnplayed, "A television page shows sort or filter controls");
            }
        };
        requireTelevisionPage();
        const std::string seasons = app.embyListPath(app.embyPages_.back(), 300, 300);
        require(seasons.starts_with("/Shows/show-a/Seasons?UserId=fixture-user") &&
                seasons.find("StartIndex=300") != std::string::npos && seasons.find("Limit=300") != std::string::npos &&
                seasons.find("Fields=") != std::string::npos && seasons.find("ParentId") != std::string::npos &&
                seasons.find("SortBy=") == std::string::npos && seasons.find("Filters=") == std::string::npos,
                "The season request lost its metadata/pagination or server order");
        emby::Item season;
        season.id = "season-a";
        season.name = "Season 2";
        season.type = "Season";
        season.isFolder = true;
        app.embyItems_ = {season};
        app.embyActivateItem(0);
        require(app.embyPages_.back().kind == App::EmbyPage::Kind::Season &&
                app.embyPages_.back().id == "season-a" && app.embyPages_.back().seriesId == "show-a" &&
                app.embyPageIsPaged(), "A season without SeriesId did not use the current series");
        requireTelevisionPage();
        const std::string episodes = app.embyListPath(app.embyPages_.back(), 300, 300);
        require(episodes.starts_with("/Shows/show-a/Episodes?UserId=fixture-user") &&
                episodes.find("SeasonId=season-a") != std::string::npos &&
                episodes.find("StartIndex=300") != std::string::npos && episodes.find("Limit=300") != std::string::npos &&
                episodes.find("Fields=") != std::string::npos && episodes.find("SortBy=") == std::string::npos &&
                episodes.find("Filters=") == std::string::npos, "The episode request lost its season, metadata or server order");
        const auto reply = [](const char* body) {
            EmbyClient::Response response;
            response.ok = true;
            response.status = 200;
            response.body = body;
            return response;
        };
        const auto firstPage = app.embyRequestSerial_;
        app.embyListArrived(firstPage, 0, reply(R"({"Items":[
            {"Id":"e2","Name":"Second","Type":"Episode","SeriesId":"show-a","SeriesName":"Aurora","ParentIndexNumber":2,"IndexNumber":2},
            {"Id":"e1","Name":"First","Type":"Episode","SeriesId":"show-a","SeriesName":"Aurora","ParentIndexNumber":2,"IndexNumber":1}
        ],"TotalRecordCount":3})"));
        require(app.embyRequestSerial_ == firstPage + 1 && app.embyLoadingMore_,
                "A season stopped after its first page instead of requesting every episode");
        app.embyActivateItem(1);
        require(app.paths_[0] == L"emby://srv/e1" && playbackQueue(app).items.size() == 2 &&
                playbackQueue(app).cursor == 1, "Choosing an episode lost the shown order or playable locator");
        app.embyListArrived(app.embyRequestSerial_, 2, reply(R"({"Items":[
            {"Id":"e3","Name":"Third","Type":"Episode","SeriesId":"show-a","SeriesName":"Aurora","ParentIndexNumber":2,"IndexNumber":3}
        ],"TotalRecordCount":3})"));
        require(app.embyItems_.size() == 3 && app.embyItems_[0].id == "e2" && app.embyItems_[1].id == "e1" &&
                app.embyItems_[2].id == "e3", "Episode paging sorted, replaced or lost the server's entries");
        require(playbackQueue(app).items.size() == 3 && playbackQueue(app).cursor == 1 && !app.embyLoadingMore_ &&
                app.embyRequestSerial_ == firstPage + 1,
                "The remaining episodes did not extend the active playlist or stop at the season's end");
        app.embyOpenAdjacent(0, 1);
        require(app.paths_[0] == L"emby://srv/e3", "The next episode did not follow the paged list");
        App::EmbyPage search;
        search.kind = App::EmbyPage::Kind::Search;
        search.term = "Aurora";
        app.embyPages_ = {App::EmbyPage{}, search};
        app.embyLoading_ = false; // This manually installed Search fixture has already completed.
        season.parentId = "show-b";
        app.embyItems_ = {season};
        app.embyActivateItem(0);
        require(app.embyPages_.back().kind == App::EmbyPage::Kind::Season &&
                app.embyPages_.back().seriesId == "show-b", "A season outside its series ignored its ParentId");
        // Explicit identity from the item is stronger than the page from
        // which it happened to be opened, including a stale/mixed page.
        app.embyPages_ = {App::EmbyPage{}, App::EmbyPage{}};
        app.embyPages_.back().kind = App::EmbyPage::Kind::Series;
        app.embyPages_.back().id = "stale-show";
        season.seriesId = "explicit-show";
        app.embyItems_ = {season};
        app.embyActivateItem(0);
        require(app.embyPages_.back().seriesId == "explicit-show", "SeriesId lost precedence over other parent metadata");
        app.embyPages_.resize(2);
        season.seriesId.clear();
        app.embyItems_ = {season};
        app.embyActivateItem(0);
        require(app.embyPages_.back().seriesId == "show-b", "A season's ParentId lost precedence over the current page");
        app.embyPages_ = {App::EmbyPage{}, search};
        app.embyLoading_ = false; // Do not carry a previous Season request into the completed Search fixture.
        season.parentId.clear();
        app.embyItems_ = {season};
        const auto before = app.embyRequestSerial_;
        app.embyActivateItem(0);
        require(app.embyPages_.size() == 3 && app.embyPages_.back().kind == App::EmbyPage::Kind::Season &&
                app.embyPages_.back().id == "season-a" && app.embyRequestSerial_ == before + 1,
                "A season without parent metadata silently ignored the click");
        const std::string fallback = app.embyListPath(app.embyPages_.back(), 0, 300);
        require(fallback.starts_with("/Users/fixture-user/Items?") && fallback.find("ParentId=season-a") != std::string::npos &&
                fallback.find("IncludeItemTypes=Episode") != std::string::npos &&
                fallback.find("SortBy=IndexNumber%2CSortName") != std::string::npos &&
                fallback.find("SortOrder=Ascending%2CAscending") != std::string::npos &&
                fallback.find("Filters=") == std::string::npos,
                "A season with no series requested an invalid show endpoint or unrelated videos");
        const std::string searchPath = app.embySearchPath("tv library", "Aurora return");
        require(searchPath.find("ParentId=tv%20library") != std::string::npos &&
                searchPath.find("SearchTerm=Aurora%20return") != std::string::npos &&
                searchPath.find("Recursive=true") != std::string::npos && searchPath.find("Episode") != std::string::npos &&
                searchPath.find("Season") == std::string::npos && searchPath.find("Fields=") != std::string::npos,
                "Library search omitted episodes or lost its scope and metadata");
        app.embyListArrived(app.embyRequestSerial_, 0, reply(R"({"Items":[
            {"Id":"e1","Name":"First","Type":"Episode"}
        ],"TotalRecordCount":3})"));
        require(app.embyLoadingMore_, "An orphan season did not load the rest of its episode list");
        const auto failedPage = app.embyRequestSerial_;
        EmbyClient::Response failed;
        failed.error = "Synthetic next-page failure";
        app.embyListArrived(failedPage, 1, failed);
        require(!app.embyLoadingMore_ && app.embyAutoMoreBlocked_ && app.embyRequestSerial_ == failedPage,
                "A failed episode page kept loading or automatically retried");
        app.embyRequestPage(1);
        const auto emptyPage = app.embyRequestSerial_;
        app.embyListArrived(emptyPage, 1, reply(R"({"Items":[],"TotalRecordCount":3})"));
        require(!app.embyLoadingMore_ && app.embyAutoMoreBlocked_ && app.embyRequestSerial_ == emptyPage,
                "An empty episode page queued an endless continuation");
    }

    // Fresh position drives movie/episode resume independently of Played:
    // a completed item can be watched again and have valid resume progress.
    static void embyMovieAndEpisodeResumeUsesFreshProgress() {
        App app;
        HiddenWindow parent;
        app.window_ = app.videoWindow_ = parent.window;
        app.instance_ = GetModuleHandleW(nullptr);
        emby::Session session;
        session.serverUrl = "synthetic fixture address";
        session.serverId = "srv";
        session.userId = "fixture-user";
        session.token = "fixture-token";
        session.deviceId = "fixture-device";
        app.embyConfigure(session);
        for (const char* type : {"Movie", "Episode"}) {
            emby::Item item;
            item.id = "watched";
            item.name = "Fresh progress";
            item.type = type;
            item.mediaType = "Video";
            item.positionTicks = 300000000;
            app.closePane(0);
            app.embyPlayItem(item, 0);
            require(app.embyPanes_[0] && app.embyPanes_[0]->resumeTicks == item.positionTicks,
                    "Premise: choosing an item keeps its listed position until fresh data arrives");
            const auto serial = app.embyPanes_[0]->serial;
            EmbyClient::Response response;
            response.ok = true;
            response.status = 200;
            response.body = std::string(R"({"Id":"watched","Name":"Fresh progress","Type":")") + type +
                R"(","MediaType":"Video","RunTimeTicks":6000000000,
                "UserData":{"PlaybackPositionTicks":4200000000,"Played":true}})";
            app.embyItemArrived(serial - 1, response);
            require(app.embyPanes_[0]->resumeTicks == item.positionTicks, "An obsolete item reply changed resume");
            app.embyItemArrived(serial, response);
            require(app.embyPanes_[0] && app.embyPanes_[0]->resumeTicks == 4200000000,
                    "A rewatched movie or episode lost valid fresh progress because Played was true");
            app.closePane(0);
            app.embyPlayItem(item, 0);
            response.body = std::string(R"({"Id":"watched","Name":"Fresh progress","Type":")") + type +
                R"(","MediaType":"Video","RunTimeTicks":6000000000,
                "UserData":{"PlaybackPositionTicks":4200000000,"Played":false}})";
            app.embyItemArrived(app.embyPanes_[0]->serial, response);
            require(app.embyPanes_[0] && app.embyPanes_[0]->resumeTicks == 4200000000,
                    "A partially watched movie or episode ignored the server's latest position");
            app.closePane(0);
            app.embyPlayItem(item, 0);
            response.body = std::string(R"({"Id":"watched","Name":"Fresh progress","Type":")") + type +
                R"(","MediaType":"Video","RunTimeTicks":6000000000,
                "UserData":{"PlaybackPositionTicks":0,"Played":false}})";
            app.embyItemArrived(app.embyPanes_[0]->serial, response);
            require(app.embyPanes_[0] && app.embyPanes_[0]->resumeTicks == 0,
                    "A movie or episode marked as not started retained the list's old resume position");
        }
    }

    static EmbyClient::Response detailReply(std::string body) {
        EmbyClient::Response response;
        response.ok = true;
        response.status = 200;
        response.body = std::move(body);
        return response;
    }

    static void prepareDetailBrowser(App& app, const HiddenWindow& parent) {
        app.window_ = app.videoWindow_ = parent.window;
        app.instance_ = GetModuleHandleW(nullptr);
        emby::Session session;
        session.serverUrl = "synthetic fixture address";
        session.serverId = "detail-server";
        session.userId = "detail-user";
        session.token = "synthetic-token";
        session.deviceId = "synthetic-device";
        app.embyConfigure(session);
        app.settingsOpen_ = app.embyBrowserOpen_ = true;
        app.settingsAlpha_ = 1.0F;
        app.browserSource_ = App::BrowserSource::Emby;
        const auto libraries = emby::parseItems(R"({"Items":[
            {"Id":"movies-a","Name":"Synthetic movies","Type":"CollectionFolder","CollectionType":"movies"},
            {"Id":"movies-b","Name":"Synthetic movies","Type":"CollectionFolder","CollectionType":"movies"},
            {"Id":"television","Name":"Synthetic TV","Type":"CollectionFolder","CollectionType":"tvshows"},
            {"Id":"home-videos","Name":"Synthetic home videos","Type":"CollectionFolder","CollectionType":"homevideos"},
            {"Id":"unknown","Name":"Synthetic unknown","Type":"CollectionFolder"}
        ]})");
        require(libraries && libraries->items.size() == 5, "Cannot parse the synthetic detail libraries");
        app.embyViews_ = libraries->items;
        App::EmbyPage library;
        library.kind = App::EmbyPage::Kind::Library;
        library.id = "movies-a";
        library.title = "Synthetic movies";
        library.collectionType = "movies";
        app.embyPages_ = {App::EmbyPage{}, library};
    }

    static std::vector<std::string> detailItemIds(const std::vector<emby::Item>& items) {
        std::vector<std::string> result;
        for (const auto& item : items) result.push_back(item.id);
        return result;
    }

    static emby::Item multiPaneItem(const std::string& id) {
        emby::Item item;
        item.id = id;
        item.name = "Synthetic " + id;
        item.type = "Episode";
        item.mediaType = "Video";
        item.seriesId = "synthetic-show";
        item.runTimeTicks = emby::ticksFromSeconds(600.0);
        item.positionTicks = emby::ticksFromSeconds(90.0);
        return item;
    }

    static void seedLiveEmbyPane(App& app, std::size_t index, const emby::Item& item) {
        App::EmbyPane pane;
        bindEmbyAccount(app, pane);
        pane.itemId = item.id;
        pane.title = L"Synthetic pane " + std::to_wstring(index + 1);
        pane.serial = ++app.embyPaneSerial_;
        pane.mediaSourceId = "media-" + item.id;
        pane.playSessionId = "session-" + item.id;
        pane.runTimeTicks = item.runTimeTicks;
        pane.queue = emby::takeQueue({item}, 0, "synthetic-page-" + item.id);
        app.embyPanes_[index] = std::move(pane);
        app.paths_[index] = emby::formatLocator(app.emby_.session().serverId, item.id);
        app.sources_[index] = std::make_unique<VideoSource>();
    }

    static void browserLinkedSeeksAlignSourceSeconds() {
        App app;
        HiddenWindow parent;
        const auto queue = syntheticLocalQueue();
        prepareLocalBrowser(app, parent, queue);
        app.deviceRecoveryPending_ = false;
        app.embyBrowserOpen_ = false; // F5 playback rows, not the F6 list.
        seedLiveLocalPane(app, 0, queue, 0);
        seedLiveLocalPane(app, 2, queue, 1);
        App::EmbyPane photo;
        photo.photo = true;
        app.embyPanes_[3] = std::move(photo);
        app.paths_[3] = L"synthetic-Emby-photo";
        app.sources_[3] = std::make_unique<VideoSource>();
        AudioSeekTestAccess::seed(*app.sources_[3], 1.0);
        seedLiveEmbyPane(app, 4, multiPaneItem("linked-seek"));
        app.seekMode_ = SeekMode::Linked;
        app.audioMask_ = 0b00101;
        app.clock_.seek(73.0);
        app.clock_.pause();
        app.duration_ = 50.0; // Deliberately stale and below the chosen source time.
        app.syncAdjustments_[0] = -30.0;
        app.syncAdjustments_[2] = 90.0;
        app.startDelays_[2] = 2.0;
        app.playbackRates_[2] = 1.25;
        app.sourcePaused_[2] = true;
        app.sourcePausedTimes_[2] = 45.0;
        app.sourceAutoRepeat_[2] = true;

        const auto localQueue = localQueuePaths(app.localPanes_[2]->queue);
        const auto embyItem = app.embyPanes_[4]->itemId;
        const auto localGeneration = AudioSeekTestAccess::videoGeneration(*app.sources_[0]);
        const auto photoGeneration = AudioSeekTestAccess::videoGeneration(*app.sources_[3]);
        const auto offsets = app.syncAdjustments_;
        app.panelRows_ = app.buildSettingsRows(1280.0F);
        int seekRow = -1;
        for (std::size_t i = 0; i < app.panelRows_.size(); ++i) {
            if (app.panelRows_[i].id == SettingId::SeekMode) seekRow = static_cast<int>(i);
        }
        require(seekRow >= 0, "Browser seek row is missing");
        require(app.panelRows_[static_cast<std::size_t>(seekRow)].enabled,
                "Browser seek row remained disabled");
        require(app.panelRows_[static_cast<std::size_t>(seekRow)].selected == static_cast<int>(SeekMode::Independent),
                "Browser seek row did not display Independent by default");
        require(app.activeSeekMode() == SeekMode::Independent,
                "Browser playback no longer uses independent timelines");
        app.activatePanelHit({PanelHitKind::Segment, seekRow, static_cast<int>(SeekMode::Linked)});
        require(app.linkedBrowserSeekBars() && app.seekMode_ == SeekMode::Linked &&
                app.activeSeekMode() == SeekMode::Independent && app.clock_.position() == 73.0 &&
                app.syncAdjustments_ == offsets &&
                AudioSeekTestAccess::videoGeneration(*app.sources_[0]) == localGeneration,
                "Selecting browser Linked immediately sought or changed independent playback policy");

        // An unresolved Emby pane must not be left at its resume position
        // after the already-loaded panes have been aligned.
        app.seekFromSourceBar(0, 100.0);
        require(app.clock_.position() == 73.0 && app.syncAdjustments_ == offsets,
                "Linked seek partially aligned a deck with an unresolved Emby video");
        AudioSeekTestAccess::seed(*app.sources_[4], 360.0);
        // Stop-at-end holds are distinct from a user's explicit pane pause.
        app.localPanes_[0]->endHandled = true;
        app.sourcePaused_[0] = true;
        app.sourcePausedTimes_[0] = 120.0;
        app.embyPanes_[4]->endHandled = true;
        app.sourcePaused_[4] = true;
        app.sourcePausedTimes_[4] = 360.0;

        app.seekFromSourceBar(0, 100.0); // V1 is 120 s; V2 is 240 s.
        require(std::abs(app.clock_.position() - 100.0) < 0.001 &&
                std::abs(app.currentSourceTime(0) - 100.0) < 0.001 &&
                std::abs(app.currentSourceTime(2) - 100.0) < 0.001 &&
                std::abs(app.currentSourceTime(4) - 100.0) < 0.001 &&
                std::abs(app.sourcePausedTimes_[2] - 100.0) < 0.001 &&
                app.duration_ >= 100.0 && !app.seekBarrier_.active(),
                "A pane-bar click did not align every source to the clicked seconds");
        require(!app.sourcePaused_[0] && !app.localPanes_[0]->endHandled &&
                !app.sourcePaused_[4] && !app.embyPanes_[4]->endHandled,
                "Linked seek left a video paused after reviving it from EOF");
        require(AudioSeekTestAccess::videoGeneration(*app.sources_[3]) == photoGeneration,
                "Linked video seek cleared an Emby photo frame");
        require(localQueuePaths(app.localPanes_[2]->queue) == localQueue &&
                app.embyPanes_[4]->itemId == embyItem && app.audioMask_ == 0b00101 &&
                app.sourceAutoRepeat_[2] && paneRepeats(app.paneTiming(2), app.activeSeekMode()) &&
                app.sourcePaused_[2] && app.perPaneTimelines() &&
                AudioSeekTestAccess::videoGeneration(*app.sources_[0]) > localGeneration,
                "Linked browser seek changed a queue, pause, repeat, audio choice or seek policy");

        // The bottom rail uses source seconds too; one quarter of the longest
        // 360-second source means 90 seconds in every reachable pane.
        require(std::abs(app.barTime().position - 100.0) < 0.001 &&
                std::abs(app.barTime().duration - 360.0) < 0.001,
                "Linked browser bottom bar still showed the old master timeline");
        app.controlDrag_ = App::ControlDrag::MasterSeek;
        app.dragFraction_ = 0.25F;
        app.endControlDrag(true);
        require(std::abs(app.currentSourceTime(0) - 90.0) < 0.001 &&
                std::abs(app.currentSourceTime(2) - 90.0) < 0.001 &&
                std::abs(app.currentSourceTime(4) - 90.0) < 0.001 &&
                std::abs(app.barTime().position - 90.0) < 0.001,
                "Bottom-bar Linked seek kept arrival-time offsets");

        // The selected pane can be shorter than the linked target. The bottom
        // playhead and consecutive arrow seeks must still follow the group.
        app.controlDrag_ = App::ControlDrag::MasterSeek;
        app.dragFraction_ = 0.75F;
        app.endControlDrag(true);
        require(std::abs(app.currentSourceTime(0) - 120.0) < 0.001 &&
                std::abs(app.currentSourceTime(4) - 270.0) < 0.001 &&
                std::abs(app.barTime().position - 270.0) < 0.001,
                "Short selected video pinned the linked bottom playhead");
        app.seekRelative(5.0);
        app.seekRelative(5.0);
        require(std::abs(app.currentSourceTime(4) - 280.0) < 0.001 &&
                std::abs(app.barTime().position - 280.0) < 0.001,
                "Repeated linked arrows stalled at the short video's end");

        app.panelRows_ = app.buildSettingsRows(1280.0F);
        app.activatePanelHit({PanelHitKind::Segment, seekRow, static_cast<int>(SeekMode::Independent)});
        const double embyTime = app.currentSourceTime(4);
        app.seekFromSourceBar(0, 50.0);
        require(!app.linkedBrowserSeekBars() && app.seekMode_ == SeekMode::Linked &&
                std::abs(app.currentSourceTime(0) - 50.0) < 0.001 &&
                std::abs(app.currentSourceTime(4) - embyTime) < 0.001,
                "Returning to Independent moved a neighbour or lost the manual seek preference");

        // Device recovery retains paths but temporarily destroys decoders.
        // Linked navigation must save the common target for reopened panes.
        app.panelRows_ = app.buildSettingsRows(1280.0F);
        app.activatePanelHit({PanelHitKind::Segment, seekRow, static_cast<int>(SeekMode::Linked)});
        app.seekFromSourceBar(4, 270.0);
        app.deviceRecoveryPending_ = true;
        app.deviceRecoveryPosition_ = app.clock_.position();
        app.deviceRecoveryBrowserPosition_ = app.currentSourceTime(0);
        app.deviceRecoverySourceDurations_[0] = 120.0;
        app.deviceRecoverySourceDurations_[2] = 240.0;
        app.deviceRecoverySourceDurations_[4] = 360.0;
        app.duration_ = 50.0; // Old master extent must not limit source-local seeks.
        for (auto& source : app.sources_) source.reset();
        app.embyPanes_[4]->endHandled = true;
        app.sourcePaused_[4] = true;
        app.seekRelative(5.0);
        require(std::abs(app.deviceRecoveryPosition_ - 275.0) < 0.001 &&
                std::abs(app.syncAdjustments_[0] - (120.0 - 275.0)) < 0.001 &&
                std::abs(app.syncAdjustments_[2] - (240.0 - 273.0 * 1.25)) < 0.001 &&
                std::abs(app.sourcePausedTimes_[2] - 240.0) < 0.001 &&
                std::abs(app.barTime().duration - 360.0) < 0.001 &&
                !app.sourcePaused_[4] && !app.embyPanes_[4]->endHandled,
                "Linked recovery lost the source durations or clamped to the old master extent");
        app.controlDrag_ = App::ControlDrag::MasterSeek;
        app.dragFraction_ = 0.5F;
        app.endControlDrag(true);
        require(std::abs(app.deviceRecoveryPosition_ - 180.0) < 0.001 &&
                std::abs(app.syncAdjustments_[0] - (120.0 - 180.0)) < 0.001 &&
                std::abs(app.syncAdjustments_[2] - (180.0 - 178.0 * 1.25)) < 0.001,
                "Linked bottom bar stopped working while the device recovers");
    }

    static void independentBrowserPanesHideFalseAggregateTime() {
        App app;
        HiddenWindow parent;
        app.window_ = app.videoWindow_ = parent.window;
        MoveWindow(parent.window, 0, 0, 1280, 720, FALSE);
        app.layoutMode_ = LayoutMode::SideBySide;
        app.dockProgress_ = 1.0F;
        app.paneChromeAlpha_.fill(1.0F);
        seedLiveEmbyPane(app, 0, multiPaneItem("short-duration"));
        seedLiveEmbyPane(app, 1, multiPaneItem("long-duration"));
        AudioSeekTestAccess::seed(*app.sources_[0], 120.0);
        AudioSeekTestAccess::seed(*app.sources_[1], 600.0);
        app.clock_.seek(1800.0);
        app.clock_.pause();
        app.syncAdjustments_[0] = -1800.0;
        app.syncAdjustments_[1] = -1800.0;
        // tick() uses this mapping: the 30-minute old master clock inflates
        // the new 10-minute video to a 40-minute aggregate extent.
        app.duration_ = std::max(timelineDuration(120.0, 0.0, app.syncAdjustments_[0]),
                                 timelineDuration(600.0, 0.0, app.syncAdjustments_[1]));
        const RECT client{0, 0, 1280, 720};
        app.refreshChromeGeometry(client);
        require(app.perPaneTimelines() && !app.bottomTimelineVisible() &&
                app.barTime().duration == 2400.0,
                "The synthetic two-Emby case no longer reproduces the stale aggregate duration");
        require(!app.barLayout_[BarItem::Time].visible() && !app.barLayout_[BarItem::Seek].visible() &&
                app.barLayout_[BarItem::Play].visible() && app.barLayout_[BarItem::Stop].visible() &&
                app.barLayout_[BarItem::Audio].visible() &&
                app.paneChrome_[0].timeline.visible() && app.paneChrome_[1].timeline.visible(),
                "Independent Emby panes lost their own controls or still show aggregate time");
        app.buildOverlayScene();
        require(app.scene_.timeText.empty() && !app.scene_.seekable,
                "The drawn bar still exposes the stale aggregate duration");
        app.updateTitle();
        require(app.windowTitle_.find(L"40:00") == std::wstring::npos,
                "The window title still exposes the stale aggregate duration");
        const auto formerRail = transportBarLayout(1280.0F, 720.0F, 1.0F, false)[BarItem::Seek];
        const POINT middle{static_cast<LONG>(formerRail.x + formerRail.width * 0.5F),
                           static_cast<LONG>(formerRail.y + formerRail.height * 0.5F)};
        const auto emptyHit = app.hitTestAt(middle);
        require(emptyHit.kind == OverlayHitKind::None,
                "The hidden bottom rail still accepts a seek click or swaps the pane below");
        const double oldClock = app.clock_.position();
        const auto oldAdjustments = app.syncAdjustments_;
        app.controlDrag_ = App::ControlDrag::MasterSeek;
        app.dragFraction_ = 0.5F;
        app.endControlDrag(true);
        require(app.clock_.position() == oldClock && app.syncAdjustments_ == oldAdjustments,
                "A stale bottom-bar drag still seeks independent Emby panes");

        app.browserSeekMode_ = SeekMode::Linked;
        app.refreshChromeGeometry(client);
        app.buildOverlayScene();
        app.updateTitle();
        require(app.bottomTimelineVisible() && app.barLayout_[BarItem::Time].visible() &&
                app.barLayout_[BarItem::Seek].visible() && app.barTime().duration == 600.0 &&
                app.scene_.timeText == L"0:00 / 10:00" &&
                app.windowTitle_.find(L"/ 10:00") != std::wstring::npos,
                "Linked Emby seek did not restore the longest-video bottom timeline");

        // Both Emby items can still be resolving when Linked is selected.
        // No ready source exists from which to derive a source-local range.
        app.sources_[0] = std::make_unique<VideoSource>();
        app.sources_[1] = std::make_unique<VideoSource>();
        app.refreshChromeGeometry(client);
        app.buildOverlayScene();
        app.updateTitle();
        require(app.barTime().duration == 0.0 && app.scene_.timeText == L"0:00 / 0:00" &&
                !app.scene_.seekable && app.windowTitle_.find(L"40:00") == std::wstring::npos,
                "Resolving Linked Emby panes briefly displayed the stale master extent");

        app.browserSeekMode_ = SeekMode::Independent;
        app.embyPanes_[1].reset();
        app.paths_[1].clear();
        app.sources_[1].reset();
        AudioSeekTestAccess::seed(*app.sources_[0], 120.0);
        app.refreshChromeGeometry(client);
        require(app.bottomTimelineVisible() && app.barLayout_[BarItem::Seek].visible() &&
                app.barTime().duration == 120.0,
                "A single Emby video's own bottom timeline was hidden");

        App manual;
        manual.sources_[0] = std::make_unique<VideoSource>();
        manual.sources_[1] = std::make_unique<VideoSource>();
        manual.paths_[0] = L"manual-one.mkv";
        manual.paths_[1] = L"manual-two.mkv";
        manual.refreshChromeGeometry(client);
        require(!manual.perPaneTimelines() && manual.bottomTimelineVisible() &&
                manual.barLayout_[BarItem::Seek].visible(),
                "A manual multi-video deck lost its master timeline");
    }

    static void oneEmbyVideoKeepsItsExactSeekPath() {
        App app;
        HiddenWindow parent;
        app.window_ = app.videoWindow_ = parent.window;
        seedLiveEmbyPane(app, 0, multiPaneItem("one-linked-emby"));
        AudioSeekTestAccess::seed(*app.sources_[0], 360.0);
        app.browserSeekMode_ = SeekMode::Linked;
        app.keyframeSeek_ = true;
        app.clock_.seek(60.0);
        app.clock_.pause();
        const auto before = AudioSeekTestAccess::videoGeneration(*app.sources_[0]);
        app.seekFromSourceBar(0, 30.0);
        require(!app.linkedBrowserSeekBars() && app.clock_.position() == 60.0 &&
                std::abs(app.currentSourceTime(0) - 30.0) < 0.001 &&
                AudioSeekTestAccess::videoGeneration(*app.sources_[0]) > before,
                "A lone Emby pane changed its exact single-pane seek path");
        App::EmbyPane photo;
        photo.photo = true;
        app.embyPanes_[1] = std::move(photo);
        app.paths_[1] = L"synthetic-Emby-photo";
        app.seekFromSourceBar(0, 40.0);
        require(!app.linkedBrowserSeekBars() && app.clock_.position() == 60.0 &&
                std::abs(app.currentSourceTime(0) - 40.0) < 0.001,
                "An Emby photo made a lone video's seek take the linked path");
    }

    static void localPlayAndAddPreserveOtherPanes() {
        App app;
        HiddenWindow parent;
        prepareDetailBrowser(app, parent);
        prepareLocalBrowser(app, parent);
        const auto queue = app.localList_;
        seedLiveEmbyPane(app, 0, multiPaneItem("local-neighbour"));
        seedLiveLocalPane(app, 2, syntheticLocalQueue(L"C:\\QuadDeckSynthetic\\Other folder"));
        auto* other = app.sources_[0].get();
        const auto otherSerial = app.embyPanes_[0]->serial;
        const auto videoTicket = other->requestSeek(12.0);
        const auto audioTicket = other->audioDecodeStatus().generation;
        app.syncAdjustments_[0] = 4.0;
        app.playbackRates_[0] = 1.25;
        app.startDelays_[0] = 2.0;
        app.seekMode_ = SeekMode::Linked;
        app.audioMask_ = 5U;
        app.clock_.seek(73.0);
        app.clock_.pause();
        auto timings = app.paneTimings();
        timings[0].loaded = timings[0].ready = true;
        timings[0].duration = 600.0;
        app.seekBarrier_.begin(44.0, false, timings, app.audioMask_, SeekMode::Independent);
        app.seekBarrier_.setGeneration(0, 11);
        app.seekBarrier_.setGeneration(2, 7);
        app.embySelectTarget(2);
        app.activateLocalItem(0);
        require(app.paths_[2] == queue[0].path && app.localPanes_[2] &&
                localQueuePaths(app.localPanes_[2]->queue) == localQueuePaths(queue) &&
                app.currentSourceTime(2) == 0.0 && !app.sourcePaused_[2] && app.audioMask_ == 5U,
                "Local Play did not start only the explicit target at zero with its own queue");
        require(app.sources_[0].get() == other && app.embyPanes_[0]->serial == otherSerial &&
                app.syncAdjustments_[0] == 4.0 && app.playbackRates_[0] == 1.25 && app.startDelays_[0] == 2.0 &&
                app.clock_.position() == 73.0 && !app.clock_.isPlaying() &&
                app.seekMode_ == SeekMode::Linked && app.activeSeekMode() == SeekMode::Independent,
                "Local Play changed an Emby neighbour, master clock or persisted seek mode");
        require(app.seekBarrier_.active() && app.seekBarrier_.waiting(0) && !app.seekBarrier_.waiting(2) &&
                !app.seekBarrier_.observeFrame(0, true, 10, true).exactFrame &&
                app.seekBarrier_.observeFrame(0, true, 11, true).exactFrame,
                "Replacing a local target cancelled or changed another pane's exact seek ticket");
        app.cancelSeekBarrier();
        app.soloPane_ = 2;
        app.activateLocalItem(0, true);
        require(app.paths_[1] == queue[0].path && app.paths_[2] == queue[0].path &&
                app.localPanes_[1]->addedMuted && app.audioMask_ == 5U && !app.clock_.isPlaying() &&
                app.currentSourceTime(1) == 0.0 && app.soloPane_ == -1 && app.embyPlaybackTarget() == 1,
                "Local Add did not allow the same file in an empty pane, muted and globally paused");
        const auto serial = app.subtitleSerial_[1];
        app.activateLocalItem(1);
        require(app.paths_[1] == queue[1].path && app.subtitleSerial_[1] != serial &&
                app.localPanes_[1]->addedMuted && app.audioMask_ == 5U,
                "Replacing an added local target lost its mute policy or changed another audio bit");
        app.setAudioMask(7U);
        require(app.audioPaneEnabled(1) && !app.localPanes_[1]->addedMuted,
                "Explicit local audio selection did not release automatic-handoff mute protection");
        app.clock_.seek(74.0);
        require(app.currentSourceTime(1) == 1.0 && app.currentSourceTime(2) == 1.0 && !app.sourcePaused_[1],
                "A local pane added under global pause could not advance with the master timeline");
        require(other->audioDecodeStatus().generation == audioTicket && other->requestSeek(12.0) == videoTicket + 1,
                "Local Play/Add issued a video or audio seek to another pane");

        App legacy;
        prepareLocalBrowser(legacy, parent);
        for (const std::size_t pane : {0U, 2U}) {
            legacy.paths_[pane] = queue[pane].path;
            legacy.sources_[pane] = std::make_unique<VideoSource>();
        }
        legacy.seekMode_ = SeekMode::Linked;
        legacy.openLocalList();
        require(!legacy.localPanes_[0] && !legacy.localPanes_[2] && legacy.activeSeekMode() == SeekMode::Linked,
                "Opening F6 changed the saved seek policy of a legacy multi-local deck");
        legacy.embySelectTarget(2);
        legacy.activateLocalItem(1);
        require(legacy.activeSeekMode() == SeekMode::Independent && legacy.seekMode_ == SeekMode::Linked,
                "A local browser route did not become independent while retaining the saved preference");

        // An Explorer-opened sole video is adopted only when a second pane
        // is added. Its previously inactive repeat preference must remain
        // inactive throughout that transition, preserving its EOF time.
        App explorer;
        prepareLocalBrowser(explorer, parent);
        seedLiveLocalPane(explorer, 0, queue);
        explorer.localPanes_[0].reset();
        explorer.sourceAutoRepeat_[0] = true;
        explorer.seekMode_ = SeekMode::Linked;
        explorer.clock_.seek(143.0);
        explorer.clock_.pause();
        explorer.audioMask_ = 1U;
        auto* original = explorer.sources_[0].get();
        const auto originalSerial = explorer.subtitleSerial_[0];
        const auto originalTicket = original->requestSeek(120.0);
        require(explorer.activeSeekMode() == SeekMode::Linked && explorer.mappedSourceTime(0, 143.0) == 143.0,
                "The Explorer fixture did not start with an inactive single-pane repeat preference");
        explorer.openLocalList();
        require(!explorer.localPanes_[0], "F6 alone adopted the Explorer source before a second pane was opened");
        explorer.activateLocalItem(1, true);
        require(explorer.localPanes_[0] && explorer.localPanes_[0]->suppressInheritedRepeat &&
                localQueuePaths(explorer.localPanes_[0]->queue) == localQueuePaths(queue) &&
                explorer.sources_[0].get() == original && explorer.subtitleSerial_[0] == originalSerial &&
                explorer.sourceAutoRepeat_[0] && !explorer.paneTiming(0).autoRepeat &&
                explorer.mappedSourceTime(0, 143.0) == 143.0 && explorer.audioMask_ == 1U &&
                explorer.clock_.position() == 143.0 && !explorer.clock_.isPlaying() &&
                original->requestSeek(120.0) == originalTicket + 1,
                "Adding beside an Explorer video changed its mapping/ticket or failed to adopt its own queue");
        explorer.playOrder_ = PlayOrder::InOrder;
        explorer.localFinishPane(0);
        require(explorer.paths_[0] == queue[1].path && explorer.paths_[1] == queue[1].path &&
                explorer.embyPlaybackTarget() == 1 && explorer.clock_.position() == 143.0,
                "The adopted Explorer video did not advance its own folder while preserving the Add target");
    }

    static void localReplacementKeepsItsSnapshot() {
        App app;
        HiddenWindow parent;
        prepareDetailBrowser(app, parent);
        prepareLocalBrowser(app, parent);
        const auto queue = app.localList_;
        for (std::size_t pane = 0; pane < kMaxPanes - 1; ++pane)
            seedLiveLocalPane(app, pane, syntheticLocalQueue(L"C:\\QuadDeckSynthetic\\Occupied " + std::to_wstring(pane)));
        seedLiveEmbyPane(app, 4, multiPaneItem("occupied-emby"));
        app.audioMask_ = 9U;
        app.clock_.seek(83.0);
        app.clock_.pause();
        app.soloPane_ = 2;
        app.settingsScroll_ = 123.0F;
        app.embySelectTarget(2);
        const auto paths = app.paths_;
        const auto serials = app.subtitleSerial_;
        PaneArray<VideoSource*> sources{};
        for (std::size_t pane = 0; pane < kMaxPanes; ++pane) sources[pane] = app.sources_[pane].get();
        const auto unchanged = [&] {
            require(app.paths_ == paths && app.subtitleSerial_ == serials && app.audioMask_ == 9U &&
                    app.clock_.position() == 83.0 && !app.clock_.isPlaying() && app.embyPlaybackTarget() == 2,
                    "A pending/cancelled local Add changed playback or its explicit target");
            for (std::size_t pane = 0; pane < kMaxPanes; ++pane)
                require(app.sources_[pane].get() == sources[pane], "A pending/cancelled local Add replaced a decoder");
        };
        app.activateLocalItem(1, true);
        require(app.localPendingPlay_ && app.localPendingPlay_->path == queue[1].path &&
                localQueuePaths(app.localPendingPlay_->queue) == localQueuePaths(queue),
                "A full local deck did not capture the requested file and queue");
        unchanged();
        app.embyBrowser_.sort = static_cast<int>(emby::SortKey::Runtime);
        app.applyLocalProbes(app.localListSerial_, app.localDirectory_, {{queue[0].path, {700.0}}, {queue[2].path, {10.0}}});
        require(app.localList_.front().path == queue[2].path && app.localPendingPlay_->path == queue[1].path &&
                localQueuePaths(app.localPendingPlay_->queue) == localQueuePaths(queue) &&
                app.localPendingPlay_->queue.front().duration == 120.0,
                "An asynchronous length reorder changed a pending local Add snapshot");
        app.localEntries_ = app.localList_ = syntheticLocalQueue(L"C:\\QuadDeckSynthetic\\Browser changed");
        const auto rows = app.buildLocalRows(800.0F);
        const auto layout = settingsPanelLayout(800.0F, 1800.0F, 1.0F, rows, 0.0F, 1.0F, {}, 800.0F);
        requireReplacementLayout(rows, layout);
        app.localCancelReplacement();
        require(!app.localPendingPlay_ && app.settingsScroll_ == 123.0F && app.soloPane_ == 2,
                "Cancel did not preserve Solo and restore the local browser position");
        unchanged();
        app.localList_ = queue;
        app.activateLocalItem(1, true);
        app.localList_ = syntheticLocalQueue(L"C:\\QuadDeckSynthetic\\Changed again");
        app.localConfirmReplacement(3);
        require(!app.localPendingPlay_ && app.paths_[3] == queue[1].path && app.localPanes_[3]->addedMuted &&
                localQueuePaths(app.localPanes_[3]->queue) == localQueuePaths(queue) && app.audioMask_ == 1U &&
                app.clock_.position() == 83.0 && !app.clock_.isPlaying(),
                "Confirmed local Add did not replace only its chosen pane using the captured queue, muted");
        for (std::size_t pane = 0; pane < kMaxPanes; ++pane) {
            if (pane == 3) continue;
            require(app.paths_[pane] == paths[pane] && app.sources_[pane].get() == sources[pane] &&
                    app.subtitleSerial_[pane] == serials[pane], "Local replacement changed an unselected pane");
        }
        app.localList_ = queue;
        app.activateLocalItem(0, true);
        ++app.subtitleSerial_[0];
        app.localConfirmReplacement(0);
        require(!app.localPendingPlay_ && app.paths_[0] == paths[0] && app.sources_[0].get() == sources[0],
                "A stale local chooser replaced newer media in the same storage pane");
        app.activateLocalItem(0, true);
        app.paths_[1] = L"C:\\QuadDeckSynthetic\\External replacement.mp4";
        app.localConfirmReplacement(1);
        require(!app.localPendingPlay_ && app.paths_[1] == L"C:\\QuadDeckSynthetic\\External replacement.mp4",
                "A local chooser ignored a changed target path");
        app.activateLocalItem(0, true);
        const auto beforeClose = app.paths_;
        app.closeSettingsPanel();
        require(!app.localPendingPlay_ && !app.settingsOpen_ && app.paths_ == beforeClose,
                "Closing F6 retained a pending local replacement or changed playback");
    }

    static void localQueuesFollowTheirPanesAndEndIndependently() {
        App app;
        HiddenWindow parent;
        prepareDetailBrowser(app, parent);
        prepareLocalBrowser(app, parent);
        const auto a = syntheticLocalQueue();
        const auto b = syntheticLocalQueue(L"C:\\QuadDeckSynthetic\\Folder B");
        seedLiveLocalPane(app, 0, a);
        seedLiveLocalPane(app, 1, b, 0, true);
        seedLiveEmbyPane(app, 2, multiPaneItem("queue-neighbour"));
        app.sourcePaused_[1] = true;
        app.audioMask_ = 1U;
        app.clock_.seek(83.0);
        app.clock_.pause();
        app.embySelectTarget(0);
        app.localPanes_[0]->queueLoadSerial = ++app.localQueueLoadSerial_;
        const auto adoptedSerial = app.localPanes_[0]->queueLoadSerial;
        app.swapPanes(0, 1);
        require(app.paths_[0] == b[0].path && app.paths_[1] == a[0].path &&
                localQueuePaths(app.localPanes_[0]->queue) == localQueuePaths(b) &&
                localQueuePaths(app.localPanes_[1]->queue) == localQueuePaths(a) &&
                app.localPanes_[0]->addedMuted && app.audioMask_ == 2U && app.embyPlaybackTarget() == 1,
                "Swapping local panes detached their queue, mute policy or explicit target");
        const std::vector<LocalEntry> discovered{a[0], a[2], a[1]};
        app.localApplyAdoptedQueue(adoptedSerial + 1, a[0].path, discovered);
        require(localQueuePaths(app.localPanes_[1]->queue) == localQueuePaths(a),
                "An old asynchronous folder snapshot was accepted without its adoption token");
        app.localApplyAdoptedQueue(adoptedSerial, a[0].path, discovered);
        require(localQueuePaths(app.localPanes_[1]->queue) == localQueuePaths(discovered) &&
                app.localPanes_[1]->queueLoadSerial == 0 && localQueuePaths(app.localPanes_[0]->queue) == localQueuePaths(b),
                "An adopted folder reply failed to find its swapped/recovery-deferred pane");
        app.localApplyAdoptedQueue(adoptedSerial, a[0].path, b);
        require(localQueuePaths(app.localPanes_[1]->queue) == localQueuePaths(discovered),
                "A consumed asynchronous adoption token replaced a newer local queue");
        app.localPanes_[1]->queue = a;
        auto* neighbour = app.sources_[2].get();
        auto* folderB = app.sources_[0].get();
        const auto neighbourSerial = app.embyPanes_[2]->serial;
        const auto neighbourVideo = neighbour->requestSeek(7.0);
        const auto neighbourAudio = neighbour->audioDecodeStatus().generation;
        app.localDirectory_ = L"C:\\QuadDeckSynthetic\\Unrelated folder";
        app.localEntries_ = app.localList_ = syntheticLocalQueue(app.localDirectory_);
        app.localPanes_[1]->queueLoadSerial = ++app.localQueueLoadSerial_;
        const auto retiredLoad = app.localPanes_[1]->queueLoadSerial;
        app.openAdjacentFile(1, 1);
        app.localApplyAdoptedQueue(retiredLoad, a[0].path, b);
        require(app.paths_[1] == a[1].path && localQueuePaths(app.localPanes_[1]->queue) == localQueuePaths(a) &&
                app.paths_[0] == b[0].path && app.sources_[0].get() == folderB &&
                app.clock_.position() == 83.0 && !app.clock_.isPlaying(),
                "Local Next borrowed the currently browsed folder or reset another pane/master clock");
        app.sources_[1] = std::make_unique<VideoSource>();
        AudioSeekTestAccess::seed(*app.sources_[1], a[1].duration);
        app.embySelectTarget(2);
        app.playOrder_ = PlayOrder::InOrder;
        app.clock_.seek(83.0 + a[1].duration);
        app.clock_.play();
        app.finishLocalPanes();
        app.clock_.pause();
        require(app.paths_[1] == a[2].path && app.embyPlaybackTarget() == 2 &&
                app.sources_[0].get() == folderB && app.sources_[2].get() == neighbour &&
                app.embyPanes_[2]->serial == neighbourSerial,
                "A local EOF did not advance only its queue or stole the explicit target");
        const auto serial = app.subtitleSerial_[1];
        app.clock_.play();
        app.finishLocalPanes();
        app.finishLocalPanes();
        app.clock_.pause();
        require(app.subtitleSerial_[1] == serial, "A pending local next source was advanced repeatedly by EOF");
        app.playOrder_ = PlayOrder::PlayOne;
        app.localFinishPane(1);
        app.localFinishPane(1);
        require(app.paths_[1] == a[2].path && app.localPanes_[1]->endHandled && app.sourcePaused_[1] &&
                app.subtitleSerial_[1] == serial, "Local Play One did not latch a single end event");
        app.playOrder_ = PlayOrder::RepeatList;
        app.localPanes_[1]->endHandled = false;
        app.localFinishPane(1);
        require(app.paths_[1] == a[0].path && app.embyPlaybackTarget() == 2,
                "Local Repeat List did not wrap its own queue without changing the explicit target");
        app.playOrder_ = PlayOrder::Shuffle;
        app.localFinishPane(1);
        require(app.paths_[1] != a[0].path && localEntryIndex(a, app.paths_[1]) >= 0,
                "Local Shuffle repeated its current item or escaped the pane's captured folder");
        app.sources_[1] = std::make_unique<VideoSource>();
        AudioSeekTestAccess::seed(*app.sources_[1], 120.0);
        auto* repeatSource = app.sources_[1].get();
        const auto repeatSerial = app.subtitleSerial_[1];
        const auto repeatPath = app.paths_[1];
        app.playOrder_ = PlayOrder::RepeatOne;
        app.localFinishPane(1);
        require(app.sources_[1].get() == repeatSource && app.paths_[1] == repeatPath &&
                app.subtitleSerial_[1] == repeatSerial && !app.localPanes_[1]->endHandled &&
                app.currentSourceTime(1) == 0.0 && app.embyPlaybackTarget() == 2,
                "Local Repeat One reopened its decoder or failed to seek/reset only its source");
        const std::vector<LocalEntry> one{a[0]};
        app.localPlayItem(one[0].path, one, 1, true, false);
        const auto oneSerial = app.subtitleSerial_[1];
        app.openAdjacentFile(1, 1);
        require(app.subtitleSerial_[1] == oneSerial, "Manual Next restarted a one-entry local queue");
        app.playOrder_ = PlayOrder::RepeatList;
        app.localFinishPane(1);
        require(app.paths_[1] == one[0].path && app.subtitleSerial_[1] != oneSerial &&
                app.localPanes_[1]->addedMuted && app.embyPlaybackTarget() == 2,
                "Automatic single-item Repeat List lost its restart, mute policy or target ownership");
        require(neighbour->audioDecodeStatus().generation == neighbourAudio &&
                neighbour->requestSeek(7.0) == neighbourVideo + 1,
                "Local navigation/EOF issued a seek to the Emby neighbour");

        // Ready synthetic sources make handoff eligibility observable: an
        // added pane stays excluded until its audio is explicitly selected.
        App handoff;
        prepareLocalBrowser(handoff, parent);
        seedLiveLocalPane(handoff, 0, a);
        seedLiveLocalPane(handoff, 1, a, 1, true);
        AudioSeekTestAccess::seed(*handoff.sources_[0], 120.0, AudioDecodeState::Ended);
        handoff.audioMask_ = 1U;
        handoff.clock_.seek(130.0);
        handoff.selectNextUnfinishedAudio(130.0);
        require(handoff.audioMask_ == 1U, "Automatic audio handoff enabled an added-muted local pane");
        handoff.setAudioMask(3U);
        handoff.setAudioMask(1U);
        handoff.selectNextUnfinishedAudio(130.0);
        require(handoff.audioMask_ == 2U && !handoff.localPanes_[1]->addedMuted,
                "Explicit local audio selection did not restore ordinary EOF handoff eligibility");
    }

    static void localLengthRepliesRetirePressedIndices() {
        App app;
        HiddenWindow parent;
        prepareLocalBrowser(app, parent);
        require(SetWindowPos(parent.window, nullptr, 0, 0, 700, 1600,
                             SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOZORDER) != FALSE,
                "Cannot size the hidden local input fixture window");
        const RECT client{0, 0, 700, 1600};
        app.embyBrowser_.sort = static_cast<int>(emby::SortKey::Runtime);
        const auto queue = app.localList_;
        for (const PanelHitKind kind : {PanelHitKind::Item, PanelHitKind::Add}) {
            app.localEntries_ = app.localList_ = queue;
            app.orderLocalList();
            app.refreshPanelGeometry(client);
            int row = -1;
            for (std::size_t i = 0; i < app.panelRows_.size(); ++i)
                if (app.panelRows_[i].id == SettingId::LocalItem && app.panelRows_[i].param == 0) row = static_cast<int>(i);
            require(row >= 0, "The local press fixture has no first file row");
            app.panelPressArmed_ = true;
            app.pressedPanelKind_ = kind;
            app.pressedPanelId_ = SettingId::LocalItem;
            app.pressedPanelRow_ = row;
            app.pressedPanelPart_ = kind == PanelHitKind::Add ? 0 : -1;
            app.pressedPanelParam_ = 0;
            app.applyLocalProbes(app.localListSerial_, app.localDirectory_, {{queue[0].path, {700.0}}, {queue[2].path, {10.0}}});
            require(app.localList_.front().path == queue[2].path && !app.panelPressArmed_ &&
                    app.pressedPanelKind_ == PanelHitKind::None,
                    "A length reply kept an armed local index after sorting it to another file");
            app.refreshPanelGeometry(client);
            const auto& geometry = app.panelLayout_.rows[static_cast<std::size_t>(row)];
            const auto& box = kind == PanelHitKind::Add ? geometry.addParts.at(0) : geometry.row;
            app.releasePanelPress({static_cast<LONG>(box.x + 2.0F), static_cast<LONG>(box.y + 2.0F)});
            require(!app.anyPaneLoaded() && !app.localPendingPlay_,
                    "Release after an asynchronous local reorder played or added the wrong file");
        }
        // Both list and tile Add rectangles have a distinct native hit, and
        // the ordinary row/tile keeps the same source index.
        for (const int view : {0, 1, 2}) {
            app.embyBrowser_.view = view;
            const auto rows = app.buildLocalRows(700.0F);
            const auto layout = settingsPanelLayout(700.0F, 2200.0F, 1.0F, rows, 0.0F, 1.0F, {}, 700.0F);
            int seen = 0;
            for (std::size_t row = 0; row < rows.size(); ++row) {
                if (rows[row].id != SettingId::LocalItem) continue;
                const auto& geometry = layout.rows[row];
                for (std::size_t part = 0; part < geometry.addParts.size(); ++part) {
                    const auto& add = geometry.addParts[part];
                    const auto hit = settingsPanelHitTest(add.x + add.width * 0.5F, add.y + add.height * 0.5F, layout, rows);
                    require(add.visible() && hit.kind == PanelHitKind::Add && hit.row == static_cast<int>(row) &&
                            panelRowParam(rows[row], hit.part) == (view == 0 ? rows[row].param : rows[row].tileParams[part]),
                            "A local list/tile Add button does not hit its drawn file index");
                    ++seen;
                }
            }
            require(seen == static_cast<int>(queue.size()), "A local view omitted a file's Add action");
        }
    }

    static void embyAddAndDedupPreserveOtherPanes() {
        App app;
        HiddenWindow parent;
        prepareDetailBrowser(app, parent);
        app.paths_[0] = L"C:\\Synthetic\\local.mp4";
        app.sources_[0] = std::make_unique<VideoSource>();
        const auto* local = app.sources_[0].get();
        app.syncAdjustments_[0] = 4.0;
        app.playbackRates_[0] = 1.25;
        app.sourceAutoRepeat_[0] = true;
        app.seekMode_ = SeekMode::Linked;
        app.audioMask_ = 5U;
        app.clock_.seek(73.0);
        app.clock_.pause();
        const auto videoTicket = app.sources_[0]->requestSeek(12.0);
        const auto audioTicket = app.sources_[0]->audioDecodeStatus().generation;
        const auto first = multiPaneItem("first");
        const auto second = multiPaneItem("second");
        const auto replacement = multiPaneItem("replacement");
        const auto firstQueue = emby::takeQueue({first, replacement}, 0, "first-list");
        app.embyRequestPlay(first, firstQueue, true);
        require(app.embyPanes_[1] && app.embyPanes_[1]->itemId == "first" && app.embyPanes_[1]->resolving &&
                !app.embyPanes_[1]->startPlaying && app.audioMask_ == 5U &&
                playbackQueue(app, 1).page == "first-list", "Add did not take the first empty pane, muted and paused");
        require(app.embyPanes_[1]->addedMuted, "An added pane lost its explicit mute policy");
        app.soloPane_ = 0;
        app.embyRequestPlay(second, emby::takeQueue({second}, 0, "second-list"), true);
        require(app.embyPanes_[2] && app.embyPanes_[2]->itemId == "second" && app.audioMask_ == 1U,
                "Adding a second video reused a resolving pane or enabled its audio");
        require(app.soloPane_ == -1, "Adding a pane left it hidden behind Solo");
        const auto firstSerial = app.embyPanes_[1]->serial;
        const auto secondSerial = app.embyPanes_[2]->serial;
        for (const bool add : {false, true}) {
            app.embyRequestPlay(first, emby::takeQueue({replacement, first}, 1, "different-list"), add, true);
            require(app.embyPanes_[1]->serial == firstSerial && !app.embyPanes_[1]->fromBeginning &&
                    playbackQueue(app, 1).page == "first-list" && playbackQueue(app, 1).cursor == 0 &&
                    !app.embyPendingPlay_ && app.embyPlaybackTarget() == 1 && app.audioMask_ == 1U,
                    "Opening the same resolving item duplicated it or changed its start/queue/audio");
        }
        app.embySelectTarget(1);
        app.embyRequestPlay(replacement, emby::takeQueue({replacement}, 0, "replacement-list"));
        require(app.embyPanes_[1]->itemId == "replacement" && app.embyPanes_[2]->serial == secondSerial &&
                playbackQueue(app, 2).page == "second-list" && app.audioMask_ == 1U,
                "Ordinary Play did not replace only the selected target");
        require(app.sources_[0].get() == local && app.paths_[0] == L"C:\\Synthetic\\local.mp4" &&
                app.syncAdjustments_[0] == 4.0 && app.playbackRates_[0] == 1.25 &&
                app.sourceAutoRepeat_[0] && app.clock_.position() == 73.0 && !app.clock_.isPlaying() &&
                app.seekMode_ == SeekMode::Linked && app.activeSeekMode() == SeekMode::Independent,
                "Adding Emby changed the local source, master clock or persisted seek choice");
        require(app.sources_[0]->audioDecodeStatus().generation == audioTicket &&
                app.sources_[0]->requestSeek(12.0) == videoTicket + 1,
                "Opening one Emby pane issued a seek to another pane");
        app.setAudioMask(app.audioMask_ | 2U);
        require(!app.embyPanes_[1]->addedMuted && app.audioPaneEnabled(1),
                "Explicitly enabling audio did not release the added pane's mute policy");
        app.openSource(3, L"emby://wrong-server/first");
        require(app.embyPanes_[3] && !app.embyPaneAccountMatches(3),
                "A restored locator lost the server it belongs to");
        app.embyRequestPlay(first, firstQueue, true);
        require(app.embyPanes_[4] && app.embyPanes_[4]->itemId == "first" && app.embyPaneAccountMatches(4),
                "A wrong-server locator suppressed an Add from the current account");
    }

    static void embyFullDeckReplacementIsExplicitAndStable() {
        App app;
        HiddenWindow parent;
        prepareDetailBrowser(app, parent);
        for (std::size_t pane = 0; pane < kMaxPanes; ++pane)
            seedLiveEmbyPane(app, pane, multiPaneItem("occupied-" + std::to_string(pane)));
        app.audioMask_ = 9U;
        app.soloPane_ = 2;
        app.clock_.seek(83.0);
        app.clock_.pause();
        app.settingsScroll_ = 123.0F;
        const auto oldPaths = app.paths_;
        PaneArray<VideoSource*> oldSources{};
        PaneArray<std::uint64_t> oldSerials{};
        for (std::size_t pane = 0; pane < kMaxPanes; ++pane) {
            oldSources[pane] = app.sources_[pane].get();
            oldSerials[pane] = app.embyPanes_[pane]->serial;
        }
        const auto item = multiPaneItem("sixth");
        const auto queue = emby::takeQueue({item, multiPaneItem("seventh")}, 0, "sixth-list");
        const auto requireUnchanged = [&] {
            require(app.paths_ == oldPaths && app.audioMask_ == 9U && app.clock_.position() == 83.0 &&
                    !app.clock_.isPlaying(), "A pending or cancelled replacement changed playback");
            for (std::size_t pane = 0; pane < kMaxPanes; ++pane)
                require(app.sources_[pane].get() == oldSources[pane] && app.embyPanes_[pane]->serial == oldSerials[pane],
                        "A pending or cancelled replacement replaced a source");
        };
        app.embyRequestPlay(item, queue, true, true);
        require(app.embyPendingPlay_ && app.embyPendingPlay_->fromBeginning &&
                app.embyPendingPlay_->queue.page == "sixth-list", "A full deck did not retain an explicit pending Add");
        app.embyItems_ = {multiPaneItem("browser-changed")};
        const auto rows = app.buildEmbyRows(800.0F);
        int choices = 0;
        bool cancel = false;
        for (const auto& row : rows) {
            cancel = cancel || row.id == SettingId::EmbyReplaceCancel;
            if (row.id != SettingId::EmbyReplaceChoice) continue;
            for (const int pane : row.tileParams) {
                require(pane >= 0 && pane < static_cast<int>(kMaxPanes), "Replacement choice lost its storage pane");
                ++choices;
            }
        }
        require(choices == 5 && cancel, "The full-deck chooser does not expose five panes and Cancel");
        requireUnchanged();
        app.embyCancelReplacement();
        require(!app.embyPendingPlay_ && app.settingsScroll_ == 123.0F, "Cancel did not restore the source scroll");
        require(app.soloPane_ == 2, "Cancelling Add changed the existing Solo view");
        requireUnchanged();
        app.embyRequestPlay(item, queue, true, true);
        app.embyConfirmReplacement(3);
        require(!app.embyPendingPlay_ && app.embyPanes_[3]->itemId == "sixth" && app.embyPanes_[3]->fromBeginning &&
                app.embyPanes_[3]->resumeTicks == 0 && playbackQueue(app, 3).page == "sixth-list" &&
                playbackQueue(app, 3).items.size() == 2 && app.audioMask_ == 1U && !app.clock_.isPlaying(),
                "Confirmed Add did not replace exactly the chosen pane with its saved queue/start, muted");
        for (std::size_t pane = 0; pane < kMaxPanes; ++pane) {
            if (pane == 3) continue;
            require(app.paths_[pane] == oldPaths[pane] && app.sources_[pane].get() == oldSources[pane] &&
                    app.embyPanes_[pane]->serial == oldSerials[pane], "Confirm changed an unselected pane");
        }
        app.embyRequestPlay(multiPaneItem("stale-choice"), queue, true);
        ++app.subtitleSerial_[0]; // The selected slot changed while its picture was displayed.
        app.embyConfirmReplacement(0);
        require(app.paths_[0] == oldPaths[0] && !app.embyPendingPlay_, "A stale chooser picture replaced newer media");
        app.embyRequestPlay(multiPaneItem("stale-account"), queue, true);
        auto newSession = app.emby_.session();
        newSession.userId = "other-user";
        app.embyConfigure(newSession);
        app.embyConfirmReplacement(1);
        require(!app.embyPendingPlay_ && app.emby_.session().userId == "other-user",
                "An old-account pending choice survived the new account");
        for (std::size_t pane = 0; pane < kMaxPanes; ++pane)
            require(app.paths_[pane].empty() && !app.embyPanes_[pane],
                    "An account switch kept old Emby playback or reopened the stale pending choice");
    }

    static void embyReportsUseEachPaneRegardlessOfAudio() {
        App app;
        HiddenWindow parent;
        prepareDetailBrowser(app, parent);
        seedLiveEmbyPane(app, 0, multiPaneItem("alpha"));
        seedLiveEmbyPane(app, 1, multiPaneItem("beta"));
        app.clock_.seek(42.0);
        app.clock_.pause();
        app.sourcePaused_[0] = app.sourcePaused_[1] = true;
        app.sourcePausedTimes_[0] = 15.0;
        app.sourcePausedTimes_[1] = 99.0;
        app.audioMask_ = 0;
        const auto alphaSerial = app.embyPanes_[0]->serial;
        auto* alphaSource = app.sources_[0].get();
        app.embyRequestPlay(multiPaneItem("alpha"), emby::takeQueue({multiPaneItem("alpha")}, 0, "changed"), true, true);
        require(app.embyPanes_[0]->serial == alphaSerial && app.sources_[0].get() == alphaSource &&
                playbackQueue(app).page == "synthetic-page-alpha" && app.audioMask_ == 0 && !app.embyPanes_[2],
                "Adding an already playing item restarted it, replaced its queue or changed its audio");
        const auto alpha = app.embyReportForPane(0);
        const auto beta = app.embyReportForPane(1);
        require(alpha && beta && alpha->itemId == "alpha" && beta->itemId == "beta" &&
                alpha->mediaSourceId == "media-alpha" && beta->mediaSourceId == "media-beta" &&
                alpha->playSessionId == "session-alpha" && beta->playSessionId == "session-beta" &&
                alpha->positionTicks == emby::ticksFromSeconds(15.0) && beta->positionTicks == emby::ticksFromSeconds(99.0) &&
                alpha->paused && beta->paused, "Muted panes did not retain their own report identity and source time");
        app.audioMask_ = 1U;
        app.clock_.play();
        app.sourcePaused_[0] = false;
        const auto running = app.embyReportForPane(0);
        const auto held = app.embyReportForPane(1);
        require(running && held && !running->paused && held->paused && held->positionTicks == beta->positionTicks,
                "A muted/paused pane borrowed another pane's pause state or progress");
        app.clock_.pause();
        app.swapPanes(0, 1);
        require(app.embyReportForPane(0)->itemId == "beta" && app.embyReportForPane(1)->itemId == "alpha" &&
                playbackQueue(app, 0).page == "synthetic-page-beta" && playbackQueue(app, 1).page == "synthetic-page-alpha",
                "Swapping pictures lost report or queue ownership");
        app.embyPanes_[0]->photo = true;
        require(!app.embyReportForPane(0), "A still image sent video progress");
        auto session = app.emby_.session();
        session.token = "new-account-generation";
        app.embyConfigure(session);
        require(!app.embyReportForPane(0) && !app.embyReportForPane(1), "Detached account panes still generated playback reports");
        app.embyRequestPlay(multiPaneItem("alpha"), emby::takeQueue({multiPaneItem("alpha")}, 0, {}), true);
        require(app.embyPanes_[0] && app.embyPanes_[0]->itemId == "alpha" && app.embyPaneAccountMatches(0) &&
                !app.embyPanes_[1] && !app.embyPanes_[2],
                "An old-account item suppressed the new account's Add");
    }

    static void embyResumeAndEndStaySourceLocal() {
        App app;
        HiddenWindow parent;
        prepareDetailBrowser(app, parent);
        const auto first = multiPaneItem("first");
        const auto second = multiPaneItem("second");
        const auto third = multiPaneItem("third");
        seedLiveEmbyPane(app, 0, first);
        seedLiveEmbyPane(app, 1, second);
        app.audioMask_ = 1U;
        app.clock_.seek(123.0);
        app.clock_.pause();
        const auto otherVideoTicket = app.sources_[1]->requestSeek(7.0);
        const auto otherAudioTicket = app.sources_[1]->audioDecodeStatus().generation;
        auto* otherSource = app.sources_[1].get();
        app.embyPanes_[0]->autoplay = true;
        app.embyPanes_[0]->resumeTicks = emby::ticksFromSeconds(90.0);
        const auto playback = emby::parsePlaybackInfo(R"({"PlaySessionId":"resolved-session","MediaSources":[
            {"Id":"resolved-media","SupportsDirectPlay":true,"RunTimeTicks":6000000000}]})");
        require(playback.has_value(), "Cannot parse synthetic direct-play reply");
        app.embyStartResolved(0, first, *playback);
        // The file comes as it lies on the server's disk: its subtitle
        // streams are read with the video, and looked at once it is open.
        require(app.sources_[0]->options_.readSubtitles && app.subtitleEmbeddedPending_[0],
                "An Emby item's stream opened without its subtitle streams being read");
        require(!app.clock_.isPlaying() && app.clock_.position() == 123.0 &&
                std::abs(app.syncAdjustments_[0] + 33.0) < 0.0001 &&
                std::abs(app.sourceProvisionalTargets_[0] - 90.0) < 0.0001 && app.sources_[1].get() == otherSource &&
                app.sources_[1]->audioDecodeStatus().generation == otherAudioTicket &&
                app.sources_[1]->requestSeek(7.0) == otherVideoTicket + 1,
                "A resolved resume moved the master/another pane or resumed a globally paused deck");
        mutablePlaybackQueue(app).items = {first, second, third};
        mutablePlaybackQueue(app).cursor = 0;
        app.sourceAutoRepeat_[0] = true;
        app.playOrder_ = PlayOrder::InOrder;
        app.embySelectTarget(1);
        app.embyPanes_[0]->resolving = false;
        app.embyPanes_[0]->started = true;
        const auto endedSerial = app.embyPanes_[0]->serial;
        const auto secondSerial = app.embyPanes_[1]->serial;
        app.embyFinishPane(0);
        require(app.embyPanes_[0]->endHandled && app.embyPanes_[0]->serial == endedSerial &&
                app.embyPanes_[0]->itemId == "first" && app.embyPanes_[1]->serial == secondSerial &&
                app.sourcePaused_[0] && !app.clock_.isPlaying(),
                "EOF repeated an Emby video or duplicated a next episode already open in another pane");
        app.embyFinishPane(0);
        require(app.embyPanes_[0]->serial == endedSerial && app.embyPanes_[1]->serial == secondSerial,
                "The same ended source requested its next episode again");
        app.embyPanes_[0]->endHandled = false;
        mutablePlaybackQueue(app).items = {first, third};
        const auto nextVideoTicket = app.sources_[1]->requestSeek(7.0);
        app.embyFinishPane(0);
        require(app.embyPanes_[0]->itemId == "third" && playbackQueue(app).cursor == 1 &&
                app.embyPanes_[1]->serial == secondSerial && app.sources_[1]->requestSeek(7.0) == nextVideoTicket + 1 &&
                app.clock_.position() == 123.0 && !app.clock_.isPlaying(),
                "Automatic next did not advance only the ended pane's queue");
        require(app.embyPlaybackTarget() == 1, "Automatic next or duplicate EOF stole the explicit play target");
        auto timings = app.paneTimings();
        for (std::size_t pane = 0; pane < 2; ++pane) {
            timings[pane].loaded = true;
            timings[pane].paused = false;
            timings[pane].duration = 600.0;
        }
        app.seekBarrier_.begin(44.0, false, timings, 1U, SeekMode::Independent);
        app.seekBarrier_.setGeneration(0, 7);
        app.seekBarrier_.setGeneration(1, 11);
        app.embyPlayItem(multiPaneItem("barrier-replacement"), 0);
        require(app.seekBarrier_.active() && !app.seekBarrier_.waiting(0) && app.seekBarrier_.waiting(1),
                "Replacing one pane cancelled another pane's global seek barrier");
        app.seekBarrier_.observeFrame(1, true, 10, true);
        require(app.seekBarrier_.waiting(1), "A replacement changed another pane's required seek generation");
        const auto exactFrame = app.seekBarrier_.observeFrame(1, true, 11, true);
        require(exactFrame.exactFrame && app.seekBarrier_.ready(0, app.paneStatuses()),
                "The original seek ticket no longer completed the retained pane");
        app.cancelSeekBarrier();
        seedLiveEmbyPane(app, 0, first);
        auto firstEntry = first, secondEntry = first;
        firstEntry.playlistItemId = "entry-one";
        secondEntry.playlistItemId = "entry-two";
        app.embyPanes_[0]->queue = emby::takeQueue({firstEntry, secondEntry, third}, 0, "duplicate-entries");
        const auto samePaneSerial = app.embyPanes_[0]->serial;
        const auto sameSession = app.embyPanes_[0]->playSessionId;
        auto* sameSource = app.sources_[0].get();
        app.embyPlayFromPlaylist(1, 0);
        require(playbackQueue(app).cursor == 1 && playbackQueue(app).items[1].playlistItemId == "entry-two" &&
                app.embyPanes_[0]->serial == samePaneSerial && app.embyPanes_[0]->playSessionId == sameSession &&
                app.sources_[0].get() == sameSource,
                "Stepping to a repeated entry in the same pane did not move its queue cursor without reopening");
        app.embyPanes_[0]->queue = emby::takeQueue({first}, 0, "single-repeat-list");
        app.playOrder_ = PlayOrder::RepeatList;
        app.embyPanes_[0]->endHandled = true;
        app.sourcePaused_[0] = true;
        app.sourcePausedTimes_[0] = 600.0;
        const auto repeatVideoTicket = app.sources_[0]->requestSeek(600.0);
        app.embyPlayFromPlaylist(0, 0);
        require(!app.embyPanes_[0]->endHandled && !app.sourcePaused_[0] &&
                app.embyPanes_[0]->serial == samePaneSerial && app.embyPanes_[0]->playSessionId == sameSession &&
                app.sources_[0].get() == sameSource &&
                app.sources_[0]->requestSeek(0.0) == repeatVideoTicket + 2 &&
                std::abs(app.currentSourceTime(0)) < 0.0001,
                "Repeating a single-entry list reopened its session instead of seeking the same source to zero");
        app.deviceRecoveryPending_ = true;
        app.hoverPane_ = app.lastPointerPane_ = 0;
        app.controlsPinned_ = true;
        app.soloPane_ = -1;
        app.localList_.emplace_back();
        app.localList_.back().path = L"C:\\Synthetic\\mixed-local.mp4";
        const auto mixedOtherTicket = app.sources_[1]->requestSeek(7.0);
        const auto mixedOtherAudio = app.sources_[1]->audioDecodeStatus().generation;
        const auto mixedOtherSerial = app.embyPanes_[1]->serial;
        const auto mixedClock = app.clock_.position();
        app.activateLocalItem(0);
        require(app.paths_[0] == L"C:\\Synthetic\\mixed-local.mp4" && !app.embyPanes_[0] &&
                app.embyPanes_[1]->serial == mixedOtherSerial && app.sources_[1].get() == otherSource &&
                app.sources_[1]->audioDecodeStatus().generation == mixedOtherAudio &&
                app.sources_[1]->requestSeek(7.0) == mixedOtherTicket + 1 &&
                app.clock_.position() == mixedClock && !app.clock_.isPlaying(),
                "Choosing a local file in a mixed deck sought or replaced another Emby pane");
    }

    // Browsing metadata freezes the source list, not the running deck. Back
    // restores that exact Random page without asking the server to shuffle.
    static void embyDetailsPreserveSourceAndPlayback() {
        App app;
        HiddenWindow parent;
        prepareDetailBrowser(app, parent);
        const auto movies = emby::parseItems(R"({"Items":[
            {"Id":"c","Name":"Third","Type":"Movie"},
            {"Id":"a","Name":"Aurora","Type":"Movie","RunTimeTicks":72000000000,
             "UserData":{"PlaybackPositionTicks":18000000000}},
            {"Id":"b","Name":"Second","Type":"Movie"}
        ],"TotalRecordCount":3})");
        require(movies.has_value(), "Cannot parse the detail source list");
        app.embyItems_ = movies->items;
        app.embyTotal_ = 3;
        app.embyBrowser_.sort = static_cast<int>(emby::SortKey::Random);
        app.settingsScroll_ = 314.0F;
        app.sources_[0] = std::make_unique<VideoSource>();
        auto* retainedSource = app.sources_[0].get();
        app.paths_[0] = L"emby://detail-server/playing";
        App::EmbyPane pane;
        pane.itemId = "playing";
        pane.serial = 42;
        bindEmbyAccount(app, pane);
        app.embyPanes_[0] = pane;
        app.audioMask_ = 1U;
        app.clock_.seek(36.0);
        app.clock_.play();
        mutablePlaybackQueue(app).items = {movies->items[2], movies->items[0]};
        mutablePlaybackQueue(app).cursor = 1;
        mutablePlaybackQueue(app).page = "another source page";
        const auto sourcePage = App::embyPageKey(app.embyPages_.back());
        const auto listSerial = app.embyRequestSerial_;
        app.embyActivateItem(1);
        require(app.embyDetails_ && app.embyDetails_->item.id == "a" &&
                app.embyDetails_->libraryId == "movies-a" && app.embyDetails_->sourceChosen == 1 &&
                app.embyDetails_->sourcePage == sourcePage && app.embyDetails_->sourceScroll == 314.0F,
                "The movie detail did not retain its source context");
        require(app.settingsScroll_ == 0.0F && app.embyPages_.size() == 2 &&
                detailItemIds(app.embyItems_) == std::vector<std::string>{"c", "a", "b"},
                "Opening a detail changed the source page or its order");
        const auto requirePlayback = [&] {
            require(app.sources_[0].get() == retainedSource && app.paths_[0] == L"emby://detail-server/playing" &&
                    app.embyPanes_[0] && app.embyPanes_[0]->serial == 42 && app.audioMask_ == 1U &&
                    app.clock_.isPlaying() && app.clock_.position() >= 36.0,
                    "Read-only detail browsing changed current playback");
            require(detailItemIds(playbackQueue(app).items) == std::vector<std::string>{"b", "c"} &&
                    playbackQueue(app).cursor == 1 && playbackQueue(app).page == "another source page",
                    "Read-only detail browsing replaced the active playlist");
        };
        requirePlayback();
        app.embyListArrived(listSerial, 0, detailReply(R"({"Items":[{"Id":"replacement","Type":"Movie"}]})"));
        require(detailItemIds(app.embyItems_) == std::vector<std::string>{"c", "a", "b"},
                "An in-flight source-list reply changed the page behind a detail");
        app.embyDetailsArrived(app.embyDetails_->serial, detailReply(R"({
            "Id":"a","Name":"Aurora","Type":"Movie","ProductionYear":2024,"RunTimeTicks":72000000000,
            "OfficialRating":"PG-13","CommunityRating":8.2,"CriticRating":92,"Genres":["Adventure","Science Fiction"],
            "Overview":"Synthetic story with enough text to expose an expandable synopsis.","Taglines":["Synthetic tagline"],
            "Studios":[{"Name":"Synthetic studio"}],
            "People":[{"Name":"Synthetic director","Type":"Director"},{"Name":"Synthetic actor","Type":"Actor"}],
            "ImageTags":{"Primary":"poster-tag","Logo":"logo-tag"},"BackdropImageTags":["backdrop-tag"],
            "MediaStreams":[{"Type":"Video","Codec":"hevc","Width":3840,"Height":2160,"VideoRange":"HDR10"},
                            {"Type":"Audio","Codec":"aac","Channels":6,"SampleRate":48000,"Language":"eng","IsDefault":true}],
            "UserData":{"PlaybackPositionTicks":18000000000}
        })"));
        const auto rows = app.buildEmbyRows(1280.0F);
        const auto hero = std::find_if(rows.begin(), rows.end(), [](const PanelRow& row) {
            return row.kind == PanelRowKind::MediaDetail;
        });
        require(hero != rows.end() && hero->id == SettingId::EmbyDetailAction && hero->mediaDetail.title == L"Aurora" &&
                hero->mediaDetail.metadata.find(L"2024") != std::wstring::npos &&
                hero->mediaDetail.metadata.find(L"PG-13") != std::wstring::npos &&
                hero->mediaDetail.metadata.find(L"8.2") != std::wstring::npos &&
                hero->mediaDetail.metadata.find(L"Adventure") != std::wstring::npos,
                "The App did not map movie metadata into the native detail row");
        require(hero->mediaDetail.mediaInfo.find(L"HEVC") != std::wstring::npos &&
                hero->mediaDetail.mediaInfo.find(L"AAC") != std::wstring::npos &&
                hero->mediaDetail.mediaInfo.find(L"Synthetic director") != std::wstring::npos &&
                hero->mediaDetail.mediaInfo.find(L"Synthetic actor") != std::wstring::npos &&
                hero->mediaDetail.overview.starts_with(L"Synthetic tagline") &&
                !hero->mediaDetail.posterKey.empty() && !hero->mediaDetail.backdropKey.empty() &&
                !hero->mediaDetail.logoKey.empty() && hero->mediaDetail.progress == 0.25F &&
                hero->options.size() == 5 && hero->options[0].starts_with(L"Resume") &&
                hero->options[1].starts_with(L"From Beginning") && hero->options[2] == L"More" &&
                hero->options[3] == L"Add & resume" && hero->options[4] == L"Add from beginning",
                "The detail row lost artwork, A/V information, progress or playback actions");
        app.embyDetailAction(2);
        require(app.embyDetails_->expanded, "More did not expand the detail synopsis");
        app.embyDetailAction(2);
        require(!app.embyDetails_->expanded, "Less did not collapse the detail synopsis");
        app.embyWantRefresh();
        const auto frozenSerial = app.embyRequestSerial_;
        app.embyRefreshWhenDue(app.embyListAskedTick_ + 60'000);
        require(app.embyRequestSerial_ == frozenSerial, "Detail browsing refreshed its frozen source list");
        app.embyNavigate(0);
        require(!app.embyDetails_ && app.embyRequestSerial_ == frozenSerial && app.settingsScroll_ == 314.0F &&
                app.embyPages_.size() == 2 && detailItemIds(app.embyItems_) == std::vector<std::string>{"c", "a", "b"},
                "Back from a detail fetched or reshuffled Random, or lost the source scroll");
        requirePlayback();
    }

    static void embyLibraryDetailTogglePreservesOriginalActions() {
        App app;
        HiddenWindow parent;
        prepareDetailBrowser(app, parent);
        const RECT client{0, 0, 1280, 900};
        const auto toggle = [&] {
            app.refreshPanelGeometry(client);
            int index = -1;
            for (std::size_t i = 0; i < app.panelRows_.size(); ++i) {
                if (app.panelRows_[i].id == SettingId::EmbyLibraryDetails) index = static_cast<int>(i);
            }
            require(index >= 0, "The known movie/TV library has no information-page toggle");
            app.activatePanelHit({PanelHitKind::Toggle, index});
            // The hidden STATIC window never dispatches a save timer. Cancel
            // it explicitly so this test cannot touch real settings.
            KillTimer(parent.window, app_internal::kSettingsSaveTimer);
            app.settingsSavePending_ = false;
        };
        require(embyLibraryDetailsEnabled(app.embyLibraries_, "detail-server", "movies-a", "movies"),
                "A known movie library does not default to information pages");
        toggle();
        const auto saved = app.captureAppSettings();
        require(!embyLibraryDetailsEnabled(saved.embyLibraries, "detail-server", "movies-a", "movies") &&
                embyLibraryDetailsEnabled(saved.embyLibraries, "detail-server", "movies-b", "movies") &&
                embyLibraryDetailsEnabled(saved.embyLibraries, "detail-server", "television", "tvshows"),
                "The library toggle did not reach captured settings or affected another library");
        emby::Item movie;
        movie.id = "movie"; movie.name = "Synthetic film"; movie.type = "Movie";
        app.embyItems_ = {movie};
        app.embyActivateItem(0);
        require(!app.embyDetails_ && app.paths_[0] == L"emby://detail-server/movie",
                "Disabling movie information pages did not restore direct playback");
        app.embyPages_.back().id = "television";
        app.embyPages_.back().collectionType = "tvshows";
        app.settingsOpen_ = true;
        emby::Item series;
        series.id = "show"; series.name = "Synthetic show"; series.type = "Series"; series.isFolder = true;
        app.embyItems_ = {series};
        app.embyActivateItem(0);
        require(app.embyDetails_ && app.embyDetails_->item.id == "show",
                "The movie-library toggle disabled TV information pages");
        app.embyCloseDetails();
        toggle();
        app.embyActivateItem(0);
        require(!app.embyDetails_ && app.embyPages_.back().kind == App::EmbyPage::Kind::Series &&
                app.embyPages_.back().id == "show", "Disabling TV information pages did not restore its season list");
        auto otherServer = app.emby_.session();
        otherServer.serverId = "other-server";
        app.embyConfigure(otherServer);
        app.embyPages_.resize(2);
        app.embyPages_.back().id = "movies-a";
        require(embyLibraryDetailsEnabled(app.captureAppSettings().embyLibraries, "other-server", "movies-a", "movies") &&
                !embyLibraryDetailsEnabled(app.captureAppSettings().embyLibraries, "detail-server", "movies-a", "movies"),
                "The preference used a name/library ID without its server identity");
    }

    static void embyDetailsRequireKnownMovieOrTvLibrary() {
        App app;
        HiddenWindow parent;
        prepareDetailBrowser(app, parent);
        emby::Item movie;
        movie.id = "movie"; movie.name = "Synthetic film"; movie.type = "Movie";
        app.embyItems_ = {movie};
        for (const char* library : {"home-videos", "unknown", "not-in-views"}) {
            app.embyPages_.back().id = library;
            app.embyPages_.back().collectionType = "movies"; // stale/guessed page metadata is insufficient
            require(!app.embyTryOpenDetails(movie, 0) && !app.embyDetails_,
                    "A movie item enabled details for an unknown or unrelated library");
            for (const auto& row : app.buildEmbyRows(1280.0F)) {
                require(row.id != SettingId::EmbyLibraryDetails,
                        "An unknown or unrelated library shows the movie/TV detail preference");
            }
        }
        app.embyPages_.back().id = "movies-a";
        for (const char* type : {"Video", "Photo", "Playlist", "Folder", "Season", "Episode"}) {
            auto other = movie;
            other.type = type;
            require(!app.embyTryOpenDetails(other, 0), "A non-Movie/Series item entered the new information flow");
        }
        App::EmbyPage search;
        search.kind = App::EmbyPage::Kind::Search;
        search.term = "Synthetic";
        app.embyPages_ = {App::EmbyPage{}, search};
        require(!app.embyTryOpenDetails(movie, 0), "An unscoped search guessed a movie library");
        app.embyRememberLibrary(movie, "movies-a");
        require(app.embyTryOpenDetails(movie, 0) && app.embyDetails_->libraryId == "movies-a",
                "A search item with proven unique library membership lost its information page");
        app.embyCloseDetails();
        app.embyRememberLibrary(movie, "movies-b");
        require(!app.embyTryOpenDetails(movie, 0), "Ambiguous library membership silently picked one preference");
        app.embyPages_.resize(1);
        movie.parentId = "movies-a";
        app.embyResume_ = {movie};
        require(app.embyTryOpenDetails(movie, -1) && app.embyDetails_->sourcePage == "resume" &&
                app.embyDetails_->sourceChosen == 0 && app.embyDetails_->sourceItems.size() == 1,
                "Continue watching with an explicit known library lost its source context");
        app.embyCloseDetails();
        auto anonymousServer = app.emby_.session();
        anonymousServer.serverId.clear();
        app.embyConfigure(anonymousServer);
        app.embyPages_.push_back(App::EmbyPage{});
        app.embyPages_.back().kind = App::EmbyPage::Kind::Library;
        app.embyPages_.back().id = "movies-a";
        require(!app.embyTryOpenDetails(movie, 0), "Details were enabled without a persistent server identity");
    }

    static void embyMovieDetailActionsHonorFreshProgress() {
        App app;
        HiddenWindow parent;
        prepareDetailBrowser(app, parent);
        emby::Item movie;
        movie.id = "movie"; movie.name = "Synthetic film"; movie.type = "Movie";
        movie.runTimeTicks = 6000000000;
        movie.positionTicks = 300000000;
        app.embyItems_ = {movie};
        const auto fresh = detailReply(R"({"Id":"movie","Name":"Synthetic film","Type":"Movie",
            "RunTimeTicks":6000000000,"UserData":{"PlaybackPositionTicks":4200000000,"Played":true}})");
        for (const int action : {1, 0}) {
            app.embyCloseDetails();
            app.closePane(0);
            app.settingsOpen_ = true;
            app.embyActivateItem(0);
            require(app.embyDetails_.has_value(), "Movie click bypassed the enabled information page");
            app.embyDetailsArrived(app.embyDetails_->serial, fresh);
            const auto rows = app.buildEmbyRows(1280.0F);
            const auto hero = std::find_if(rows.begin(), rows.end(), [](const PanelRow& row) {
                return row.id == SettingId::EmbyDetailAction;
            });
            require(hero != rows.end() && hero->options[0].starts_with(L"Resume"), "A rewatch lost its detail Resume action");
            // The actual panel activation dispatches the fixed action slot,
            // including when a narrow layout places the second one below it.
            app.panelRows_ = rows;
            app.activatePanelHit({PanelHitKind::Button, static_cast<int>(hero - rows.begin()), action});
            KillTimer(parent.window, app_internal::kSettingsSaveTimer);
            app.settingsSavePending_ = false;
            require(app.embyPanes_[0] && app.paths_[0] == L"emby://detail-server/movie" &&
                    app.embyPanes_[0]->autoplay && app.embyPanes_[0]->fromBeginning == (action == 1),
                    "A detail playback action did not open its target with the chosen start intent");
            const auto serial = app.embyPanes_[0]->serial;
            app.embyItemArrived(serial, fresh);
            require(app.embyPanes_[0] && app.embyPanes_[0]->resumeTicks == (action == 1 ? 0 : 4200000000),
                    "Fresh progress overrode From Beginning, or Resume ignored the fresh position");
            require(playbackQueue(app).items.size() == 1 && playbackQueue(app).items[0].id == "movie" &&
                    playbackQueue(app).cursor == 0, "An explicit movie action did not adopt its source list");
        }
    }

    static void embyDetailRepliesCannotReviveOldPagesOrAccounts() {
        App app;
        HiddenWindow parent;
        prepareDetailBrowser(app, parent);
        emby::Item movie;
        movie.id = "movie"; movie.name = "Original fallback"; movie.type = "Movie";
        app.embyItems_ = {movie};
        app.embyActivateItem(0);
        const auto old = app.embyDetails_->serial;
        app.embyNavigate(0);
        app.embyDetailsArrived(old, detailReply(R"({"Id":"movie","Name":"Late","Type":"Movie"})"));
        require(!app.embyDetails_, "A reply resurrected a detail after Back");
        app.embyActivateItem(0);
        const auto current = app.embyDetails_->serial;
        EmbyClient::Response unauthorized;
        unauthorized.status = 401;
        app.embyDetailsArrived(old, unauthorized);
        require(app.emby_.session().signedIn() && app.embyDetails_->serial == current,
                "An old detail's unauthorized reply expired the current account");
        app.embyDetailsArrived(current, detailReply(R"({"Id":"different","Name":"Wrong item","Type":"Movie"})"));
        require(app.embyDetails_->item.name == "Original fallback" && !app.embyDetails_->error.empty() &&
                !app.embyDetails_->loading, "A mismatched detail reply replaced the selected item or lost its fallback");
        app.embyRequestDetails();
        const auto refreshed = app.embyDetails_->serial;
        app.embyDetailsArrived(current, detailReply(R"({"Id":"movie","Name":"Obsolete refresh","Type":"Movie"})"));
        require(app.embyDetails_->serial == refreshed && app.embyDetails_->item.name == "Original fallback",
                "An older detail request replaced the newer refresh");
        auto otherAccount = app.emby_.session();
        otherAccount.userId = "other-user";
        otherAccount.token = "other-synthetic-token";
        app.embyConfigure(otherAccount);
        app.embyDetailsArrived(refreshed, unauthorized);
        require(!app.embyDetails_ && app.emby_.session().userId == "other-user" &&
                app.emby_.session().token == "other-synthetic-token",
                "A late detail reply crossed an account change");
    }

    static EmbyClient::Response detailEpisodePage(const std::string& season, int first, int count, int total) {
        nlohmann::json document{{"Items", nlohmann::json::array()}, {"TotalRecordCount", total}};
        for (int number = first; number < first + count; ++number) {
            document["Items"].push_back({{"Id", season + "-e" + std::to_string(number)},
                {"Name", "Synthetic episode " + std::to_string(number)}, {"Type", "Episode"},
                {"SeriesId", "show"}, {"SeasonId", season}, {"IndexNumber", number}, {"ParentIndexNumber", 1},
                {"RunTimeTicks", 18000000000LL}, {"UserData", {{"PlaybackPositionTicks", number == 1 ? 300000000LL : 0LL}}}});
        }
        return detailReply(document.dump());
    }

    static void embySeriesDetailsKeepBrowsingSeparateFromPlayback() {
        App app;
        HiddenWindow parent;
        prepareDetailBrowser(app, parent);
        app.embyPages_.back().id = "television";
        emby::Item series;
        series.id = "show"; series.name = "Synthetic show"; series.type = "Series"; series.isFolder = true;
        app.embyItems_ = {series};
        const auto wholeSeason = emby::parseItems(detailEpisodePage("season-one", 1, 600, 600).body);
        require(wholeSeason && wholeSeason->items.size() == 600, "Cannot construct the long-season regression fixture");
        mutablePlaybackQueue(app).items = wholeSeason->items;
        mutablePlaybackQueue(app).cursor = 250;
        mutablePlaybackQueue(app).page = "detail-season:show/season-one";
        const auto seasons = detailReply(R"({"Items":[
            {"Id":"specials","Type":"Season","IndexNumber":0},
            {"Id":"season-one","Type":"Season","IndexNumber":1},
            {"Id":"season-two","Type":"Season","IndexNumber":2}
        ],"TotalRecordCount":3})");
        const auto supplySeasons = [&] {
            app.embyDetailChildrenArrived(app.embyDetails_->serial, app.embyDetailChildrenSerial_, "", 0, seasons);
        };
        app.embyActivateItem(0);
        require(app.embyDetails_ && app.embyDetails_->item.type == "Series", "TV click bypassed the enabled detail page");
        supplySeasons();
        require(app.embyDetails_->seasonId == "season-one" && app.embyDetails_->childrenLoading,
                "A series did not load the first regular season before Specials");
        app.embyDetailChildrenArrived(app.embyDetails_->serial, app.embyDetailChildrenSerial_, "season-one", 0,
                                     detailEpisodePage("season-one", 1, 300, 600));
        require(app.embyDetails_->episodes.size() == 300 && app.embyDetails_->childrenLoading &&
                playbackQueue(app).items.size() == 600 && playbackQueue(app).cursor == 250 &&
                !app.embyDetails_->playlistAdopted,
                "Reopening details truncated a 600-episode playing sequence to the first 300");
        const auto retired = app.embyDetails_->serial;
        const auto retiredChildren = app.embyDetailChildrenSerial_;
        app.embyNavigate(0);
        app.embyDetailChildrenArrived(retired, retiredChildren, "season-one", 300,
                                     detailEpisodePage("season-one", 301, 300, 600));
        require(!app.embyDetails_ && playbackQueue(app).items.size() == 600 && playbackQueue(app).cursor == 250,
                "Back or a late detail episode page changed the active long-season playlist");

        app.embyActivateItem(0);
        supplySeasons();
        const auto firstSeasonRequest = app.embyDetailChildrenSerial_;
        app.embyChooseDetailSeason(2);
        const auto secondSeasonRequest = app.embyDetailChildrenSerial_;
        app.embyDetailChildrenArrived(app.embyDetails_->serial, firstSeasonRequest, "season-one", 0,
                                     detailEpisodePage("season-one", 1, 2, 2));
        require(app.embyDetails_->seasonId == "season-two" && app.embyDetails_->episodes.empty() &&
                app.embyDetails_->childrenLoading, "A late episode reply populated a different selected season");
        app.embyDetailChildrenArrived(app.embyDetails_->serial, secondSeasonRequest, "season-two", 0,
                                     detailEpisodePage("season-two", 1, 2, 3));
        require(app.embyDetails_->episodes.size() == 2 && app.embyDetails_->childrenLoading &&
                playbackQueue(app).items.size() == 600, "Browsing another season replaced playback before an explicit action");
        const auto rows = app.buildEmbyRows(1280.0F);
        const auto hero = std::find_if(rows.begin(), rows.end(), [](const PanelRow& row) {
            return row.id == SettingId::EmbyDetailAction;
        });
        const auto firstEpisode = std::find_if(rows.begin(), rows.end(), [](const PanelRow& row) {
            return row.id == SettingId::EmbyDetailEpisode && row.param == 0;
        });
        require(hero != rows.end() && hero->options[0] == L"Choose episode" && hero->options[1].empty() &&
                firstEpisode != rows.end() && firstEpisode->options.size() == 4 &&
                firstEpisode->options[0].starts_with(L"Resume") && firstEpisode->options[1].starts_with(L"From Beginning") &&
                firstEpisode->options[2] == L"Add & resume" && firstEpisode->options[3] == L"Add from beginning",
                "Series information does not expose its explicit season/episode playback controls");
        app.embyDetailAction(0);
        require(app.paths_[0].empty() && playbackQueue(app).items.size() == 600,
                "The series hero guessed an episode and started playback");
        app.panelRows_ = rows;
        app.activatePanelHit({PanelHitKind::Button, static_cast<int>(firstEpisode - rows.begin()), 1});
        KillTimer(parent.window, app_internal::kSettingsSaveTimer);
        app.settingsSavePending_ = false;
        require(playbackQueue(app).items.size() == 2 && playbackQueue(app).cursor == 0 &&
                app.paths_[0] == L"emby://detail-server/season-two-e1" && app.embyPanes_[0]->fromBeginning,
                "Explicit episode From Beginning did not adopt that season or preserve the chosen start");
        app.embyItemArrived(app.embyPanes_[0]->serial, detailReply(R"({"Id":"season-two-e1","Type":"Episode",
            "RunTimeTicks":18000000000,"UserData":{"PlaybackPositionTicks":9000000000}})"));
        require(app.embyPanes_[0]->resumeTicks == 0, "Fresh episode progress overrode the explicit beginning action");
        app.embyDetailChildrenArrived(app.embyDetails_->serial, app.embyDetailChildrenSerial_, "season-two", 2,
                                     detailEpisodePage("season-two", 3, 1, 3));
        require(playbackQueue(app).items.size() == 3 && playbackQueue(app).cursor == 0 && !app.embyDetails_->childrenLoading,
                "A later episode page did not extend the explicitly selected season sequence");
        app.embyRequestDetails();
        require(app.embyDetails_->seasonId.empty() && app.embyDetails_->seasons.empty() &&
                app.embyDetails_->preferredSeasonId == "season-two", "Refresh did not re-fetch seasons while retaining selection");
        app.embyDetailChildrenArrived(app.embyDetails_->serial, app.embyDetailChildrenSerial_, "", 0,
            detailReply(R"({"Items":[{"Id":"season-one","Type":"Season","IndexNumber":1},
                {"Id":"season-two","Type":"Season","IndexNumber":2},
                {"Id":"season-three","Type":"Season","IndexNumber":3}],"TotalRecordCount":3})"));
        require(app.embyDetails_->seasons.size() == 3 && app.embyDetails_->seasonId == "season-two",
                "Refresh hid a newly added season or forgot the previous selected season");
        app.embyDetailChildrenArrived(app.embyDetails_->serial, app.embyDetailChildrenSerial_, "season-two", 0,
                                     detailEpisodePage("season-two", 1, 2, 3));
        require(playbackQueue(app).items.size() == 3 && playbackQueue(app).cursor == 0,
                "Refreshing details shortened a playlist that was already playing");
        EmbyClient::Response failed;
        failed.error = "Synthetic detail page failure";
        app.embyDetailChildrenArrived(app.embyDetails_->serial, app.embyDetailChildrenSerial_, "season-two", 2, failed);
        require(!app.embyDetails_->childrenLoading && app.embyDetails_->childrenBlocked,
                "A failed detail episode page automatically retried or stayed loading");
    }

    static void embySearchAndF5KeepTheirOwnContext() {
        App app;
        HiddenWindow parent;
        prepareDetailBrowser(app, parent);
        emby::Item movie;
        movie.id = "movie"; movie.name = "Synthetic film"; movie.type = "Movie";
        app.embyItems_ = {movie};
        App::EmbyPage search;
        search.kind = App::EmbyPage::Kind::Search;
        search.term = "Synthetic";
        app.embyPages_ = {App::EmbyPage{}, search};
        app.embyRememberLibrary(movie, "movies-a");
        app.embyLoading_ = true;
        for (const int view : {0, 1, 2}) {
            app.embyBrowser_.view = view;
            for (const auto& row : app.buildEmbyRows(1280.0F)) {
                if (row.id == SettingId::EmbyItem)
                    require(!row.enabled, "An incomplete multi-library Search exposes clickable item rows");
            }
            app.embyActivateItem(0);
            require(!app.embyDetails_ && app.paths_[0].empty() && playbackQueue(app).items.empty(),
                    "A partial Search retired the remaining library requests or started playback");
        }
        app.embyLoading_ = false;
        app.embyRememberLibrary(movie, "movies-b");
        require(!app.embyTryOpenDetails(movie, 0), "Completed Search guessed between two libraries with different preferences");
        app.embyPages_.back().kind = App::EmbyPage::Kind::Library;
        app.embyPages_.back().id = "movies-a";
        app.settingsScroll_ = 500.0F;
        app.embyActivateItem(0);
        require(app.embyDetails_ && !app.embyPageAcceptsFlat(), "A detail still accepts the source-list flatten shortcut");
        app.embyBrowserOpen_ = false;
        app.settingsScroll_ = 200.0F;
        app.embyCloseDetails();
        require(app.settingsScroll_ == 200.0F && app.embyListScroll_ == 500.0F,
                "Closing a hidden detail overwrote the ordinary Settings scroll");
        // The information-page preference is each library page's own
        // toggle; the settings sheet lists none.
        for (const auto tab : {SettingsTab::Playback, SettingsTab::Audio, SettingsTab::Subtitles,
                               SettingsTab::Picture, SettingsTab::General}) {
            app.settingsTab_ = tab;
            for (const auto& row : app.buildSettingsRows(1280.0F)) {
                require(row.id != SettingId::EmbyLibraryDetails, "F5 still lists a library's information-page toggle");
            }
        }
    }

    static void embyFirstSignInRetiresOnlyTheCompletedPhase() {
        App app;
        HiddenWindow parent;
        app.window_ = app.videoWindow_ = parent.window;
        const auto directory = std::filesystem::temp_directory_path() /
            (L"QuadDeckDetailSignIn-" + std::to_wstring(GetCurrentProcessId()));
        std::error_code code;
        std::filesystem::create_directories(directory, code);
        require(!code, "Cannot create the isolated first-sign-in fixture directory");
        app.embyAuthPathOverride_ = directory / L"emby.qauth";
        std::filesystem::remove(app.embyAuthPathOverride_, code);
        emby::Session session;
        session.serverUrl = "synthetic fixture address";
        session.userName = "Synthetic user";
        session.deviceId = "synthetic-device";
        app.embyConfigure(session);
        app.embyLoading_ = true;
        app.settingsOpen_ = app.embyBrowserOpen_ = true;
        app.browserSource_ = App::BrowserSource::Emby;
        const auto publicSerial = ++app.embyRequestSerial_;
        app.embyServerInfoArrived(publicSerial, "Synthetic user", "", detailReply(
            R"({"Id":"new-server-id","ServerName":"Synthetic server","Version":"synthetic"})"));
        const auto authenticationSerial = app.embyRequestSerial_;
        require(authenticationSerial > publicSerial && app.emby_.session().serverId == "new-server-id" &&
                app.embyLoading_ && !app.emby_.session().signedIn(),
                "First server identification did not retire the public-info phase cleanly");
        const auto authentication = detailReply(R"({"AccessToken":"synthetic-token","ServerId":"new-server-id",
            "User":{"Id":"synthetic-user","Name":"Synthetic user"}})");
        app.embyAuthenticationArrived(publicSerial, authentication);
        require(!app.emby_.session().signedIn() && !std::filesystem::exists(app.embyAuthPathOverride_),
                "An auth response carrying the retired public-info serial was accepted");
        app.embyAuthenticationArrived(authenticationSerial, authentication);
        require(app.emby_.session().signedIn() && app.emby_.session().serverId == "new-server-id" &&
                app.emby_.session().userId == "synthetic-user" && std::filesystem::exists(app.embyAuthPathOverride_) &&
                app.embyPages_.size() == 1 && app.embyPages_.back().kind == App::EmbyPage::Kind::Home,
                "First sign-in ignored authentication after learning the server ID");
        app.embyAuthenticationArrived(authenticationSerial, detailReply(
            R"({"AccessToken":"wrong-stale-token","User":{"Id":"wrong-stale-user"}})"));
        require(app.emby_.session().userId == "synthetic-user" && app.emby_.session().token == "synthetic-token",
                "A completed auth response replaced the account after the Home request began");
        std::filesystem::remove(app.embyAuthPathOverride_, code);
        std::filesystem::remove(directory, code);
    }

    // Opening the browser must not choose another random order. Only a
    // deliberate sort, refresh or new page may replace the retained list.
    static void embyRandomOrderSurvivesReopening() {
        App app;
        HiddenWindow parent;
        app.window_ = app.videoWindow_ = parent.window;
        app.instance_ = GetModuleHandleW(nullptr);
        emby::Session session;
        session.serverUrl = "http://127.0.0.1:9";
        session.serverId = "srv";
        session.userId = "u1";
        session.token = "token";
        session.deviceId = "dev";
        app.embyConfigure(session);
        app.embyBrowser_.sort = static_cast<int>(emby::SortKey::Random);
        app.embyPages_.push_back(App::EmbyPage{});
        App::EmbyPage page;
        page.kind = App::EmbyPage::Kind::Folder;
        page.id = "folder";
        app.embyPages_.push_back(page);
        const auto reply = [](const std::string& ids) {
            EmbyClient::Response response;
            response.ok = true;
            response.status = 200;
            response.body = "{\"Items\":[";
            for (const char id : ids) {
                if (response.body.back() != '[') response.body += ',';
                response.body += std::string("{\"Id\":\"") + id +
                    "\",\"Name\":\"Movie\",\"Type\":\"Movie\",\"MediaType\":\"Video\"}";
            }
            response.body += "],\"TotalRecordCount\":" + std::to_string(ids.size()) + "}";
            return response;
        };
        const auto ids = [](const std::vector<emby::Item>& items) {
            std::string result;
            for (const auto& item : items) result += item.id;
            return result;
        };
        app.embyOpenBrowser();
        const auto initial = app.embyRequestSerial_;
        require(initial != 0 && app.embyLoading_, "An unseen Random page was not requested");
        app.embyOpenBrowser();
        require(app.embyRequestSerial_ == initial, "Reopening superseded Random's first request");
        app.embyListArrived(initial, 0, reply("cab"));
        app.embyTakePlaylist(app.embyItems_, 1, 0);
        mutablePlaybackQueue(app).page = App::embyPageKey(page);
        app.paths_[0] = L"emby://srv/a";
        App::EmbyPane pane;
        pane.itemId = "a";
        bindEmbyAccount(app, pane);
        app.embyPanes_[0] = pane;
        app.settingsScroll_ = 42.0F;
        app.embyTakePlaylist(app.embyItems_, 1, 0, App::embyPageKey(page));
        for (int i = 0; i < 3; ++i) {
            app.toggleEmbyBrowser();
            app.toggleEmbyBrowser();
            require(app.settingsOpen_ && app.embyBrowserOpen_ && app.embyRequestSerial_ == initial,
                    "F6 reopening requested another random order");
            require(ids(app.embyItems_) == "cab" && ids(playbackQueue(app).items) == "cab" &&
                    playbackQueue(app).cursor == 1 && app.settingsScroll_ == 42.0F,
                    "F6 reopening changed Random's list, playing entry or scroll");
        }
        app.toggleSettingsPanel();
        app.embyOpenBrowser();  // Ctrl+E / Open from Emby / Browse library
        require(app.embyRequestSerial_ == initial && app.settingsScroll_ == 42.0F,
                "Returning from Settings requested another random order");
        app.closeSettingsPanel();
        const RECT client{0, 0, 1280, 720};
        app.embyEdgeHover(POINT{400, 200}, client, true);
        app.embyEdgeHover(POINT{1279, 200}, client, true);
        require(app.settingsOpen_ && app.embyBrowserByEdge_ && app.embyRequestSerial_ == initial,
                "Edge opening requested another random order");

        app.embyWantRefresh();
        app.embyRefreshWhenDue(app.embyListAskedTick_ + 60'000);
        require(app.embyRequestSerial_ == initial, "Random was automatically refreshed");
        app.embyNavigate(3);
        require(app.embyRequestSerial_ == initial + 1 && app.embyRefreshing_,
                "Explicit Refresh did not request Random");
        app.embyOpenBrowser();
        require(app.embyRequestSerial_ == initial + 1, "Reopening superseded an explicit refresh");
        app.embyListArrived(initial + 1, 0, reply("bca"));
        require(ids(app.embyItems_) == "bca" && ids(playbackQueue(app).items) == "cab",
                "Explicit Refresh did not update the view or disturbed the playing order");
        app.embyChooseSort(static_cast<int>(emby::SortKey::Random));
        require(app.embyRequestSerial_ == initial + 2 && !app.embyBrowser_.descending,
                "Choosing Random again did not explicitly reshuffle");
        app.embyListArrived(initial + 2, 0, reply("abc"));

        // Sort keys while hidden invalidate the cached page, not the
        // scroll of Settings or Local; a late old reply cannot undo it.
        app.toggleSettingsPanel();
        app.settingsScroll_ = 73.0F;
        app.sortListBy(static_cast<int>(emby::SortKey::Name));
        app.sortListBy(static_cast<int>(emby::SortKey::Random));
        const auto hidden = app.embyRequestSerial_;
        require(app.embyItems_.empty() && !app.embyLoading_ && app.settingsScroll_ == 73.0F,
                "A hidden sort kept a stale page or moved another sheet");
        app.embyListArrived(initial + 2, 0, reply("cab"));
        require(app.embyItems_.empty(), "An old reply revived the invalidated random page");
        app.embyOpenBrowser();
        require(app.embyRequestSerial_ == hidden + 1 && app.embyLoading_,
                "Reopening ignored an explicit hidden sort");
        app.embyListArrived(hidden + 1, 0, reply("acb"));
        app.embyOpenBrowser();
        require(app.embyRequestSerial_ == hidden + 1 && ids(app.embyItems_) == "acb",
                "The newly chosen random order was not retained");
        app.closeSettingsPanel();
        app.sortListBy(static_cast<int>(emby::SortKey::Random));
        require(app.embyItems_.empty(), "Ctrl+9 while already Random kept the old cached order");
        app.embyOpenBrowser();
        const auto failed = app.embyRequestSerial_;
        EmbyClient::Response away;
        away.error = "Server away";
        app.embyListArrived(failed, 0, away);
        app.embyOpenBrowser();
        require(app.embyRequestSerial_ == failed + 1 && app.embyLoading_,
                "An empty failed Random page could not be retried");
        app.embyListArrived(failed + 1, 0, reply("bca"));

        // Search remains cached. Pages that do not apply Random still
        // refresh, even though Random is the saved browser preference.
        app.embyPages_.back().kind = App::EmbyPage::Kind::Search;
        const auto search = app.embyRequestSerial_;
        app.embyOpenBrowser();
        require(app.embyRequestSerial_ == search, "Reopening repeated a merged search");
        app.embyPages_.back().kind = App::EmbyPage::Kind::Playlist;
        app.embyPages_.back().ownOrder = false;
        app.embyOpenBrowser();
        require(app.embyRequestSerial_ == search, "Reopening reshuffled a Random playlist page");
        for (const auto kind : {App::EmbyPage::Kind::Home, App::EmbyPage::Kind::Series,
                                App::EmbyPage::Kind::Season, App::EmbyPage::Kind::Playlist}) {
            app.embyPages_.back().kind = kind;
            app.embyPages_.back().ownOrder = true;
            const auto before = app.embyRequestSerial_;
            app.embyOpenBrowser();
            require(app.embyRequestSerial_ == before + 1 && app.embyRefreshing_,
                    "Saved Random preference blocked a page with its own order");
            app.embyListArrived(before + 1, 0, reply("abc"));
        }
        app.embyPages_.back() = page;
        app.embyBrowser_.sort = static_cast<int>(emby::SortKey::Name);
        const auto named = app.embyRequestSerial_;
        app.embyOpenBrowser();
        require(app.embyRequestSerial_ == named + 1 && app.embyRefreshing_,
                "Reopening an ordinary order no longer refreshes");
        app.embyListArrived(named + 1, 0, reply("abc"));
        app.embyLoadMore();
        const auto more = app.embyRequestSerial_;
        app.embyOpenBrowser();
        require(app.embyRequestSerial_ == more && app.embyLoadingMore_,
                "Reopening superseded a further page in flight");
        app.embyListArrived(more, 3, reply("d"));
        app.browserSource_ = App::BrowserSource::Local;
        app.settingsScroll_ = 84.0F;
        app.sortListBy(static_cast<int>(emby::SortKey::Random));
        require(app.embyItems_.empty() && !app.embyLoading_ && !app.embyLoadingMore_ &&
                app.settingsScroll_ == 84.0F && app.browserSource_ == App::BrowserSource::Local,
                "Sorting Local requested the hidden Emby page or moved Local's scroll");
    }

    // What was watched or changed on the server since a page was asked for
    // shows up without the browser being closed: the page is asked for
    // again in place when there is news and every half minute, the list
    // and the press on it staying true, the list being played not re-ordered.
    // The Emby browser marks the videos that have subtitles to show. A
    // list's items do not say, so once a list has come its videos are
    // asked about by id; a list kept fresh unasked asks only about videos
    // it had not shown, Refresh asks again, another account forgets.
    static void embyBrowserMarksVideosWithSubtitles() {
        App app;
        HiddenWindow parent;
        app.window_ = app.videoWindow_ = parent.window;
        app.instance_ = GetModuleHandleW(nullptr);
        emby::Session session;
        session.serverUrl = "http://127.0.0.1:9";
        session.serverId = "srv";
        session.userId = "u1";
        session.token = "token";
        session.deviceId = "dev";
        app.embyConfigure(session);
        app.settingsOpen_ = true;
        app.settingsAlpha_ = 1.0F;
        app.embyBrowserOpen_ = true;
        app.embyBrowser_.view = 0;
        App::EmbyPage folder;
        folder.kind = App::EmbyPage::Kind::Folder;
        folder.id = "7";
        app.embyPages_.push_back(folder);
        const RECT client{0, 0, 1280, 720};
        const auto reply = [](const std::string& items) {
            EmbyClient::Response response;
            response.ok = true;
            response.status = 200;
            int count = 0;
            for (std::size_t at = items.find("\"Id\""); at != std::string::npos; at = items.find("\"Id\"", at + 1)) ++count;
            response.body = "{\"Items\":[" + items + "],\"TotalRecordCount\":" + std::to_string(count) + "}";
            return response;
        };
        const auto movie = [](const char* id) {
            return std::string("{\"Name\":\"Movie ") + id + "\",\"Id\":\"" + id +
                   "\",\"Type\":\"Movie\",\"MediaType\":\"Video\",\"IsFolder\":false,\"RunTimeTicks\":6000000000}";
        };
        const std::string subfolder = R"({"Name":"Extras","Id":"f","Type":"Folder","IsFolder":true})";
        const auto asking = [&] {
            std::vector<std::string> ids(app.embySubtitleAsking_.begin(), app.embySubtitleAsking_.end());
            std::sort(ids.begin(), ids.end());
            std::string joined;
            for (const auto& id : ids) joined += id;
            return joined;
        };
        const auto tags = [&] {
            app.refreshPanelGeometry(client);
            std::string marked;
            for (const auto& row : app.panelRows_) {
                if (row.id != SettingId::EmbyItem) continue;
                if (row.kind == PanelRowKind::Item) {
                    marked += row.tag == L"Sub" ? "S" : row.tag.empty() ? "-" : "?";
                } else if (row.kind == PanelRowKind::Tiles) {
                    for (const auto& tag : row.tileTags) marked += tag == L"Sub" ? "S" : tag.empty() ? "-" : "?";
                }
            }
            return marked;
        };

        app.embyRequestPage();
        const std::uint64_t opened = app.embyRequestSerial_;
        app.embyListArrived(opened, 0, reply(subfolder + "," + movie("a") + "," + movie("b") + "," + movie("c")));
        require(app.embyItems_.size() == 4 && asking() == "abc",
                "The videos of a list that came were not asked about, or the folder was");
        require(tags() == "----", "A video was marked before the server said so");
        // The answer names the ones with subtitles; the rest have none.
        app.embySubtitlesArrived(app.embyAccountSerial_, {"a", "b"}, reply(movie("a")));
        require(asking() == "c" && app.embySubtitled_.at("a") && !app.embySubtitled_.at("b") &&
                    !app.embySubtitled_.contains("c"),
                "An answer was not taken as it was given");
        // A failure leaves the video unmarked, to be asked about later.
        EmbyClient::Response failed;
        failed.status = 0;
        failed.error = "connection refused";
        app.embySubtitlesArrived(app.embyAccountSerial_, {"c"}, failed);
        require(asking().empty() && !app.embySubtitled_.contains("c"), "A failed question was taken as an answer");
        require(tags() == "-S--", "The list does not mark the video with subtitles");
        app.embyBrowser_.view = 2;
        require(tags() == "-S--", "The tiles do not mark the video with subtitles");
        app.embyBrowser_.view = 0;

        // Kept fresh unasked: only what had not been answered is asked.
        app.embyRequestPage(0, true);
        app.embyListArrived(app.embyRequestSerial_, 0,
                            reply(subfolder + "," + movie("a") + "," + movie("b") + "," + movie("c") + "," + movie("d")));
        require(asking() == "cd", "A list kept fresh asked again about videos already answered");
        app.embySubtitlesArrived(app.embyAccountSerial_, {"c", "d"}, reply(movie("d")));
        require(tags() == "-S--S", "A video new to the list was not marked");
        // Refresh asks again about all of them; the marks stay meanwhile.
        app.embyRefreshShown();
        require(app.embyRefreshing_ && app.embySubtitlesAgain_, "Premise: Refresh asks for the page again in place");
        app.embyListArrived(app.embyRequestSerial_, 0,
                            reply(subfolder + "," + movie("a") + "," + movie("b") + "," + movie("c") + "," + movie("d")));
        require(asking() == "abcd" && !app.embySubtitlesAgain_ && tags() == "-S--S",
                "Refresh did not ask again, or the marks went while it did");
        app.embySubtitlesArrived(app.embyAccountSerial_, {"a", "b", "c", "d"}, reply(movie("b") + "," + movie("d")));
        require(tags() == "--S-S", "What Refresh found was not shown");
        // A further page asks about its own videos.
        app.embyTotal_ = 6;
        app.embyRequestPage(5);
        app.embyListArrived(app.embyRequestSerial_, 5, reply(movie("e")));
        require(asking() == "e" && app.embyItems_.size() == 6, "A further page's videos were not asked about");

        // Another account: what was answered is forgotten, and an answer
        // for the one before is dropped.
        const auto before = app.embyAccountSerial_;
        emby::Session other = session;
        other.userId = "u2";
        app.embyConfigure(other);
        require(app.embySubtitled_.empty() && asking().empty(), "Another account kept the answers of the one before");
        app.embySubtitlesArrived(before, {"e"}, reply(movie("e")));
        require(app.embySubtitled_.empty(), "An answer for the account before was taken");
    }

    static void embyBrowserKeepsItselfFresh() {
        App app;
        HiddenWindow parent;
        app.window_ = app.videoWindow_ = parent.window;
        app.instance_ = GetModuleHandleW(nullptr);
        emby::Session session;
        session.serverUrl = "http://127.0.0.1:9";
        session.serverId = "srv";
        session.userId = "u1";
        session.token = "token";
        session.deviceId = "dev";
        app.embyConfigure(session);
        app.settingsOpen_ = true;
        app.settingsAlpha_ = 1.0F;
        app.embyBrowserOpen_ = true;
        app.embyBrowser_.view = 0;
        app.embyBrowser_.sort = static_cast<int>(emby::SortKey::LastPlayed);
        app.embyBrowser_.descending = true;
        app.embyPages_.push_back(App::EmbyPage{});
        emby::Item view;
        view.id = "114041";
        view.name = "Films";
        view.type = "CollectionFolder";
        view.collectionType = "movies";
        view.isFolder = true;
        app.embyItems_ = {view};
        const RECT client{0, 0, 1280, 720};
        // A page never asked for is not asked for again, whatever is wanted.
        const auto beforeRefresh = app.embyRequestSerial_;
        app.embyWantRefresh();
        app.embyRefreshWhenDue(GetTickCount64() + 60'000);
        require(app.embyRequestSerial_ == beforeRefresh && app.embyListAskedTick_ == 0 && !app.embyRefreshing_,
                "A list the server was never asked for was refreshed");

        app.embyActivateItem(0);
        const std::uint64_t opened = app.embyRequestSerial_;
        const unsigned long long asked = app.embyListAskedTick_;
        require(asked != 0 && app.embyRefreshWantedTick_ == 0, "Asking for a page did not note when");
        const auto reply = [](const std::string& items) {
            EmbyClient::Response response;
            response.ok = true;
            response.status = 200;
            int count = 0;
            for (std::size_t at = items.find("\"Id\""); at != std::string::npos; at = items.find("\"Id\"", at + 1)) ++count;
            response.body = "{\"Items\":[" + items + "],\"TotalRecordCount\":" + std::to_string(count) + "}";
            return response;
        };
        const auto movie = [](const char* id, const char* name, long long position = 0) {
            return std::string("{\"Name\":\"") + name + "\",\"Id\":\"" + id +
                   "\",\"Type\":\"Movie\",\"MediaType\":\"Video\",\"IsFolder\":false,\"RunTimeTicks\":6000000000,"
                   "\"UserData\":{\"PlaybackPositionTicks\":" + std::to_string(position) + ",\"Played\":false}}";
        };
        app.embyListArrived(opened, 0, reply(movie("a", "Alpha") + "," + movie("b", "Bravo") + "," + movie("c", "Charlie")));
        require(app.embyItems_.size() == 3 && !app.embyLoading_, "The page did not arrive");
        app.refreshPanelGeometry(client);
        bool refreshButton = false;
        for (const auto& row : app.panelRows_) {
            if (row.id != SettingId::EmbyNav) continue;
            refreshButton = row.options.size() == 4 && row.options[3] == L"Refresh";
        }
        require(refreshButton, "The browser offers no Refresh");

        // Nothing is due at once, nor a second after news.
        app.embyRefreshWhenDue(asked + 1'000);
        app.embyWantRefresh();
        require(app.embyRefreshWantedTick_ != 0, "News was not noted");
        app.embyRefreshWhenDue(asked + 1'000);
        require(app.embyRequestSerial_ == opened, "The page was asked for again within a second of being asked for");
        // Not while a row is pressed, nor with the browser away.
        app.pressedPanelKind_ = PanelHitKind::Item;
        app.embyRefreshWhenDue(asked + 5'000);
        app.pressedPanelKind_ = PanelHitKind::None;
        app.settingsOpen_ = false;
        app.embyRefreshWhenDue(asked + 5'000);
        app.settingsOpen_ = true;
        require(app.embyRequestSerial_ == opened, "The page was asked for again under a press or with the browser away");
        // Then it is: in place, saying nothing, the list left as it is.
        app.settingsScroll_ = 12.0F;
        app.embyRefreshWhenDue(asked + 5'000);
        require(app.embyRequestSerial_ == opened + 1 && app.embyRefreshing_ && !app.embyLoading_ &&
                app.embyItems_.size() == 3 && app.settingsScroll_ == 12.0F && app.embyRefreshWantedTick_ == 0,
                "News did not ask for the page again in place");
        // Its next page does not overtake it, however the list is scrolled.
        app.embyTotal_ = 900;
        app.settingsScroll_ = 1.0e9F;
        app.refreshPanelGeometry(client);
        require(app.embyRequestSerial_ == opened + 1, "A further page overtook the page being asked for again");
        app.embyTotal_ = 3;
        app.settingsScroll_ = 0.0F;

        // "b" was chosen from the list and plays; a row of the list is
        // pressed when the reply lands with "b" watched elsewhere and now
        // first, as the order by last played has it.
        app.embyTakePlaylist(app.embyItems_, 1, 0);
        mutablePlaybackQueue(app).page = App::embyPageKey(app.embyPages_.back());
        app.paths_[0] = L"emby://srv/b";
        App::EmbyPane pane;
        pane.itemId = "b";
        pane.serial = ++app.embyPaneSerial_;
        bindEmbyAccount(app, pane);
        app.embyPanes_[0] = pane;
        app.refreshPanelGeometry(client);
        app.embyTakePlaylist(app.embyItems_, 1, 0, App::embyPageKey(app.embyPages_.back()));
        int pressedRow = -1;
        for (std::size_t i = 0; i < app.panelRows_.size(); ++i) {
            if (app.panelRows_[i].id == SettingId::EmbyItem && app.panelRows_[i].param == 0) pressedRow = static_cast<int>(i);
        }
        require(pressedRow >= 0, "Premise: the first item has a row");
        app.pressedPanelKind_ = PanelHitKind::Item;
        app.pressedPanelRow_ = pressedRow;
        app.pressedPanelPart_ = -1;
        app.pressedPanelId_ = SettingId::EmbyItem;
        app.pressedPanelParam_ = 0;
        require(app.embyPressedItem() == "a", "The press does not name its item");
        app.embyListArrived(opened + 1, 0, reply(movie("b", "Bravo", 1800000000) + "," + movie("a", "Alpha") + "," +
                                                 movie("c", "Charlie")));
        require(!app.embyRefreshing_ && app.embyItems_.size() == 3 && app.embyItems_[0].id == "b" &&
                app.embyItems_[0].positionTicks == 1800000000, "The page asked for again did not bring the news");
        require(app.pressedPanelKind_ == PanelHitKind::None,
                "A press stayed on a row that has come to name another item");
        const auto& rowBox = app.panelLayout_.rows[static_cast<std::size_t>(pressedRow)].row;
        app.releasePanelPress(POINT{static_cast<LONG>(rowBox.x + 2.0F), static_cast<LONG>(rowBox.y + 2.0F)});
        require(app.paths_[0] == L"emby://srv/b", "The release chose what had come to stand under the press");
        // The list shown is the server's; the list being played keeps its
        // order, so the step after "b" is still "c", not back to "a".
        std::string playing;
        for (const auto& entry : playbackQueue(app).items) playing += entry.id;
        require(playing == "abc" && playbackQueue(app).cursor == 1, "A page asked for again re-ordered the list being played");
        app.refreshPanelGeometry(client);
        bool newsShown = false;
        for (const auto& row : app.panelRows_) {
            newsShown = newsShown || (row.id == SettingId::EmbyItem && row.param == 0 && row.current &&
                                      row.value == L"\x25B6 30%   10:00");
        }
        require(newsShown, "The list does not show what was watched elsewhere");
        // A press that still names its item is left alone.
        app.pressedPanelKind_ = PanelHitKind::Item;
        app.pressedPanelRow_ = pressedRow;
        app.pressedPanelId_ = SettingId::EmbyItem;
        app.pressedPanelParam_ = 0;
        app.embyKeepPressOn("b");
        require(app.pressedPanelKind_ == PanelHitKind::Item, "A press on an item that did not move was given up");
        app.pressedPanelKind_ = PanelHitKind::None;
        app.pressedPanelId_ = SettingId::None;
        app.pressedPanelParam_ = -1;

        // Half a minute on it is asked for again unasked; with the order
        // kept, a video added since joins the list being played.
        const unsigned long long second = app.embyListAskedTick_;
        app.embyRefreshWhenDue(second + 29'000);
        require(app.embyRequestSerial_ == opened + 1, "The page was asked for again before half a minute was up");
        app.embyRefreshWhenDue(second + 30'000);
        require(app.embyRequestSerial_ == opened + 2 && app.embyRefreshing_ && app.embyRefreshQuiet_,
                "Half a minute did not ask for the page again");
        mutablePlaybackQueue(app).items = app.embyItems_;
        mutablePlaybackQueue(app).cursor = 0;
        app.embyListArrived(opened + 2, 0, reply(movie("b", "Bravo", 1800000000) + "," + movie("n", "New") + "," +
                                                 movie("a", "Alpha") + "," + movie("c", "Charlie")));
        playing.clear();
        for (const auto& entry : playbackQueue(app).items) playing += entry.id;
        require(playing == "bnac" && playbackQueue(app).cursor == 0, "A video added since did not join the list being played");
        // Unasked and failing, it leaves the list and says nothing over it.
        const unsigned long long third = app.embyListAskedTick_;
        app.embyRefreshWhenDue(third + 30'000);
        require(app.embyRequestSerial_ == opened + 3, "Premise: the third asking");
        EmbyClient::Response away;
        away.error = "The server is away";
        app.embyListArrived(opened + 3, 0, away);
        require(app.embyItems_.size() == 4 && app.embyNote_.empty() && !app.embyRefreshing_,
                "A refresh nobody asked for wrote its failure over the list");
        // Asked for by hand and failing, it says so; coming back, it says no more.
        app.embyNavigate(3);
        require(app.embyRequestSerial_ == opened + 4 && app.embyRefreshing_ && !app.embyRefreshQuiet_ &&
                app.embyItems_.size() == 4, "Refresh did not ask for the page again in place");
        app.embyListArrived(opened + 4, 0, away);
        require(!app.embyNote_.empty() && app.embyItems_.size() == 4, "A refresh asked for by hand failed without a word");
        app.embyNavigate(3);
        app.embyListArrived(opened + 5, 0, reply(movie("b", "Bravo") + "," + movie("a", "Alpha")));
        require(app.embyNote_.empty() && app.embyItems_.size() == 2, "The page that came back still speaks of the failure");

        // A random order is not asked for again unasked -- the list would
        // be shuffled under the viewer -- but Refresh does it.
        app.embyBrowser_.sort = static_cast<int>(emby::SortKey::Random);
        const unsigned long long fourth = app.embyListAskedTick_;
        app.embyWantRefresh();
        app.embyRefreshWhenDue(fourth + 60'000);
        require(app.embyRequestSerial_ == opened + 5, "A random order was asked for again unasked");
        app.embyNavigate(3);
        require(app.embyRequestSerial_ == opened + 6, "Refresh did not ask for a random order again");
        app.embyBrowser_.sort = static_cast<int>(emby::SortKey::Name);
        app.embyListArrived(opened + 6, 0, reply(movie("a", "Alpha") + "," + movie("b", "Bravo")));

        // Chosen from a list that says it was watched to 30 s, a video
        // resumes where the server says it was left: further on, watched
        // elsewhere since the list was fetched, or not at all.
        emby::Item stale;
        stale.id = "a";
        stale.name = "Alpha";
        stale.type = "Movie";
        stale.mediaType = "Video";
        stale.runTimeTicks = 6000000000;
        stale.positionTicks = 300000000;
        app.closePane(0);
        app.embyPlayItem(stale, 0);
        require(app.embyPanes_[0] && app.embyPanes_[0]->resolving && app.embyPanes_[0]->resumeTicks == 300000000,
                "Premise: the pane starts from what the list said");
        const std::uint64_t paneSerial = app.embyPanes_[0]->serial;
        EmbyClient::Response further;
        further.ok = true;
        further.status = 200;
        further.body = "{\"Name\":\"Alpha\",\"Id\":\"a\",\"Type\":\"Movie\",\"MediaType\":\"Video\","
                       "\"RunTimeTicks\":6000000000,\"UserData\":{\"PlaybackPositionTicks\":4200000000,\"Played\":false}}";
        app.embyItemArrived(paneSerial - 1, further);
        require(app.embyPanes_[0]->resumeTicks == 300000000, "An item asked for by an earlier pane was taken");
        app.embyItemArrived(paneSerial, further);
        require(app.embyPanes_[0] && app.embyPanes_[0]->resumeTicks == 4200000000,
                "The video resumes from the list's old position, not the server's");
        app.closePane(0);
        app.embyPlayItem(stale, 0);
        further.body = "{\"Name\":\"Alpha\",\"Id\":\"a\",\"Type\":\"Movie\",\"MediaType\":\"Video\","
                       "\"RunTimeTicks\":6000000000,\"UserData\":{\"PlaybackPositionTicks\":0,\"Played\":false}}";
        app.embyItemArrived(app.embyPanes_[0]->serial, further);
        require(app.embyPanes_[0] && app.embyPanes_[0]->resumeTicks == 0,
                "A video marked as not started still resumes from the list's old position");
        app.embyRefreshWantedTick_ = 0;
        pane.itemId = "b";
        pane.serial = ++app.embyPaneSerial_;
        bindEmbyAccount(app, pane);
        app.embyPanes_[0] = pane;
        app.paths_[0] = L"emby://srv/b";

        // The window coming back to the front, and a video reported
        // started or stopped, are news.
        require(app.embyRefreshWantedTick_ == 0, "Premise: nothing is wanted");
        app.handleMessage(WM_ACTIVATE, WA_INACTIVE, 0);
        require(app.embyRefreshWantedTick_ == 0, "Losing the front counted as news");
        app.handleMessage(WM_ACTIVATE, WA_ACTIVE, 0);
        require(app.embyRefreshWantedTick_ != 0, "Coming back to the front is not news");
        app.embyRefreshWantedTick_ = 0;
        app.embyPanes_[0]->mediaSourceId = "m";
        app.embyReport(0, "progress", "TimeUpdate");
        require(app.embyRefreshWantedTick_ == 0, "A progress report counted as news");
        app.embyReport(0, "playing");
        require(app.embyRefreshWantedTick_ != 0 && app.embyPanes_[0]->started, "A started video is not news");
        app.embyRefreshWantedTick_ = 0;
        app.embyReport(0, "stopped");
        require(app.embyRefreshWantedTick_ != 0, "A stopped video is not news");
        // The frame's tick is where it is asked for.
        app.embyListAskedTick_ = GetTickCount64() - 5'000;
        app.embyRefreshWantedTick_ = app.embyListAskedTick_;
        const std::uint64_t before = app.embyRequestSerial_;
        app.embyTick();
        require(app.embyRequestSerial_ == before + 1 && app.embyRefreshing_, "The frame's tick did not ask for the page again");
    }

    // A video the server has only just found is in the list the moment the
    // list is asked for again, and comes first in the order by what the
    // server found or changed last -- by the date it was added it would
    // stand among the files of its own date, years back.
    static void embyNewlyFoundVideosComeFirst() {
        App app;
        HiddenWindow parent;
        app.window_ = app.videoWindow_ = parent.window;
        app.instance_ = GetModuleHandleW(nullptr);
        emby::Session session;
        session.serverUrl = "http://127.0.0.1:9";
        session.serverId = "srv";
        session.userId = "u1";
        session.token = "token";
        session.deviceId = "dev";
        app.embyConfigure(session);
        app.settingsOpen_ = true;
        app.settingsAlpha_ = 1.0F;
        app.embyBrowserOpen_ = true;
        app.embyBrowser_.view = 0;
        app.embyPages_.push_back(App::EmbyPage{});
        emby::Item view;
        view.id = "114041";
        view.name = "Films";
        view.type = "CollectionFolder";
        view.collectionType = "movies";
        view.isFolder = true;
        app.embyItems_ = {view};
        app.embyActivateItem(0);
        const std::uint64_t opened = app.embyRequestSerial_;
        const auto reply = [](const std::string& items, int total) {
            EmbyClient::Response response;
            response.ok = true;
            response.status = 200;
            response.body = "{\"Items\":[" + items + "],\"TotalRecordCount\":" + std::to_string(total) + "}";
            return response;
        };
        const auto movie = [](const char* id, const char* name) {
            return std::string("{\"Name\":\"") + name + "\",\"Id\":\"" + id +
                   "\",\"Type\":\"Movie\",\"MediaType\":\"Video\",\"IsFolder\":false,"
                   "\"DateCreated\":\"2026-04-18T07:26:08.0000000Z\"}";
        };
        app.embyListArrived(opened, 0, reply(movie("114403", "Bravo") + "," + movie("119854", "Charlie"), 2));
        const RECT client{0, 0, 1280, 720};
        const auto sortRow = [&]() -> const PanelRow* {
            app.refreshPanelGeometry(client);
            for (const auto& row : app.panelRows_) if (row.id == SettingId::EmbySort) return &row;
            return nullptr;
        };
        const PanelRow* row = sortRow();
        require(row && row->options.size() == 8 && row->options[7] == L"Updated" && row->selected == 0 &&
                row->segmentsPerLine == 8, "The orders do not end with Updated, all on the wide sheet's line");

        // Chosen, it asks the server by when it last wrote each item,
        // the latest first, and says what it goes by.
        const int segment = 7;
        app.embyChooseSort(segment);
        require(app.embyBrowser_.sort == static_cast<int>(emby::SortKey::Updated) && app.embyBrowser_.descending &&
                app.embyRequestSerial_ == opened + 1 && app.embyLoading_, "Choosing Updated did not ask for the list by it");
        const std::string newestFirst = app.embyListPath(app.embyPages_.back(), 0, 300);
        require(newestFirst.find("SortBy=DateLastSaved&SortOrder=Descending") != std::string::npos &&
                newestFirst.find("IncludeItemTypes=Movie") != std::string::npos,
                "Updated is not asked for as the server's last-saved order");
        require(app.captureAppSettings().embyBrowser.sort == 7, "The order is not saved with the settings");
        // Two videos the server has only just found: first, though their
        // files are older than the rest.
        app.embyListArrived(opened + 1, 0, reply(movie("123200", "Zulu") + "," + movie("123199", "Yankee") + "," +
                                                 movie("119854", "Charlie") + "," + movie("114403", "Bravo"), 4));
        require(app.embyItems_.size() == 4 && app.embyItems_[0].id == "123200" && app.embyItems_[1].id == "123199",
                "What the server found last does not come first");
        row = sortRow();
        require(row && row->selected == 7 &&
                row->label.find(L"what the server found or changed last comes first") != std::wstring::npos,
                "The order does not say what it goes by");
        app.embyChooseSort(segment);
        require(!app.embyBrowser_.descending &&
                app.embyListPath(app.embyPages_.back(), 0, 300).find("SortBy=DateLastSaved&SortOrder=Ascending") !=
                    std::string::npos, "Choosing Updated again did not reverse it");
        row = sortRow();
        require(row && row->label.find(L"comes last") != std::wstring::npos, "Reversed, the order still says first");
        app.embyChooseSort(segment);
        app.embyListArrived(app.embyRequestSerial_, 0,
                            reply(movie("123200", "Zulu") + "," + movie("123199", "Yankee") + "," +
                                  movie("119854", "Charlie") + "," + movie("114403", "Bravo"), 4));

        // While the browser stays up the server finds another: asked for
        // again, the list has it, first, and the rest has not moved.
        const unsigned long long asked = app.embyListAskedTick_;
        app.embyRefreshWhenDue(asked + 30'000);
        require(app.embyRefreshing_ && app.embyItems_.size() == 4, "Half a minute did not ask for the list again");
        app.embyListArrived(app.embyRequestSerial_, 0,
                            reply(movie("123201", "Found just now") + "," + movie("123200", "Zulu") + "," +
                                  movie("123199", "Yankee") + "," + movie("119854", "Charlie") + "," +
                                  movie("114403", "Bravo"), 5));
        require(app.embyItems_.size() == 5 && app.embyTotal_ == 5 && app.embyItems_[0].id == "123201" &&
                app.embyItems_[4].id == "114403", "A video the server found since is not in the list");
        bool shown = false;
        app.refreshPanelGeometry(client);
        for (const auto& item : app.panelRows_) {
            shown = shown || (item.id == SettingId::EmbyItem && item.param == 0 && item.label == L"Found just now");
        }
        require(shown, "The video found since has no row");

        // The list being played goes by the same order at once, by the
        // numbers the server gave, which count up.
        const auto video = [](const char* id, const char* name) {
            emby::Item item;
            item.id = id;
            item.name = name;
            item.type = "Movie";
            item.mediaType = "Video";
            return item;
        };
        app.embyBrowser_.sort = static_cast<int>(emby::SortKey::Name);
        mutablePlaybackQueue(app).items = {video("119854", "Charlie"), video("123200", "Zulu"), video("9731", "Alpha")};
        mutablePlaybackQueue(app).cursor = 2;
        app.sortListBy(static_cast<int>(emby::SortKey::Updated));
        require(playbackQueue(app).items[0].id == "123200" && playbackQueue(app).items[1].id == "119854" &&
                playbackQueue(app).items[2].id == "9731" && playbackQueue(app).cursor == 2 && app.embyBrowser_.descending,
                "The list being played was not put in the order of what the server found last");

        // A library walked by folder keeps its folders first, the ones
        // the server put something in last before the others.
        App::EmbyPage home;
        home.kind = App::EmbyPage::Kind::Library;
        home.id = "47096";
        home.collectionType = "homevideos";
        require(app.embyListPath(home, 0, 300).find("SortBy=IsFolder%2CDateLastSaved&SortOrder=Ascending%2CDescending") !=
                    std::string::npos, "By folder, Updated does not keep the folders first");
        // A local folder has no such order and stays by name.
        require(localSortChoice(emby::SortKey::Updated) == -1, "A folder claims an order it does not have");
    }

    // F6 beside a playing video: the browser docks over the right quarter,
    // the video keeps the whole window under it and only the bar keeps to
    // the uncovered part; with nothing playing the browser is the whole
    // window. One video also gets the step buttons on the bar.
    static void embyBrowserDocksBesideAVideo() {
        App app;
        HiddenWindow parent;
        app.window_ = app.videoWindow_ = parent.window;
        app.instance_ = GetModuleHandleW(nullptr);
        emby::Session session;
        session.serverUrl = "http://127.0.0.1:9";
        session.serverId = "srv";
        session.userId = "u1";
        session.token = "token";
        session.deviceId = "dev";
        app.embyConfigure(session);
        const RECT client{0, 0, 1280, 720};
        // Nothing playing: the browser takes the window, no step buttons.
        app.settingsOpen_ = true;
        app.settingsAlpha_ = 1.0F;
        app.embyBrowserOpen_ = true;
        app.embyPages_.push_back(App::EmbyPage{});
        app.refreshChromeGeometry(client);
        require(app.panelLayout_.sheet.x == 0.0F && app.panelLayout_.sheet.width == 1280.0F,
                "An empty player does not give the browser the window");
        require(!app.barLayout_[BarItem::Previous].visible(), "Step buttons without a video");
        // One video: docked at the right half; the player is the left half.
        app.paths_[0] = L"emby://srv/b";
        app.sources_[0] = std::make_unique<VideoSource>();
        app.refreshChromeGeometry(client);
        require(app.embyBrowserDocked(), "The browser did not dock beside the video");
        require(app.panelLayout_.sheet.x == 960.0F && app.panelLayout_.sheet.width == 320.0F,
                "The docked browser is not the right quarter");
        require(app.chromeCells_[0].width == 1280.0F && app.chromeCells_[0].x == 0.0F,
                "The video was shrunk instead of covered");
        require(app.barLayout_.row.width == 960.0F, "The bar does not keep to the uncovered part");
        require(app.barLayout_[BarItem::Previous].visible() && app.barLayout_[BarItem::Next].visible(),
                "One video has no step buttons");
        require(app.videoAreaWidth(1280.0F) == 960.0F, "Wrong uncovered width");
        // The video beside the sheet is still the player, not "outside".
        require(app.panelHitAt(POINT{100, 100}).kind == PanelHitKind::Outside, "Premise");
        // Closed, the video takes the window again; F6 reopens it docked.
        app.toggleEmbyBrowser();
        require(!app.settingsOpen_ && !app.embyBrowserOpen_, "F6 did not close the browser");
        app.refreshChromeGeometry(client);
        require(app.barLayout_.row.width == 1280.0F && app.videoAreaWidth(1280.0F) == 1280.0F,
                "Closing the browser did not give the bar the window back");
        app.toggleEmbyBrowser();
        require(app.settingsOpen_ && app.embyBrowserOpen_ && app.embyBrowserDocked(), "F6 did not reopen the browser");
        // Two videos: still docked, but no step buttons.
        app.paths_[1] = L"C:\\Videos\\two.mp4";
        app.sources_[1] = std::make_unique<VideoSource>();
        app.refreshChromeGeometry(client);
        require(app.singleLoadedPane() < 0 && !app.barLayout_[BarItem::Previous].visible(),
                "Two videos still offer the step buttons");
        require(app.embyBrowserDocked() && app.panelLayout_.sheet.x == 960.0F, "Two videos undocked the browser");
        // A narrow window keeps the sheet usable; the bar takes the rest.
        const RECT narrow{0, 0, 600, 400};
        app.refreshChromeGeometry(narrow);
        require(app.panelLayout_.sheet.width == 320.0F && app.videoAreaWidth(600.0F) == 280.0F &&
                app.barLayout_.row.width == 280.0F && app.chromeCells_[0].width == 300.0F,
                "A narrow window does not keep a usable sheet");
    }

    // The sheet scrolls by dragging: a press on a row that moves becomes a
    // scroll and is not a click; the scrollbar's thumb drags too.
    static void sheetScrollsByDragging() {
        App app;
        HiddenWindow parent;
        require(SetWindowPos(parent.window, nullptr, 0, 0, 400, 720,
                             SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOZORDER) != FALSE,
                "Cannot size the hidden scroll fixture window");
        app.window_ = app.videoWindow_ = parent.window;
        app.instance_ = GetModuleHandleW(nullptr);
        emby::Session session;
        session.serverUrl = "http://127.0.0.1:9";
        session.serverId = "srv";
        session.userId = "u1";
        session.token = "token";
        session.deviceId = "dev";
        app.embyConfigure(session);
        app.settingsOpen_ = true;
        app.settingsAlpha_ = 1.0F;
        app.embyBrowserOpen_ = true;
        app.embyPages_.push_back(App::EmbyPage{});
        app.embyBrowser_.view = 0;
        for (int i = 0; i < 40; ++i) {
            emby::Item item;
            item.id = std::to_string(i);
            item.name = "Clip " + std::to_string(i);
            item.type = "Video";
            item.mediaType = "Video";
            app.embyItems_.push_back(item);
        }
        const RECT client{0, 0, 400, 720};
        app.refreshPanelGeometry(client);
        require(app.panelLayout_.maxScroll > 100.0F, "Premise: the list is taller than the sheet");
        int firstItemRow = -1;
        for (std::size_t i = 0; i < app.panelRows_.size(); ++i) {
            if (app.panelRows_[i].id == SettingId::EmbyItem && app.panelLayout_.rows[i].row.y > app.panelLayout_.content.y) {
                firstItemRow = static_cast<int>(i);
                break;
            }
        }
        require(firstItemRow >= 0, "Premise: an item row is on screen");
        const auto& box = app.panelLayout_.rows[static_cast<std::size_t>(firstItemRow)].row;
        const LONG x = static_cast<LONG>(box.x + 10.0F);
        const LONG y = static_cast<LONG>(box.y + 5.0F);
        require(app.panelLayout_.content.contains(static_cast<float>(x), static_cast<float>(y)),
                "The drag fixture's item point is outside the visible content");
        const int threshold = GetSystemMetrics(SM_CYDRAG);
        app.handleVideoMessage(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
        require(app.pressedPanelKind_ == PanelHitKind::Item && app.panelPressArmed_, "The press was not taken");
        // A wobble under the drag threshold is still a click.
        app.handleVideoMessage(WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x, y + 1));
        require(app.controlDrag_ == App::ControlDrag::None && app.pressedPanelKind_ == PanelHitKind::Item,
                "A small move ended the press");
        app.handleVideoMessage(WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x, y - threshold - 60));
        require(app.controlDrag_ == App::ControlDrag::PanelScroll && app.pressedPanelKind_ == PanelHitKind::None,
                "A drag did not become a scroll");
        require(std::abs(app.settingsScroll_ - static_cast<float>(threshold + 60)) < 0.01F,
                "The content did not follow the pointer");
        app.handleVideoMessage(WM_LBUTTONUP, 0, MAKELPARAM(x, y - threshold - 60));
        require(app.controlDrag_ == App::ControlDrag::None && !app.panelPressArmed_, "The release did not end the drag");
        require(app.paths_[0].empty() && app.embyPages_.size() == 1, "The drag was taken as a click");
        require(std::abs(app.settingsScroll_ - static_cast<float>(threshold + 60)) < 0.01F, "The release lost the scroll");
        // The thumb: grabbed and dragged down, the scroll grows; the track
        // beside it jumps the thumb under the pointer.
        app.refreshPanelGeometry(client);
        const OverlayRect thumb = panelScrollThumb(app.panelLayout_, app.uiScale_);
        require(thumb.visible(), "Premise: the scrollbar shows");
        const LONG tx = static_cast<LONG>(thumb.x + 2.0F);
        const LONG ty = static_cast<LONG>(thumb.y + 2.0F);
        const float before = app.settingsScroll_;
        app.handleVideoMessage(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(tx, ty));
        require(app.controlDrag_ == App::ControlDrag::PanelScrollbar, "The thumb was not grabbed");
        app.handleVideoMessage(WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(tx, ty + 30));
        require(app.settingsScroll_ > before, "Dragging the thumb down did not scroll down");
        app.handleVideoMessage(WM_LBUTTONUP, 0, MAKELPARAM(tx, ty + 30));
        require(app.controlDrag_ == App::ControlDrag::None, "The thumb drag did not end");
        app.refreshPanelGeometry(client);
        const LONG bottom = static_cast<LONG>(app.panelLayout_.scrollbar.bottom() - 2.0F);
        app.handleVideoMessage(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(tx, bottom));
        app.handleVideoMessage(WM_LBUTTONUP, 0, MAKELPARAM(tx, bottom));
        require(std::abs(app.settingsScroll_ - app.panelLayout_.maxScroll) < 1.0F,
                "A click at the end of the track did not scroll to the end");
        // A press on the empty sheet drags too, and a plain release there
        // does nothing.
        app.refreshPanelGeometry(client);
        const LONG sx = static_cast<LONG>(app.panelLayout_.sheet.x + 4.0F);
        const LONG sy = static_cast<LONG>(app.panelLayout_.content.y + 4.0F);
        require(app.panelHitAt(POINT{sx, sy}).kind == PanelHitKind::Sheet, "Premise: the sheet's edge is empty");
        const float atEnd = app.settingsScroll_;
        app.handleVideoMessage(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(sx, sy));
        app.handleVideoMessage(WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(sx, sy + threshold + 50));
        require(app.settingsScroll_ < atEnd, "Dragging the empty sheet down did not scroll up");
        app.handleVideoMessage(WM_LBUTTONUP, 0, MAKELPARAM(sx, sy + threshold + 50));
        require(app.controlDrag_ == App::ControlDrag::None && app.settingsOpen_, "The sheet drag closed the sheet");
    }

    // While an Emby item plays, the pointer at the window's right edge
    // brings the browser up docked; it goes once the pointer has been away
    // from it for a moment. A browser opened by F6 stays.
    static void rightEdgeOpensTheEmbyBrowser() {
        App app;
        HiddenWindow parent;
        app.window_ = app.videoWindow_ = parent.window;
        app.instance_ = GetModuleHandleW(nullptr);
        emby::Session session;
        session.serverUrl = "http://127.0.0.1:9";
        session.serverId = "srv";
        session.userId = "u1";
        session.token = "token";
        session.deviceId = "dev";
        app.embyConfigure(session);
        app.embyPages_.push_back(App::EmbyPage{});
        // The sheet's slide is the frame loop's; here it is always fully in.
        app.settingsAlpha_ = 1.0F;
        // The only video a local file: the edge brings up its folder, and it
        // goes again once the pointer has left it.
        app.paths_[0] = L"C:\\QuadDeck-no-such-folder\\one.mp4";
        app.sources_[0] = std::make_unique<VideoSource>();
        app.updateHoverControls(parent.screenPoint(399, 100));
        require(app.settingsOpen_ && app.embyBrowserOpen_ && app.embyBrowserByEdge_ &&
                    app.browserSource_ == App::BrowserSource::Local,
                "The edge did not open the folder of a local video");
        app.updateHoverControls(parent.screenPoint(20, 150));
        app.embyEdgeLeftSince_ = GetTickCount64() - 2000;
        app.updateHoverControls(parent.screenPoint(20, 150));
        require(!app.settingsOpen_ && !app.embyBrowserOpen_, "The folder list did not close after the pointer left it");
        // An Emby item: the edge opens the library, docked.
        app.paths_[0] = L"emby://srv/42";
        app.updateHoverControls(parent.screenPoint(200, 100));
        require(!app.settingsOpen_, "The middle of the window opened the browser");
        // The top-right corner is the caption's Close, not the edge.
        app.updateHoverControls(parent.screenPoint(399, 10));
        require(!app.settingsOpen_, "Heading for Close opened the browser");
        app.updateHoverControls(parent.screenPoint(200, 100));
        app.updateHoverControls(parent.screenPoint(399, 100));
        require(app.settingsOpen_ && app.embyBrowserOpen_ && app.embyBrowserByEdge_ && app.embyBrowserDocked() &&
                    app.browserSource_ == App::BrowserSource::Emby,
                "The right edge did not open the docked browser");
        // Over the sheet it stays; away from it, it goes after the delay.
        app.updateHoverControls(parent.screenPoint(390, 150));
        require(app.settingsOpen_ && app.embyEdgeLeftSince_ == 0, "Hovering the sheet started the close timer");
        // From the sheet to the transport bar beside it: closing now would
        // stretch the bar under the pointer, so it stays while the bar is up.
        app.updateHoverControls(parent.screenPoint(20, 290));
        require(app.settingsOpen_ && app.embyEdgeLeftSince_ == 0, "Moving onto the bar started the close timer");
        app.embyEdgeLeftSince_ = GetTickCount64() - 2000;
        app.updateHoverControls(parent.screenPoint(20, 290));
        require(app.settingsOpen_ && app.embyEdgeLeftSince_ == 0, "The browser closed under the pointer on the bar");
        // With the bar hidden, the same place is the picture again.
        app.dockProgress_ = 0.0F;
        app.updateHoverControls(parent.screenPoint(20, 290));
        require(app.settingsOpen_ && app.embyEdgeLeftSince_ != 0, "A hidden bar held the browser open");
        app.dockProgress_ = 1.0F;
        app.updateHoverControls(parent.screenPoint(390, 150));
        app.updateHoverControls(parent.screenPoint(20, 150));
        require(app.settingsOpen_ && app.embyEdgeLeftSince_ != 0, "Leaving the sheet did not start the close timer");
        app.embyEdgeLeftSince_ = GetTickCount64() - 2000;
        app.updateHoverControls(parent.screenPoint(20, 150));
        require(!app.settingsOpen_ && !app.embyBrowserOpen_ && !app.embyBrowserByEdge_,
                "The browser did not close after the pointer left it");
        // Back at the edge it opens again (the pointer was away in between).
        app.updateHoverControls(parent.screenPoint(399, 100));
        require(app.settingsOpen_ && app.embyBrowserByEdge_, "The edge did not open the browser a second time");
        // Opened by F6 instead, it is not the edge's to close.
        app.toggleEmbyBrowser();
        require(!app.settingsOpen_, "Premise: closed");
        app.updateHoverControls(parent.screenPoint(20, 150));
        app.toggleEmbyBrowser();
        require(app.settingsOpen_ && !app.embyBrowserByEdge_, "F6's browser counts as the edge's");
        app.updateHoverControls(parent.screenPoint(20, 150));
        app.embyEdgeLeftSince_ = GetTickCount64() - 2000;
        app.updateHoverControls(parent.screenPoint(20, 150));
        require(app.settingsOpen_, "F6's browser closed itself");
    }

    // Playing one local file loads its folder into the F6 list: listed off
    // the window thread, docked beside the video, the playing file marked,
    // re-ordered by the sort keys, walked by the step keys in that order,
    // and a row chosen plays in the same pane.
    static void localFolderIsTheF6List() {
        const auto directory = std::filesystem::temp_directory_path() / L"QuadDeckFolderListTest";
        std::error_code code;
        std::filesystem::remove_all(directory, code);
        std::filesystem::create_directories(directory, code);
        require(!code, "Cannot create the fixture directory");
        const auto make = [&](const wchar_t* name, std::size_t bytes) {
            std::ofstream file(directory / name, std::ios::binary);
            file << std::string(bytes, 'x');
        };
        make(L"b (2).mp4", 30);
        make(L"a (10).mp4", 10);
        make(L"c.mkv", 20);
        make(L"notes.txt", 5);
        // Subtitles: one of c's, and one whose name is not a (10)'s.
        make(L"c.chs.ass", 5);
        make(L"a (1).srt", 5);
        // With a real video named in the environment, its length is read too.
        wchar_t sample[MAX_PATH]{};
        const bool haveSample = GetEnvironmentVariableW(L"QUADDECK_TEST_VIDEO", sample, MAX_PATH) > 0 &&
                                std::filesystem::exists(sample);
        App app;
        HiddenWindow parent;
        app.window_ = app.videoWindow_ = parent.window;
        app.instance_ = GetModuleHandleW(nullptr);
        app.settingsAlpha_ = 1.0F;
        const std::wstring current = (directory / L"b (2).mp4").wstring();
        app.paths_[0] = current;
        app.sources_[0] = std::make_unique<VideoSource>();
        app.refreshLocalList(current);
        require(app.localListLoading_ && app.localList_.empty(), "The folder must not be read on the calling thread");
        for (int i = 0; i < 500 && app.localListLoading_; ++i) {
            app.mainQueue_->drain();
            Sleep(10);
        }
        const auto names = [&] {
            std::wstring joined;
            for (const auto& entry : app.localList_) joined += localFileName(entry.path) + L"|";
            return joined;
        };
        require(!app.localListLoading_ && names() == L"a (10).mp4|b (2).mp4|c.mkv|",
                "The folder's videos did not arrive in name order");
        // Asking again for the same folder does not read it again.
        const auto serial = app.localListSerial_;
        app.refreshLocalList((directory / L"c.mkv").wstring());
        require(app.localListSerial_ == serial && !app.localListLoading_, "The same folder was read twice");
        // The lengths follow, off the window thread too: what is not a video
        // has none, and a length that arrives is shown beside the size.
        for (int i = 0; i < 500 && app.localProbes_.size() < 3; ++i) {
            app.mainQueue_->drain();
            Sleep(10);
        }
        require(app.localProbes_.size() == 3, "The lengths were never read");
        for (const auto& entry : app.localList_) {
            require(entry.duration == 0.0 && !entry.subtitleStream, "A file of x's has a length or a subtitle stream");
            require(entry.subtitleFile == (localFileName(entry.path) == L"c.mkv"),
                    "The subtitle files beside were not matched to their videos");
        }
        // A subtitle stream found inside arrives with the length.
        app.applyLocalProbes(app.localListSerial_, app.localDirectory_,
                             {{(directory / L"a (10).mp4").wstring(), {95.0, true}},
                              {(directory / L"c.mkv").wstring(), {30.0}}});
        require(app.localList_[0].duration == 95.0 && app.localList_[2].duration == 30.0,
                "Lengths that arrived were not taken");
        require(app.localList_[0].subtitleStream && !app.localList_[2].subtitleStream,
                "A subtitle stream that arrived was not taken");
        app.applyLocalProbes(app.localListSerial_ + 1, app.localDirectory_,
                             {{(directory / L"b (2).mp4").wstring(), {7.0, true}}});
        require(app.localList_[1].duration == 0.0 && !app.localList_[1].subtitleStream,
                "What a superseded scan read was taken");
        // Read again, the folder keeps what its files were found to hold
        // without opening them again.
        app.refreshLocalList(current, true);
        for (int i = 0; i < 500 && app.localListLoading_; ++i) {
            app.mainQueue_->drain();
            Sleep(10);
        }
        require(!app.localListLoading_ && app.localList_.size() == 3 && app.localList_[0].duration == 95.0 &&
                    app.localList_[0].subtitleStream && app.localList_[2].subtitleFile,
                "A refresh forgot what the folder's files hold");
        // F6 with the only video a local file: its folder, docked.
        app.toggleEmbyBrowser();
        require(app.settingsOpen_ && app.embyBrowserOpen_ && app.browserSource_ == App::BrowserSource::Local &&
                    app.embyBrowserDocked(),
                "F6 did not open the folder list beside the video");
        const RECT client{0, 0, 1280, 720};
        app.embyBrowser_.view = 0;
        app.embyBrowser_.sort = 0;
        app.embyBrowser_.descending = false;
        app.orderLocalList();
        app.refreshPanelGeometry(client);
        int items = 0;
        int marked = 0;
        for (const auto& row : app.panelRows_) {
            if (row.id != SettingId::LocalItem) continue;
            ++items;
            if (row.param == 0) require(row.value == L"1:35   10 B", "A length read is not shown beside the size");
            // A file beside or a stream inside: marked either way.
            require(row.tag == (row.label == L"b (2).mp4" ? L"" : L"Sub"), "A video's subtitles are not marked");
            if (row.current) {
                ++marked;
                require(row.label == L"b (2).mp4" && row.value == L"30 B" && row.param == 1,
                        "The wrong row is marked as playing");
            }
        }
        require(items == 3 && marked == 1, "The list does not show the folder with the playing file marked");
        // By length: shortest first, the file whose length is not known last;
        // again, longest first, and it is still last.
        app.sortListBy(static_cast<int>(emby::SortKey::Runtime));
        require(app.embyBrowser_.descending && names() == L"a (10).mp4|c.mkv|b (2).mp4|",
                "Length did not order the folder longest first");
        app.sortListBy(static_cast<int>(emby::SortKey::Runtime));
        require(names() == L"c.mkv|a (10).mp4|b (2).mp4|", "Length again did not reverse the folder");
        // A length arriving while sorted by length puts the file in its place.
        app.applyLocalProbes(app.localListSerial_, app.localDirectory_,
                             {{(directory / L"b (2).mp4").wstring(), {60.0}}});
        require(names() == L"c.mkv|b (2).mp4|a (10).mp4|", "A length that arrived did not re-order the list");
        app.applyLocalProbes(app.localListSerial_, app.localDirectory_,
                             {{(directory / L"b (2).mp4").wstring(), {0.0}}});
        // The list keeps its place: closed and opened again it is where it
        // was left, not at its top; so do the settings, separately.
        app.settingsScroll_ = 37.0F;
        app.toggleEmbyBrowser();
        require(!app.settingsOpen_, "Premise: F6 closed the list");
        app.toggleSettingsPanel();
        require(app.settingsOpen_ && !app.embyBrowserOpen_ && app.settingsScroll_ == 0.0F,
                "The settings opened at the list's place");
        app.settingsScroll_ = 11.0F;
        app.toggleEmbyBrowser();
        require(app.embyBrowserOpen_ && app.browserSource_ == App::BrowserSource::Local &&
                    app.settingsScroll_ == 37.0F,
                "The folder list did not come back where it was left");
        app.toggleSettingsPanel();
        require(!app.embyBrowserOpen_ && app.settingsScroll_ == 11.0F, "The settings lost their place");
        app.toggleEmbyBrowser();
        require(app.settingsScroll_ == 37.0F, "The folder list lost its place to the settings");
        app.settingsScroll_ = 0.0F;
        // The sort keys re-order the list, and the step keys walk that order.
        app.sortListBy(static_cast<int>(emby::SortKey::Size));
        require(names() == L"b (2).mp4|c.mkv|a (10).mp4|", "Size did not order the folder largest first");
        int position = -1;
        auto files = app.siblingFiles(current, position);
        require(files.size() == 3 && position == 0 && localFileName(files[1]) == L"c.mkv",
                "The step keys do not walk the order shown");
        app.sortListBy(static_cast<int>(emby::SortKey::Size));
        require(names() == L"a (10).mp4|c.mkv|b (2).mp4|", "Size again did not reverse the folder");
        // A row chosen plays in the same pane; the docked sheet stays.
        app.deviceRecoveryPending_ = true;   // a path, no decoder: there is no device here
        app.activateLocalItem(1);
        require(localFileName(app.paths_[0]) == L"c.mkv" && app.paths_[1].empty(), "The chosen file did not take the pane");
        require(app.settingsOpen_ && app.embyBrowserOpen_, "Choosing from the docked list closed it");
        app.activateLocalItem(7);
        require(localFileName(app.paths_[0]) == L"c.mkv", "A row that is not there did something");
        // The grid: one tile per file, its picture keyed by path, the playing one marked.
        app.embyBrowser_.view = 2;
        app.refreshPanelGeometry(client);
        int tiles = 0;
        int markedTiles = 0;
        for (const auto& row : app.panelRows_) {
            if (row.kind != PanelRowKind::Tiles) continue;
            require(row.id == SettingId::LocalItem, "A local tile row has the wrong identity");
            for (std::size_t part = 0; part < row.options.size(); ++part) {
                ++tiles;
                const auto key = parseLocalThumbnailKey(row.tileKeys[part]);
                require(key && localFileName(key->path) == row.options[part], "A tile's picture is not keyed by its file");
                require(row.tileTags.size() == row.options.size() &&
                            row.tileTags[part] == (row.options[part] == L"b (2).mp4" ? L"" : L"Sub"),
                        "A tile does not mark the video's subtitles");
            }
            if (row.selected >= 0) {
                ++markedTiles;
                require(row.options[static_cast<std::size_t>(row.selected)] == L"c.mkv", "The wrong tile is marked");
            }
        }
        require(tiles == 3 && markedTiles == 1, "The grid does not show the folder with the playing file marked");
        // F6 again closes it. With an Emby item as the only video, F6 is the library.
        app.toggleEmbyBrowser();
        require(!app.settingsOpen_ && !app.embyBrowserOpen_, "F6 did not close the folder list");
        emby::Session session;
        session.serverUrl = "http://127.0.0.1:9";
        session.serverId = "srv";
        session.userId = "u1";
        session.token = "token";
        session.deviceId = "dev";
        app.embyConfigure(session);
        app.paths_[0] = L"emby://srv/42";
        app.toggleEmbyBrowser();
        require(app.settingsOpen_ && app.browserSource_ == App::BrowserSource::Emby, "F6 on an Emby item is not the library");
        // The Emby browser keeps its place too: reopened with a page
        // shown, the page stays and is asked for again in place.
        App::EmbyPage folderPage;
        folderPage.kind = App::EmbyPage::Kind::Folder;
        folderPage.id = "7";
        app.embyPages_.push_back(folderPage);
        emby::Item clip;
        clip.id = "42";
        clip.name = "Clip";
        clip.type = "Video";
        clip.mediaType = "Video";
        app.embyItems_ = {clip};
        app.embyLoading_ = false;
        app.settingsScroll_ = 53.0F;
        app.toggleEmbyBrowser();
        require(!app.settingsOpen_, "Premise: F6 closed the browser");
        app.toggleEmbyBrowser();
        require(app.settingsOpen_ && app.settingsScroll_ == 53.0F && app.embyItems_.size() == 1 && !app.embyLoading_,
                "The Emby browser emptied itself or lost its place on reopening");
        // Into a folder and back: the page left is returned to where it was
        // once it has arrived.
        App::EmbyPage inner;
        inner.kind = App::EmbyPage::Kind::Folder;
        inner.id = "8";
        app.embyPushPage(inner);
        require(app.embyPages_[app.embyPages_.size() - 2].scroll == 53.0F && app.settingsScroll_ == 0.0F,
                "Opening a folder did not note where its parent was left");
        app.embyNavigate(0);
        require(app.embyPages_.back().id == "7" && app.embyPendingScroll_ == 53.0F,
                "Back does not know where to return to");
        // The folder's own list again, after another folder: from its top.
        std::filesystem::remove_all(directory, code);
        if (haveSample) {
            std::filesystem::create_directories(directory, code);
            std::filesystem::copy_file(sample, directory / L"real.mp4",
                                       std::filesystem::copy_options::overwrite_existing, code);
            require(!code, "Cannot copy the sample video");
            app.paths_[0] = (directory / L"real.mp4").wstring();
            app.refreshLocalList(app.paths_[0], true);
            for (int i = 0; i < 1000 && (app.localListLoading_ || app.localProbes_.empty()); ++i) {
                app.mainQueue_->drain();
                Sleep(10);
            }
            require(app.localList_.size() == 1 && app.localList_[0].duration > 1.0,
                    "The sample video's length was not read");
            std::cout << "sample video length read: " << app.localList_[0].duration << " s\n";
            std::filesystem::remove_all(directory, code);
        }
    }

    // Reading a file for the list finds a text subtitle stream inside it,
    // which the player would show, and not one of pictures, which it does
    // not.
    static void localProbeFindsTextSubtitleStreams() {
        const auto directory = std::filesystem::temp_directory_path() / L"QuadDeckLocalProbeTest";
        std::error_code code;
        std::filesystem::remove_all(directory, code);
        std::filesystem::create_directories(directory, code);
        require(!code, "Cannot create the probe fixture directory");
        writeSubtitleOnlyMatroska(directory / L"text.mkv", AV_CODEC_ID_SUBRIP);
        writeSubtitleOnlyMatroska(directory / L"pictures.mkv", AV_CODEC_ID_HDMV_PGS_SUBTITLE);
        std::atomic<std::uint64_t> token{1};
        const LocalProbe text = App::probeLocalFile((directory / L"text.mkv").wstring(), token, 1);
        require(text.subtitleStream && text.duration > 0.5, "A text subtitle stream inside was not found");
        const LocalProbe pictures = App::probeLocalFile((directory / L"pictures.mkv").wstring(), token, 1);
        require(!pictures.subtitleStream, "A stream of subtitle pictures counted as subtitles to show");
        const LocalProbe missing = App::probeLocalFile((directory / L"missing.mkv").wstring(), token, 1);
        require(!missing.subtitleStream && missing.duration == 0.0, "A file that is not there was read");
        std::filesystem::remove_all(directory, code);
    }

    // The shell's thumbnails are asked for off the calling thread and
    // answered on it; what was not started can be dropped.
    static void localThumbnailsAnswerOnTheCallingThread() {
        LocalThumbnailer thumbnails;
        int answered = 0;
        int cancelled = 0;
        const auto handler = [&](const LocalThumbnailer::Result& result) {
            if (result.cancelled) ++cancelled;
            else {
                ++answered;
                require(result.bytes.empty(), "A file that is not there has a picture");
            }
        };
        thumbnails.request(L"C:\\QuadDeck-no-such-folder\\none.mp4", 320, 180, handler);
        require(answered == 0, "The answer must not arrive on the asking call");
        for (int i = 0; i < 500 && answered == 0; ++i) {
            thumbnails.pump();
            Sleep(10);
        }
        require(answered == 1 && thumbnails.pendingCount() == 0, "The request was not answered");
        for (int i = 0; i < 6; ++i) thumbnails.request(L"C:\\QuadDeck-no-such-folder\\none.mp4", 320, 180, handler);
        thumbnails.clearPending();
        for (int i = 0; i < 500 && answered + cancelled < 7; ++i) {
            thumbnails.pump();
            Sleep(10);
        }
        require(answered + cancelled == 7, "A dropped request never heard of it");
        // With a real video named in the environment, the shell's picture
        // comes back as a BMP the overlay's decoder reads.
        wchar_t sample[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"QUADDECK_TEST_VIDEO", sample, MAX_PATH) > 0 &&
            std::filesystem::exists(sample)) {
            std::string bytes;
            bool done = false;
            thumbnails.request(sample, 440, 248, [&](const LocalThumbnailer::Result& result) {
                bytes = result.bytes;
                done = true;
            });
            for (int i = 0; i < 3000 && !done; ++i) {
                thumbnails.pump();
                Sleep(10);
            }
            require(done && bytes.size() > 54 && bytes[0] == 'B' && bytes[1] == 'M',
                    "The shell gave no picture for the sample video");
            std::cout << "shell thumbnail for the sample video: " << bytes.size() << " bytes\n";
        }
    }

    // One video is the timeline: it follows the master clock whatever
    // the saved seek mode, so the only bar it has moves with it, and its
    // repeat is "When the only video ends" rather than a wrap under a
    // clock that runs on. Several videos get the saved mode back.
    static void singleVideoFollowsTheMasterTimeline() {
        App app;
        HiddenWindow parent;
        app.window_ = app.videoWindow_ = parent.window;
        app.instance_ = GetModuleHandleW(nullptr);
        app.seekMode_ = SeekMode::Independent;
        app.sourceAutoRepeat_.fill(true);
        require(app.activeSeekMode() == SeekMode::Independent, "With no video the saved mode is not in force");
        app.paths_[0] = L"C:\\Videos\\one.mp4";
        app.sources_[0] = std::make_unique<VideoSource>();
        require(app.singleLoadedPane() == 0 && app.activeSeekMode() == SeekMode::Linked,
                "One video does not follow the master timeline");
        require(app.seekMode_ == SeekMode::Independent, "The saved choice was changed");
        // A repeating video no longer wraps under the clock: past its end
        // the mapping runs on, which is what ends it and restarts the clock.
        {
            PaneTiming timing;
            timing.loaded = timing.ready = true;
            timing.duration = 10.0;
            timing.autoRepeat = true;
            require(mappedPaneTime(timing, 25.0, SeekMode::Independent) == 5.0, "Premise: Independent wraps");
            require(mappedPaneTime(timing, 25.0, app.activeSeekMode()) == 25.0, "One video still wraps under the clock");
            require(!paneRepeats(timing, app.activeSeekMode()), "One video's flag still counts as a repeat");
        }
        // The arrow keys move the master clock, and with it the bar.
        app.duration_ = 100.0;
        app.clock_.seek(10.0);
        app.seekRelative(5.0);
        require(std::abs(app.clock_.position() - 15.0) < 0.001, "The arrow key did not move the master timeline");
        // The repeat chip is the rule for the only video.
        app.playOrder_ = PlayOrder::InOrder;
        app.activateChip(0, PaneChip::Repeat);
        require(app.playOrder_ == PlayOrder::RepeatOne && app.sourceAutoRepeat_[0],
                "The chip did not set what happens when the only video ends");
        app.activateChip(0, PaneChip::Repeat);
        require(app.playOrder_ == PlayOrder::InOrder, "The chip did not switch the repeat off again");
        // Two videos: the saved mode and the videos' own flags again.
        app.paths_[1] = L"C:\\Videos\\two.mp4";
        app.sources_[1] = std::make_unique<VideoSource>();
        require(app.singleLoadedPane() < 0 && app.activeSeekMode() == SeekMode::Independent,
                "Several videos lost the saved seek mode");
        app.activateChip(1, PaneChip::Repeat);
        require(!app.sourceAutoRepeat_[1] && app.playOrder_ == PlayOrder::InOrder,
                "Among several the chip is not the video's own flag");
        // Nothing here may reach the settings file.
        KillTimer(parent.window, app_internal::kSettingsSaveTimer);
        app.settingsSavePending_ = false;
    }

    // The only video opened from F6 (or Emby) ends the same way as the only
    // manual one: its chip shows "When the only video ends", so a saved
    // per-pane repeat flag must not loop it behind the chip's back.
    static void singleBrowserVideoEndsByTheOnlyVideoRule() {
        App app;
        HiddenWindow parent;
        const auto queue = syntheticLocalQueue();
        prepareLocalBrowser(app, parent, queue);
        app.sourceAutoRepeat_.fill(true);
        app.playOrder_ = PlayOrder::InOrder;
        seedLiveLocalPane(app, 2, queue);
        require(app.perPaneTimelines() && app.singleLoadedPane() == 2 &&
                app.activeSeekMode() == SeekMode::Independent, "Premise: one F6 pane on its own timeline");
        require(!app.paneTiming(2).autoRepeat && !paneRepeats(app.paneTiming(2), app.activeSeekMode()),
                "The only F6 video looped by its hidden per-pane flag");
        app.activateChip(2, PaneChip::Repeat);
        require(app.playOrder_ == PlayOrder::RepeatOne && app.sourceAutoRepeat_[2],
                "The only F6 video's chip did not set the only-video rule");
        app.playOrder_ = PlayOrder::InOrder;
        // A second pane brings the per-pane flags back.
        seedLiveLocalPane(app, 4, queue, 1);
        require(app.singleLoadedPane() < 0 && app.paneTiming(2).autoRepeat &&
                paneRepeats(app.paneTiming(2), app.activeSeekMode()),
                "Among several F6 videos the per-pane flag is not in force");
        KillTimer(parent.window, app_internal::kSettingsSaveTimer);
        app.settingsSavePending_ = false;
    }

    // The next video shows none of the time the one before it reached. An F6
    // or Emby video starts at its own zero under a master clock that keeps
    // counting, so the bar and the title show the video's own time; a file
    // that replaces the only manual video starts the timeline from zero.
    static void onlyVideoShowsNoTimeFromTheOneBefore() {
        HiddenWindow parent;
        const auto queue = syntheticLocalQueue();
        const auto titleText = [&] {
            wchar_t text[512]{};
            GetWindowTextW(parent.window, text, 512);
            return std::wstring(text);
        };
        {
            // Explorer opened the first file at 0:00; at 0:30 F6 played the
            // second, which has been playing for 70 s.
            App app;
            prepareLocalBrowser(app, parent, queue);
            seedLiveLocalPane(app, 0, queue);
            app.clock_.seek(100.0);
            app.clock_.pause();
            app.deviceRecoveryResume_ = false;
            app.syncAdjustments_[0] = -30.0;
            auto shown = app.barTime();
            require(shown.pane == 0 && shown.position == 70.0 && shown.duration == 120.0,
                    "The only F6 video's bar showed the master clock instead of its own time");
            app.localPlayItem(queue[1].path, queue, 0);
            shown = app.barTime();
            require(shown.pane == 0 && shown.position == 0.0 && shown.duration == 0.0,
                    "An F6 video still opening showed the master clock");
            app.sources_[0] = std::make_unique<VideoSource>();
            AudioSeekTestAccess::seed(*app.sources_[0], queue[1].duration);
            shown = app.barTime();
            require(app.clock_.position() == 100.0 && app.currentSourceTime(0) == 0.0 &&
                    shown.position == 0.0 && shown.duration == queue[1].duration,
                    "The next F6 video's bar carried the time the previous one had reached");
            app.updateTitle();
            require(titleText().find(L"  0:00 / 4:00") != std::wstring::npos,
                    "The title carried the time the previous F6 video had reached");
            app.uiScale_ = 1.0F;
            const RECT client{0, 0, 1280, 720};
            app.refreshChromeGeometry(client);
            app.refreshPanelGeometry(client);
            app.buildOverlayScene();
            require(app.scene_.timeText == L"0:00 / 4:00" && app.scene_.seekable && app.scene_.seek01 == 0.0F,
                    "The drawn bar carried the time the previous F6 video had reached");
            // The bar is that video's own rail: half way is 2:00 of 4:00.
            app.dragFraction_ = 0.5F;
            app.controlDrag_ = App::ControlDrag::MasterSeek;
            app.endControlDrag(true);
            require(std::abs(app.currentSourceTime(0) - 120.0) < 0.001 &&
                    std::abs(app.barTime().position - 120.0) < 0.001,
                    "A drag on the only F6 video's bar did not land on its own time");
            // A second pane: the bar is the master timeline again.
            seedLiveLocalPane(app, 3, queue, 2, true);
            require(app.barTime().pane < 0 && app.barTime().position == app.clock_.position(),
                    "Several panes did not share the master bar");
        }
        {
            // An Emby replacement still being resolved has no time yet, and
            // the master clock under it says nothing about it.
            App stream;
            stream.window_ = stream.videoWindow_ = parent.window;
            stream.instance_ = GetModuleHandleW(nullptr);
            App::EmbyPane pane;
            pane.itemId = "next";
            pane.resolving = true;
            stream.embyPanes_[1] = pane;
            stream.paths_[1] = L"emby://srv/next";
            stream.clock_.seek(3324.0);
            stream.clock_.pause();
            auto shown = stream.barTime();
            require(shown.pane == 1 && shown.position == 0.0 && shown.duration == 0.0,
                    "An Emby video being resolved showed the master clock");
            stream.embyPanes_[1]->resolving = false;
            stream.sources_[1] = std::make_unique<VideoSource>();
            AudioSeekTestAccess::seed(*stream.sources_[1], 600.0);
            stream.syncAdjustments_[1] = 90.0 - 3324.0;
            shown = stream.barTime();
            require(shown.position == 90.0 && shown.duration == 600.0,
                    "The only Emby video's bar did not show its resume point");
        }
        {
            // One manual video at 1:15: Page Down and a file dropped on it
            // start from zero, playing or paused as before; among several
            // the replaced pane joins the deck's time.
            App app;
            prepareLocalBrowser(app, parent, queue);
            app.paths_[0] = queue[0].path;
            app.sources_[0] = std::make_unique<VideoSource>();
            AudioSeekTestAccess::seed(*app.sources_[0], queue[0].duration);
            app.clock_.seek(75.0);
            app.clock_.pause();
            app.deviceRecoveryResume_ = false;
            require(app.barTime().pane < 0 && app.barTime().position == 75.0,
                    "Premise: one manual video is the master timeline");
            app.openAdjacentFile(1, 0);
            require(app.paths_[0] == queue[1].path && app.clock_.position() == 0.0 &&
                    app.deviceRecoveryPosition_ == 0.0 && !app.playbackIntended(),
                    "Page Down on the only video kept the time the previous file had reached");
            app.clock_.seek(75.0);
            app.deviceRecoveryResume_ = true;
            app.openAdjacentFile(-1, 0);
            require(app.paths_[0] == queue[0].path && app.clock_.position() == 0.0 && app.playbackIntended(),
                    "Page Up on the only playing video kept its time or stopped it");
            app.clock_.seek(75.0);
            app.addFiles({queue[2].path}, 0);
            require(app.paths_[0] == queue[2].path && app.clock_.position() == 0.0,
                    "A file dropped on the only video kept the time the replaced one had reached");
            app.paths_[1] = queue[1].path;
            app.sources_[1] = std::make_unique<VideoSource>();
            AudioSeekTestAccess::seed(*app.sources_[1], queue[1].duration);
            app.clock_.seek(75.0);
            app.openAdjacentFile(1, 1);
            app.addFiles({queue[1].path}, 0);
            require(app.paths_[1] == queue[2].path && app.paths_[0] == queue[1].path &&
                    app.clock_.position() == 75.0,
                    "Replacing one of several manual videos moved the deck's time");
        }
        KillTimer(parent.window, app_internal::kSettingsSaveTimer);
    }

    // A local folder follows the same sort keys: by name (Explorer's
    // order), by size, by date, reversed, or one shuffle per folder.
    static void localFolderFollowsTheSortKeys() {
        const auto directory = std::filesystem::temp_directory_path() / L"QuadDeckSortTest";
        std::error_code code;
        std::filesystem::remove_all(directory, code);
        std::filesystem::create_directories(directory, code);
        require(!code, "Cannot create the fixture directory");
        const auto make = [&](const wchar_t* name, std::size_t bytes) {
            std::ofstream file(directory / name, std::ios::binary);
            file << std::string(bytes, 'x');
        };
        make(L"clip (2).mp4", 30);
        make(L"clip (10).mp4", 10);
        make(L"clip (1).mp4", 20);
        App app;
        HiddenWindow parent;
        app.window_ = app.videoWindow_ = parent.window;
        app.instance_ = GetModuleHandleW(nullptr);
        const std::wstring current = (directory / L"clip (1).mp4").wstring();
        const auto names = [&](const std::vector<std::wstring>& files) {
            std::wstring joined;
            for (const auto& file : files) joined += std::filesystem::path(file).filename().wstring() + L"|";
            return joined;
        };
        int position = -1;
        app.embyBrowser_.sort = static_cast<int>(emby::SortKey::Name);
        app.embyBrowser_.descending = false;
        require(names(app.siblingFiles(current, position)) == L"clip (1).mp4|clip (2).mp4|clip (10).mp4|" && position == 0,
                "Names are not in Explorer's order");
        app.embyBrowser_.descending = true;
        require(names(app.siblingFiles(current, position)) == L"clip (10).mp4|clip (2).mp4|clip (1).mp4|" && position == 2,
                "Names did not reverse");
        app.embyBrowser_.sort = static_cast<int>(emby::SortKey::Size);
        require(names(app.siblingFiles(current, position)) == L"clip (2).mp4|clip (1).mp4|clip (10).mp4|" && position == 1,
                "Sizes are not largest first");
        app.embyBrowser_.descending = false;
        require(names(app.siblingFiles(current, position)) == L"clip (10).mp4|clip (1).mp4|clip (2).mp4|",
                "Sizes are not smallest first");
        app.embyBrowser_.sort = static_cast<int>(emby::SortKey::Random);
        const auto first = names(app.siblingFiles(current, position));
        require(first == names(app.siblingFiles(current, position)) && position >= 0,
                "The random order does not hold within a session");
        // Length has nothing to go on without opening the files: names.
        app.embyBrowser_.sort = static_cast<int>(emby::SortKey::Runtime);
        require(names(app.siblingFiles(current, position)) == L"clip (1).mp4|clip (2).mp4|clip (10).mp4|",
                "An unknown order did not fall back to names");
        std::filesystem::remove_all(directory, code);
    }

    // At the end of the only video the playback order decides: the next of
    // the list it was chosen from, the list wrapped, another one at random,
    // or nothing. Several videos keep the set's own rule.
    static void playOrderFollowsTheListAtTheEnd() {
        App app;
        HiddenWindow parent;
        app.window_ = app.videoWindow_ = parent.window;
        app.instance_ = GetModuleHandleW(nullptr);
        emby::Session session;
        session.serverUrl = "http://127.0.0.1:9";
        session.serverId = "srv";
        session.userId = "u1";
        session.token = "token";
        session.deviceId = "dev";
        app.embyConfigure(session);
        const auto video = [](const char* id) {
            emby::Item item;
            item.id = id;
            item.name = id;
            item.type = "Video";
            item.mediaType = "Video";
            return item;
        };
        app.paths_[0] = L"emby://srv/b";
        app.sources_[0] = std::make_unique<VideoSource>();
        App::EmbyPane pane;
        pane.itemId = "b";
        pane.serial = ++app.embyPaneSerial_;
        bindEmbyAccount(app, pane);
        app.embyPanes_[0] = pane;
        const auto playing = [&] {
            return app.embyPanes_[0] ? app.embyPanes_[0]->itemId : std::string();
        };
        mutablePlaybackQueue(app).items = {video("a"), video("b"), video("c")};
        mutablePlaybackQueue(app).cursor = 1;
        app.playOrder_ = PlayOrder::PlayOne;
        require(!app.finishSingleVideo(), "An Emby source used the local-only end handler");
        simulateEmbyEnd(app);
        require(playing() == "b" && app.sourcePaused_[0] && !app.clock_.isPlaying(),
                "Play one did not stop at the end");
        app.playOrder_ = PlayOrder::InOrder;
        simulateEmbyEnd(app);
        require(playing() == "c" && app.paths_[0] == L"emby://srv/c",
                "In order did not go to the next of the list");
        simulateEmbyEnd(app);
        require(playing() == "c", "In order went past the end of the list");
        app.playOrder_ = PlayOrder::RepeatList;
        simulateEmbyEnd(app);
        require(playing() == "a", "Repeat the list did not wrap round");
        app.playOrder_ = PlayOrder::Shuffle;
        simulateEmbyEnd(app);
        require(playing() != "a" && !playing().empty(),
                "Shuffle played the same one again");
        // The keys: Ctrl+4/6/7/8/9 choose an order; plain digits still
        // switch audio panes.
        app.setPlayOrder(PlayOrder::RepeatList);
        require(app.playOrder_ == PlayOrder::RepeatList, "setPlayOrder");
        require(app.captureAppSettings().playOrder == PlayOrder::RepeatList, "The order is not saved with the settings");
        // Two videos: the set's own rule, not the order.
        app.paths_[1] = L"C:\\Videos\\two.mp4";
        app.sources_[1] = std::make_unique<VideoSource>();
        require(!app.finishSingleVideo(), "Several videos followed the single-video order");
    }

    // E07: sidecar subtitles are found and read off the window thread and
    // arrive through the main-thread queue, for the pane that asked and only
    // while it still shows the file that asked.
    static void sidecarSubtitlesArriveOffTheWindowThread() {
        const auto directory = std::filesystem::temp_directory_path() / L"QuadDeckSidecarTest";
        std::error_code code;
        std::filesystem::remove_all(directory, code);
        std::filesystem::create_directories(directory, code);
        require(!code, "Cannot create the fixture directory");
        { std::ofstream video(directory / L"clip.mp4", std::ios::binary); }
        {
            std::ofstream srt(directory / L"clip.chs.srt", std::ios::binary);
            srt << "\xEF\xBB\xBF" "1\r\n00:00:01,000 --> 00:00:02,000\r\nHello\r\n";
        }
        App app;
        HiddenWindow parent;
        app.window_ = app.videoWindow_ = parent.window;
        app.instance_ = GetModuleHandleW(nullptr);
        const std::wstring clip = (directory / L"clip.mp4").wstring();
        app.loadSidecarSubtitles(0, clip);
        require(!app.subtitles_[0], "The scan must not complete on the calling thread");
        for (int i = 0; i < 500 && !app.subtitles_[0]; ++i) {
            app.mainQueue_->drain();
            Sleep(10);
        }
        require(app.subtitles_[0] && app.subtitleFiles_[0].size() == 1 &&
                app.subtitleSelection_[0].kind == App::SubtitleKind::File &&
                app.subtitleSelection_[0].file == directory / L"clip.chs.srt",
                "Sidecar subtitles did not arrive");
        require(subtitleTextAt(*app.subtitles_[0], 1.5) == L"Hello", "The sidecar's cue is wrong");
        // A scan whose pane has moved on to another file is dropped.
        app.loadSidecarSubtitles(0, clip);
        app.loadSidecarSubtitles(0, L"C:\\nowhere\\none.mp4");
        require(!app.subtitles_[0], "Reloading must clear the old subtitles");
        for (int i = 0; i < 50; ++i) {
            app.mainQueue_->drain();
            Sleep(10);
        }
        require(!app.subtitles_[0] && app.subtitleFiles_[0].empty(), "A stale scan result was applied");
        std::filesystem::remove_all(directory, code);
    }

    // An Emby item's subtitles: the account named none, so the player picks
    // the stream that suits the languages wanted; the server's own choice
    // wins when it makes one, the viewer's over both; pictures are listed
    // but never chosen; the menu, the keys and the sheet all act on the
    // same state. The streams inside the file are read with the video, the
    // ones beside it asked of the server.
    static void subtitlesAreChosenForTheViewer() {
        App app;
        HiddenWindow parent;
        app.window_ = app.videoWindow_ = parent.window;
        app.instance_ = GetModuleHandleW(nullptr);
        // The list this was written for: an English Windows with Chinese added.
        app.systemLanguagesOverride_ = std::vector<std::string>{"en-US", "zh-Hans-CN"};
        require(app.subtitleWanted() == (std::vector<std::string>{"zh-Hans", "en"}) &&
                app.subtitleLocale() == L"zh-Hans", "The wanted languages are not Windows' own, English last");
        emby::Session session;
        session.serverUrl = "http://127.0.0.1:9";
        session.serverId = "srv";
        session.userId = "u1";
        session.token = "token";
        session.deviceId = "dev";
        app.emby_.configure(session);
        const auto stream = [](int index, const char* codec, const char* language, const char* title,
                               const char* display, bool external, bool isDefault, bool text) {
            emby::MediaStream entry;
            entry.index = index;
            entry.type = "Subtitle";
            entry.codec = codec;
            entry.language = language;
            entry.title = title;
            entry.displayTitle = display;
            entry.isExternal = external;
            entry.isDefault = isDefault;
            entry.isTextSubtitle = text;
            return entry;
        };
        const auto playItem = [&] {
            app.resetPaneMediaState(0);
            app.sources_[0] = std::make_unique<VideoSource>();
            app.paths_[0] = L"emby://srv/42";
            App::EmbyPane pane;
            pane.serial = ++app.embyPaneSerial_;
            pane.itemId = "42";
            pane.mediaSourceId = "ms";
            emby::MediaStream video;
            video.index = 0;
            video.type = "Video";
            pane.streams = {video,
                            stream(2, "ass", "", "TC", "(ASS)", false, false, true),
                            stream(3, "ass", "", "SC", "(\xE9\xBB\x98\xE8\xAE\xA4 ASS)", false, true, true),
                            stream(4, "sup", "chi", "", "Chinese (PGSSUB)", true, false, false),
                            stream(5, "srt", "eng", "", "English (SRT)", true, false, true)};
            bindEmbyAccount(app, pane);
            app.embyPanes_[0] = pane;
        };
        const auto shown = [&](App::SubtitleKind kind, int index) {
            return app.subtitleSelection_[0].kind == kind && app.subtitleSelection_[0].stream == index;
        };
        playItem();
        const auto options = app.subtitleOptions(0);
        require(options.size() == 4 && options[0].usable && options[1].usable && !options[2].usable &&
                options[3].usable && options[0].label == L"(ASS)  \x00B7  TC" &&
                options[2].label.find(L"pictures") != std::wstring::npos && options[3].label == L"English (SRT)",
                "An Emby item's subtitle streams are not what the menu lists");
        require(options[0].what.kind == App::SubtitleKind::Embedded &&
                options[1].what.kind == App::SubtitleKind::Embedded &&
                options[2].what.kind == App::SubtitleKind::EmbyStream &&
                options[3].what.kind == App::SubtitleKind::EmbyStream,
                "An Emby item's streams inside its file are not read with the video");
        require(app.subtitleLabel(0) == L"Off", "Nothing is shown before anything is chosen");
        // No default from the server: Simplified before Traditional before
        // English, those inside the file ranked with the one beside it.
        app.autoChooseSubtitle(0, -1);
        require(shown(App::SubtitleKind::Embedded, 3) && app.sources_[0]->subtitleTrack() == 3 &&
                app.embyPanes_[0]->subtitleStream == -1 && !app.subtitleChosenByViewer_[0],
                "The stream in the wanted language was not chosen, or was asked of the server");
        require(app.subtitleLabel(0) == options[1].label, "The shown subtitle's label is wrong");
        // The server's own choice, where it made one, wherever the stream
        // comes from; never a stream of pictures.
        playItem();
        app.autoChooseSubtitle(0, 5);
        require(shown(App::SubtitleKind::EmbyStream, 5), "The server's default stream was not taken");
        playItem();
        app.autoChooseSubtitle(0, 2);
        require(shown(App::SubtitleKind::Embedded, 2), "The server's default stream inside the file was not taken");
        // Once the video is open, a stream it turns out not to read -- one
        // past the streams it reads, say -- is asked of the server, and the
        // menu says where each comes from; one it reads stays with it.
        playItem();
        app.autoChooseSubtitle(0, -1);
        {
            VideoSource& source = *app.sources_[0];
            std::scoped_lock lock(source.subtitleMutex_);
            source.subtitleEvents_.push_back({2, "[Script Info]\n", false, {}});
        }
        app.sources_[0]->ready_.store(true);
        app.subtitleEmbeddedPending_[0] = true;
        app.adoptEmbeddedSubtitles();
        require(shown(App::SubtitleKind::EmbyStream, 3) && app.embyPanes_[0]->subtitleStream == 3 &&
                !app.subtitleEmbeddedPending_[0] && !app.subtitleChosenByViewer_[0],
                "A stream the video does not carry was not asked of the server");
        const auto opened = app.subtitleOptions(0);
        require(opened[0].what.kind == App::SubtitleKind::Embedded &&
                opened[1].what.kind == App::SubtitleKind::EmbyStream && app.subtitleLabel(0) == opened[1].label,
                "The menu does not say where the streams come from once the video is open");
        app.selectSubtitle(0, opened[0].what, true);
        app.subtitleEmbeddedPending_[0] = true;
        app.adoptEmbeddedSubtitles();
        require(shown(App::SubtitleKind::Embedded, 2) && app.sources_[0]->subtitleTrack() == 2 &&
                app.embyPanes_[0]->subtitleStream == -1, "A stream the video carries was not left to it");
        playItem();
        app.autoChooseSubtitle(0, 4);
        require(shown(App::SubtitleKind::Embedded, 3), "A picture stream named by the server must be passed over");
        // Another language in the sheet re-chooses what plays.
        app.setSubtitleLanguage(static_cast<int>(SubtitleLanguage::ChineseTraditional));
        require(shown(App::SubtitleKind::Embedded, 2) && app.captureAppSettings().subtitles.language == 2,
                "Changing the language did not re-choose the stream");
        app.setSubtitleLanguage(static_cast<int>(SubtitleLanguage::English));
        require(shown(App::SubtitleKind::EmbyStream, 5), "English was asked for and not shown");
        // The viewer's choice at the menu stands against both.
        app.handleContextCommand(app_internal::CmdSubtitleFirst + 1, 0);
        require(shown(App::SubtitleKind::Embedded, 3) && app.subtitleChosenByViewer_[0] &&
                app.noticeText_.find(L"Subtitles: ") == 0, "The menu's choice was not taken");
        app.setSubtitleLanguage(static_cast<int>(SubtitleLanguage::ChineseTraditional));
        app.autoChooseSubtitle(0, 5);
        require(shown(App::SubtitleKind::Embedded, 3), "The viewer's choice was replaced");
        // A greyed entry does nothing even when its command arrives.
        app.handleContextCommand(app_internal::CmdSubtitleFirst + 2, 0);
        require(shown(App::SubtitleKind::Embedded, 3), "A picture stream was selected");
        app.handleContextCommand(app_internal::CmdSubtitleOff, 0);
        require(app.subtitleSelection_[0].kind == App::SubtitleKind::None && app.embyPanes_[0]->subtitleStream == -1 &&
                app.noticeText_ == L"Subtitles off", "Off did not turn the subtitles off");
        // Alt+L: each usable one in turn, then off again.
        const LPARAM alt = static_cast<LPARAM>(1) << 29;
        for (const int expected : {2, 3, 5}) {
            app.handleMessage(WM_SYSKEYDOWN, 'L', alt);
            require(shown(expected == 5 ? App::SubtitleKind::EmbyStream : App::SubtitleKind::Embedded, expected),
                    "Alt+L did not step to the next subtitle");
        }
        app.handleMessage(WM_SYSKEYDOWN, 'L', alt);
        require(app.subtitleSelection_[0].kind == App::SubtitleKind::None, "Alt+L did not come round to off");

        // The menu itself: shown, off, the streams, and the rest.
        app.handleContextCommand(app_internal::CmdSubtitleFirst + 3, 0);
        HMENU menu = CreatePopupMenu();
        app.appendSubtitleMenu(menu, 0);
        const auto state = [&](unsigned command) { return GetMenuState(menu, command, MF_BYCOMMAND); };
        require((state(app_internal::CmdSubtitleShow) & MF_CHECKED) != 0 &&
                (state(app_internal::CmdSubtitleOff) & MF_CHECKED) == 0 &&
                (state(app_internal::CmdSubtitleFirst + 3) & MF_CHECKED) != 0 &&
                (state(app_internal::CmdSubtitleFirst + 0) & MF_CHECKED) == 0 &&
                (state(app_internal::CmdSubtitleFirst + 2) & MF_GRAYED) != 0 &&
                (state(app_internal::CmdSubtitleLoad) & MF_GRAYED) == 0 &&
                (state(app_internal::CmdSubtitleSyncReset) & MF_GRAYED) != 0,
                "The Subtitles menu does not show the state");
        DestroyMenu(menu);
        // A command names an entry by its place in the menu as it was shown.
        // The player runs on under an open menu: a stream that arrives
        // meanwhile must not turn the click into another entry.
        app.embyPanes_[0]->streams.insert(app.embyPanes_[0]->streams.begin(),
                                          stream(1, "srt", "fre", "", "French (SRT)", true, false, true));
        app.handleContextCommand(app_internal::CmdSubtitleFirst + 1, 0);
        require(shown(App::SubtitleKind::Embedded, 3), "A menu choice was shifted by what arrived under the menu");
        app.subtitleMenuOptions_.clear();
        app.subtitleMenuPane_ = -1;
        app.handleContextCommand(app_internal::CmdSubtitleFirst + 1, 0);
        require(shown(App::SubtitleKind::Embedded, 2), "Without an open menu a command goes by what the pane offers");
        app.embyPanes_[0]->streams.erase(app.embyPanes_[0]->streams.begin());
        // A video with more streams than the menu has room for says so.
        for (int extra = 0; extra < 100; ++extra) {
            app.embyPanes_[0]->streams.push_back(stream(10 + extra, "ssa", "zh-CN", "", "Chinese Simplified (SSA)", true, false, true));
        }
        menu = CreatePopupMenu();
        app.appendSubtitleMenu(menu, 0);
        wchar_t text[128]{};
        bool saidMore = false;
        for (int item = 0; item < GetMenuItemCount(menu); ++item) {
            GetMenuStringW(menu, static_cast<UINT>(item), text, 128, MF_BYPOSITION);
            saidMore = saidMore || std::wstring(text).find(L"40 more") == 0;
        }
        require(saidMore && state(app_internal::CmdSubtitleLast) != static_cast<UINT>(-1),
                "A long list of streams is not cut where the menu's commands end");
        DestroyMenu(menu);

        // Timing: PotPlayer's keys, the pane they act on being the only video.
        require(app.subtitlePane() == 0, "The only video is not the subtitle keys' target");
        app.handleMessage(WM_KEYDOWN, VK_OEM_COMMA, 0);
        require(app.subtitleDelay_[0] == 0.5 && app.noticeText_.find(L"later") != std::wstring::npos,
                "The comma key did not show the subtitles later");
        app.handleMessage(WM_KEYDOWN, VK_OEM_PERIOD, 0);
        app.handleMessage(WM_KEYDOWN, VK_OEM_PERIOD, 0);
        require(app.subtitleDelay_[0] == -0.5 && app.noticeText_.find(L"earlier") != std::wstring::npos,
                "The full stop did not show the subtitles earlier");
        app.handleMessage(WM_KEYDOWN, VK_OEM_2, 0);
        require(app.subtitleDelay_[0] == 0.0, "The slash did not reset the timing");
        // Size, place and visibility: Alt keys, and only with Alt.
        app.handleMessage(WM_SYSKEYDOWN, VK_PRIOR, alt);
        app.handleMessage(WM_SYSKEYDOWN, VK_UP, alt);
        app.handleMessage(WM_SYSKEYDOWN, 'H', alt);
        auto saved = app.captureAppSettings().subtitles;
        require(std::abs(saved.size - 1.1F) < 0.001F && std::abs(saved.position - 0.02F) < 0.001F && !saved.show,
                "The Alt keys did not reach the settings that are saved");
        require(app.noticeText_.find(L"Subtitles") == 0, "Alt+H said nothing");
        app.handleMessage(WM_SYSKEYDOWN, VK_NEXT, 0);
        app.handleMessage(WM_SYSKEYDOWN, 'H', 0);
        require(app.captureAppSettings().subtitles == saved, "A key without Alt changed the subtitles");
        app.handleMessage(WM_SYSKEYDOWN, VK_HOME, alt);
        app.handleMessage(WM_SYSKEYDOWN, 'H', alt);
        for (int i = 0; i < 40; ++i) app.handleMessage(WM_SYSKEYDOWN, VK_NEXT, alt);
        saved = app.captureAppSettings().subtitles;
        require(saved.show && saved.position == SubtitleSettings{}.position && saved.size == kSubtitleSizeMinimum,
                "Alt+Home, Alt+H or the size's lower bound is wrong");

        // The sheet: the tab's rows, and a hit on the pane's buttons.
        app.settingsOpen_ = true;
        app.settingsAlpha_ = 1.0F;
        app.settingsTab_ = SettingsTab::Subtitles;
        const RECT client{0, 0, 400, 300};
        app.refreshPanelGeometry(client);
        int showRow = -1, languageRow = -1, sizeRow = -1, positionRow = -1, backgroundRow = -1, paneRow = -1;
        for (std::size_t i = 0; i < app.panelRows_.size(); ++i) {
            const auto id = app.panelRows_[i].id;
            if (id == SettingId::SubtitleShow) showRow = static_cast<int>(i);
            else if (id == SettingId::SubtitleLanguage) languageRow = static_cast<int>(i);
            else if (id == SettingId::SubtitleSize) sizeRow = static_cast<int>(i);
            else if (id == SettingId::SubtitlePosition) positionRow = static_cast<int>(i);
            else if (id == SettingId::SubtitleBackground) backgroundRow = static_cast<int>(i);
            else if (id == SettingId::SubtitlePane) paneRow = static_cast<int>(i);
        }
        require(showRow >= 0 && languageRow > showRow && sizeRow > languageRow && positionRow > sizeRow &&
                backgroundRow > positionRow && paneRow > backgroundRow,
                "The Subtitles section of the sheet is incomplete");
        const auto& language = app.panelRows_[static_cast<std::size_t>(languageRow)];
        require(app.panelRows_[static_cast<std::size_t>(showRow)].on && language.options.size() == 6 &&
                language.selected == 2 && language.options[0] == L"Auto",
                "The sheet does not show the subtitle settings");
        require(app.panelRows_[static_cast<std::size_t>(paneRow)].label.find(L"V1") == 0 &&
                app.panelRows_[static_cast<std::size_t>(paneRow)].options.size() == 4,
                "The pane's subtitle row is wrong");
        app.activatePanelHit({PanelHitKind::Button, paneRow, 2});
        require(app.subtitleDelay_[0] == 0.5, "Later in the sheet did not delay the subtitles");
        app.activatePanelHit({PanelHitKind::Button, paneRow, 1});
        require(app.subtitleDelay_[0] == 0.0, "Earlier in the sheet did not undo it");
        app.activatePanelHit({PanelHitKind::Segment, languageRow, 0});
        require(app.subtitleSettings_.language == 0, "Auto in the sheet was not taken");
        app.applyPanelSlider(sizeRow, 0.26F);
        require(std::abs(app.subtitleSettings_.size - 1.0F) < 0.001F, "The size slider does not land on steps");
        app.applyPanelSlider(positionRow, 0.5F);
        require(std::abs(app.subtitleSettings_.position - 0.4F) < 0.001F, "The position slider is wrong");
        // The box behind the lines: off until asked for, and what the
        // switch sets is what is saved.
        require(!app.panelRows_[static_cast<std::size_t>(backgroundRow)].on &&
                !app.captureAppSettings().subtitles.background,
                "Subtitles stand on a box nobody asked for");
        app.activatePanelHit({PanelHitKind::Toggle, backgroundRow, -1});
        require(app.subtitleSettings_.background && app.captureAppSettings().subtitles.background,
                "The sheet's switch did not put the box behind the subtitles");
        app.activatePanelHit({PanelHitKind::Toggle, backgroundRow, -1});
        require(!app.captureAppSettings().subtitles.background,
                "The box behind the subtitles cannot be taken away again");
        app.activatePanelHit({PanelHitKind::Toggle, showRow, -1});
        require(!app.subtitleSettings_.show, "The sheet's switch did not hide the subtitles");

        // New media on the pane starts clean, and retires what was in flight.
        const auto serial = app.subtitleSerial_[0];
        app.subtitleDelay_[0] = 2.0;
        app.resetPaneMediaState(0);
        require(app.subtitleSelection_[0].kind == App::SubtitleKind::None && app.subtitleDelay_[0] == 0.0 &&
                !app.subtitleChosenByViewer_[0] && app.subtitleSerial_[0] != serial &&
                !app.subtitlePaneWithSerial(serial).has_value(), "A pane's subtitle state outlived its media");
        KillTimer(parent.window, app_internal::kSettingsSaveTimer);
        app.settingsSavePending_ = false;
    }

    // A local video's subtitles: the file beside it whose name says the
    // wanted language, a file loaded by hand, and the stream inside it the
    // decoder is told to read from the first packet.
    static void localSubtitlesFollowTheLanguageAndTheViewer() {
        const auto directory = std::filesystem::temp_directory_path() / L"QuadDeckSubtitleChoiceTest";
        std::error_code code;
        std::filesystem::remove_all(directory, code);
        std::filesystem::create_directories(directory, code);
        require(!code, "Cannot create the fixture directory");
        const auto write = [&](const wchar_t* name, const char* content) {
            std::ofstream file(directory / name, std::ios::binary);
            file << content;
        };
        write(L"clip.mkv", "");
        write(L"clip.chs.srt", "1\n00:00:01,000 --> 00:00:02,000\nSimplified\n");
        write(L"clip.cht.ass", "[Script Info]\n\n[Events]\nDialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,{\\an8}Traditional\n");
        write(L"clip.srt", "1\n00:00:01,000 --> 00:00:02,000\nUnnamed\n");
        write(L"clip.empty.vtt", "WEBVTT\n");
        write(L"other.srt", "1\n00:00:01,000 --> 00:00:02,000\nOther\n");
        write(L"nothing.srt", "no timing at all\n");
        const std::wstring clip = (directory / L"clip.mkv").wstring();
        const auto settle = [](App& app) {
            for (int i = 0; i < 100; ++i) {
                app.mainQueue_->drain();
                Sleep(10);
            }
        };
        const auto waitForTrack = [](App& app) {
            for (int i = 0; i < 500 && !app.subtitles_[0]; ++i) {
                app.mainQueue_->drain();
                Sleep(10);
            }
            require(app.subtitles_[0] != nullptr, "The subtitle file did not arrive");
        };
        const auto bottom = [](App& app) { return subtitleLinesAt(*app.subtitles_[0], 1.5).bottom; };
        struct Case {
            std::vector<std::string> languages;
            const wchar_t* file;
        };
        // Simplified, Traditional, and for a reader of neither the file that
        // names no language rather than one that names another.
        for (const Case& expected : {Case{{"en-US", "zh-Hans-CN"}, L"clip.chs.srt"},
                                     Case{{"zh-TW"}, L"clip.cht.ass"}, Case{{"en-US"}, L"clip.srt"}}) {
            App app;
            HiddenWindow parent;
            app.window_ = app.videoWindow_ = parent.window;
            app.systemLanguagesOverride_ = expected.languages;
            app.loadSidecarSubtitles(0, clip);
            waitForTrack(app);
            require(app.subtitleFiles_[0].size() == 4 && app.subtitleSelection_[0].kind == App::SubtitleKind::File &&
                    app.subtitleSelection_[0].file.filename() == expected.file,
                    "The file beside the video in the wanted language was not the one shown");
        }
        App app;
        HiddenWindow parent;
        app.window_ = app.videoWindow_ = parent.window;
        app.instance_ = GetModuleHandleW(nullptr);
        app.systemLanguagesOverride_ = std::vector<std::string>{"zh-TW"};
        app.sources_[0] = std::make_unique<VideoSource>();
        app.paths_[0] = clip;
        app.loadSidecarSubtitles(0, clip);
        waitForTrack(app);
        require(subtitleLinesAt(*app.subtitles_[0], 1.5).top == L"Traditional", "An ASS file's placement was lost");
        const auto options = app.subtitleOptions(0);
        require(options.size() == 4 && options[0].label == L"clip.chs.srt" && options[0].candidate.title == L"chs" &&
                options[3].candidate.title == L"" && options[0].candidate.external,
                "The files beside the video are not what the menu lists");
        // A file loaded by hand is added, shown, and the viewer's choice.
        app.addSubtitleFile(0, directory / L"other.srt");
        require(app.subtitleChosenByViewer_[0] && !app.subtitles_[0] && app.subtitleFiles_[0].size() == 5,
                "A loaded file was not added, or was read on the calling thread");
        waitForTrack(app);
        require(bottom(app) == L"Other" && app.subtitleLabel(0) == L"other.srt", "The loaded file is not what is shown");
        // A choice made while a file is still being read stands.
        app.addSubtitleFile(0, directory / L"clip.srt");
        app.selectSubtitle(0, {App::SubtitleKind::File, -1, directory / L"clip.chs.srt"}, true);
        waitForTrack(app);
        settle(app);
        require(bottom(app) == L"Simplified", "A read that finished late replaced a newer choice");
        // A file without a line says so and leaves nothing selected.
        app.addSubtitleFile(0, directory / L"nothing.srt");
        settle(app);
        require(app.subtitleSelection_[0].kind == App::SubtitleKind::None && !app.subtitles_[0] &&
                app.noticeText_.find(L"No subtitle lines") == 0, "A file without lines was accepted");
        // Alt+L steps past a file that turns out to hold no line instead of
        // starting over at it, and does not stop at one known to hold none.
        std::vector<std::wstring> stepped;
        for (int step = 0; step < 8; ++step) {
            app.cycleSubtitle(0);
            // Until the file chosen has been read, or found to hold nothing.
            for (int i = 0; i < 500 && app.subtitleSelection_[0].kind != App::SubtitleKind::None &&
                            !app.subtitles_[0]; ++i) {
                app.mainQueue_->drain();
                Sleep(10);
            }
            stepped.push_back(app.subtitleSelection_[0].kind == App::SubtitleKind::None
                ? L"-" : app.subtitleSelection_[0].file.filename().wstring());
        }
        const std::vector<std::wstring> expectedSteps{
            L"clip.chs.srt", L"clip.cht.ass", L"-", L"clip.srt", L"other.srt", L"-", L"clip.chs.srt", L"clip.cht.ass"};
        require(stepped == expectedSteps, "Alt+L did not step through the files past the ones without lines");
        require(app.subtitleFilesWithoutLines_[0].size() == 2 && !app.subtitleOptions(0)[2].usable &&
                app.subtitleOptions(0)[2].label.find(L"no lines") != std::wstring::npos,
                "A file found to hold no line is still offered");
        // The same when the player chose it: the next best takes its place.
        {
            App chooser;
            HiddenWindow chooserParent;
            chooser.window_ = chooser.videoWindow_ = chooserParent.window;
            chooser.systemLanguagesOverride_ = std::vector<std::string>{"en-US"};
            chooser.paths_[0] = clip;
            chooser.subtitleSerial_[0] = ++chooser.subtitleSerialCounter_;
            chooser.subtitleFiles_[0] = {directory / L"clip.empty.vtt", directory / L"clip.srt"};
            chooser.autoChooseSubtitle(0);
            require(chooser.subtitleSelection_[0].file.filename() == L"clip.empty.vtt", "Premise: the empty file is tried first");
            waitForTrack(chooser);
            require(chooser.subtitleSelection_[0].file.filename() == L"clip.srt" && bottom(chooser) == L"Unnamed",
                    "A file without lines chosen by the player did not give way to the next");
        }
        // A file that is not Unicode is read in the code page its name says,
        // whatever the system's own is: Big5 and GBK here.
        write(L"legacy.cht.srt", "1\n00:00:01,000 --> 00:00:02,000\n\xC1\x63\xC5\xE9\n");
        write(L"legacy.chs.srt", "1\n00:00:01,000 --> 00:00:02,000\n\xBC\xF2\xCC\xE5\n");
        app.addSubtitleFile(0, directory / L"legacy.cht.srt");
        waitForTrack(app);
        require(bottom(app) == L"\x7E41\x9AD4", "A Big5 subtitle file was not read as Big5");
        app.addSubtitleFile(0, directory / L"legacy.chs.srt");
        waitForTrack(app);
        require(bottom(app) == L"\x7B80\x4F53", "A GBK subtitle file was not read as GBK");
        app.subtitleFiles_[0].resize(6);
        // The files found beside a video do not replace the viewer's choice.
        app.selectSubtitle(0, {}, true);
        const auto serial = app.subtitleSerial_[0];
        auto scan = App::scanSidecars(clip, app.subtitleWanted());
        require(scan.files.size() == 4 && scan.chosen == 1 && scan.track, "The scan did not choose among the files");
        app.applySidecarScan(0, serial, std::move(scan));
        require(app.subtitleSelection_[0].kind == App::SubtitleKind::None && app.subtitleFiles_[0].size() == 6,
                "A late scan chose over the viewer, or dropped the files loaded by hand");
        // What Explorer may drop on a video as its subtitles.
        require(App::isSubtitleFile(L"C:\\x\\a.SRT") && App::isSubtitleFile(L"b.ass") && App::isSubtitleFile(L"c.vtt") &&
                !App::isSubtitleFile(L"d.mkv") && !App::isSubtitleFile(L"srt"), "Subtitle files are told apart wrongly");

        // The stream inside the file: the decoder is handed the choice.
        const auto choose = app.localSourceOptions().chooseSubtitle;
        require(static_cast<bool>(choose) && app.localSourceOptions().readSubtitles,
                "A local file opens without its subtitle streams being read or chosen");
        std::vector<SubtitleTrackInfo> tracks(4);
        tracks[0] = {3, "eng", "", "subrip", true, true, false};
        tracks[1] = {4, "chi", "SC", "ass", true, false, false};
        tracks[2] = {5, "chi", "TC", "ass", true, false, false};
        tracks[3] = {6, "chi", "", "hdmv_pgs_subtitle", false, false, false};
        require(choose(tracks) == 5, "The embedded stream in the wanted language was not chosen");
        app.systemLanguagesOverride_ = std::vector<std::string>{"fr-FR"};
        require(app.localSourceOptions().chooseSubtitle(tracks) == 3, "No wanted language: the default stream");
        tracks.erase(tracks.begin(), tracks.begin() + 3);
        require(choose(tracks) == -1, "A stream of pictures was chosen");
        // Nothing is taken up from a source that never opened.
        app.subtitleEmbeddedPending_[0] = true;
        app.sources_[0].reset();
        app.adoptEmbeddedSubtitles();
        require(!app.subtitleEmbeddedPending_[0] && app.subtitleSelection_[0].kind == App::SubtitleKind::None,
                "A pane without a source kept waiting for its subtitles");
        KillTimer(parent.window, app_internal::kSettingsSaveTimer);
        app.settingsSavePending_ = false;
        std::filesystem::remove_all(directory, code);
    }

    // libass draws what the pane shows: a script's lines where it places
    // them on the picture, a plain file in its own look, made again when
    // that look changes; the plain lines where libass cannot.
    static void assSubtitlesAreDrawnWhereTheScriptPutsThem() {
        App app;
        HiddenWindow parent;
        app.window_ = app.videoWindow_ = parent.window;
        app.systemLanguagesOverride_ = std::vector<std::string>{"en-US"};
        app.sources_[0] = std::make_unique<VideoSource>();
        app.chromeCells_[0] = {100.0F, 50.0F, 640.0F, 360.0F};
        app.subtitleSerial_[0] = ++app.subtitleSerialCounter_;
        const auto show = [&](const char* text) {
            auto track = parseSubtitles(text);
            require(track.has_value(), "The fixture did not parse");
            app.subtitleSelection_[0] = {App::SubtitleKind::File, -1, L"fixture"};
            app.subtitles_[0] = std::make_shared<const SubtitleTrack>(std::move(*track));
        };
        const auto inkCentre = [](const SubtitleBitmap& bitmap, POINT origin) {
            long long sumX = 0, sumY = 0, count = 0;
            for (const auto& piece : bitmap.pieces) {
                for (int y = 0; y < piece.height; ++y) {
                    for (int x = 0; x < piece.width; ++x) {
                        if ((piece.pixels[static_cast<std::size_t>(y) * piece.width + x] >> 24) < 128) continue;
                        sumX += piece.x + x;
                        sumY += piece.y + y;
                        ++count;
                    }
                }
            }
            require(count > 0, "A subtitle picture with nothing on it");
            return std::pair<double, double>{origin.x + static_cast<double>(sumX) / count,
                                             origin.y + static_cast<double>(sumY) / count};
        };
        show("[Script Info]\nScriptType: v4.00+\nPlayResX: 640\nPlayResY: 360\n\n[V4+ Styles]\n"
             "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, "
             "Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, "
             "MarginL, MarginR, MarginV, Encoding\n"
             "Style: Default,Arial,30,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,0,0,2,"
             "10,10,10,1\n\n[Events]\n"
             "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
             "Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,{\\an5\\pos(160,90)}Sign\n");
        bool drawn = false;
        POINT origin{};
        auto picture = app.renderAssSubtitles(0, 1.5, drawn, origin);
        require(drawn && picture && origin.x == 100 && origin.y == 50, "libass did not draw the script");
        // An unopened source has no size: the picture is the pane.
        const auto [x, y] = inkCentre(*picture, origin);
        require(std::abs(x - 260.0) < 8.0 && std::abs(y - 140.0) < 8.0, "\\pos was not measured against the picture");
        require(app.renderAssSubtitles(0, 3.0, drawn, origin) == nullptr && drawn,
                "A moment without a line is not empty");
        require(!app.assPanes_[0].plain && !app.assPanes_[0].failed, "A script was taken for a plain file");

        // A plain file in the plain look, its box made again with the setting.
        show("1\n00:00:01,000 --> 00:00:02,000\nPlain line\n");
        picture = app.renderAssSubtitles(0, 1.5, drawn, origin);
        require(drawn && picture && app.assPanes_[0].plain, "libass did not draw the plain file");
        require(inkCentre(*picture, origin).second > 50.0 + 360.0 * 0.7, "A plain line is not at the bottom");
        const auto unboxed = picture;
        app.subtitleSettings_.background = true;
        picture = app.renderAssSubtitles(0, 1.5, drawn, origin);
        require(picture && picture != unboxed && app.assPanes_[0].plainStyle.box,
                "The box setting did not make the plain script again");
        // Raised by the viewer: the bottom line moves up.
        app.subtitleSettings_.background = false;
        const double resting = inkCentre(*app.renderAssSubtitles(0, 1.5, drawn, origin), origin).second;
        app.subtitleSettings_.position = 0.3F;
        const double raised = inkCentre(*app.renderAssSubtitles(0, 1.5, drawn, origin), origin).second;
        require(resting - raised > 60.0, "Raising did not move the plain line up");
        app.subtitleSettings_.position = 0.0F;

        // A file libass takes nothing from is drawn as plain lines.
        SubtitleTrack unreadable;
        unreadable.cues = {{1.0, 2.0, L"Fallback", false}};
        unreadable.text = "[Script Info]\n[Events]\n";
        unreadable.ass = true;
        app.subtitles_[0] = std::make_shared<const SubtitleTrack>(std::move(unreadable));
        require(app.renderAssSubtitles(0, 1.5, drawn, origin) == nullptr && !drawn && app.assPanes_[0].failed,
                "A script libass could not read was not left to the plain lines");
        app.assDisabled_ = true;
        show("1\n00:00:01,000 --> 00:00:02,000\nPlain line\n");
        require(!app.syncAssSubtitles(0), "libass drew while turned off");
        app.assDisabled_ = false;

        // The pane's libass goes with its media and moves with a swap.
        require(app.syncAssSubtitles(0) && app.assPanes_[0].subtitles, "No libass for the pane");
        AssSubtitles* held = app.assPanes_[0].subtitles.get();
        app.sources_[1] = std::make_unique<VideoSource>();
        app.swapPanes(0, 1);
        require(app.assPanes_[1].subtitles.get() == held && !app.assPanes_[0].subtitles,
                "A swap left the pane's libass behind");
        app.resetPaneMediaState(1);
        require(!app.assPanes_[1].subtitles, "New media kept the old video's libass and fonts");
        KillTimer(parent.window, app_internal::kSettingsSaveTimer);
        app.settingsSavePending_ = false;
    }

    static void singleSourceUsesOnlyTheMasterTimeline() {
        App app;
        // Exercise the real geometry and hit-test paths with unopened sources:
        // no HWND, renderer, media, audio device or settings I/O is needed.
        const RECT client{0, 0, 1280, 720};
        app.layoutMode_ = LayoutMode::SideBySide;
        app.dockProgress_ = 1.0F;
        app.paneChromeAlpha_.fill(1.0F);
        const auto center = [](const OverlayRect& box) {
            return POINT{static_cast<LONG>(box.x + box.width * 0.5F),
                         static_cast<LONG>(box.y + box.height * 0.5F)};
        };
        const auto requireSingleSourceControls = [&](std::size_t pane) {
            const auto& chrome = app.paneChrome_[pane];
            require(!chrome.timeline.visible() && !chrome.timeLabel.visible(),
                    "A single source still has its own timeline or time label");
            // The former rail area must be video, not an invisible seek target.
            const auto previous = paneChromeLayout(
                app.chromeCells_[pane], app.uiScale_, app.pillWidths_[pane], true,
                app.paneCoveredBottom(app.chromeCells_[pane]), true);
            require(previous.timeline.visible(), "Test pane cannot hold a timeline");
            auto hit = app.hitTestAt(center(previous.timeline));
            require(hit.kind == OverlayHitKind::Video && hit.pane == static_cast<int>(pane),
                    "The hidden single-source timeline still consumes pointer input");
            const auto& master = app.barLayout_[BarItem::Seek];
            require(master.visible(), "The single-source master timeline was hidden");
            hit = app.hitTestAt(center(master));
            require(hit.kind == OverlayHitKind::BarItem && hit.item == static_cast<int>(BarItem::Seek),
                    "The master timeline lost its hit target");
            require(chrome.pill.visible(), "The single-source pane menu was hidden");
            hit = app.hitTestAt(center(chrome.pill));
            require(hit.kind == OverlayHitKind::PanePill && hit.pane == static_cast<int>(pane),
                    "The single-source pane menu lost its hit target");
            for (std::size_t chip = 0; chip < kPaneChipCount; ++chip) {
                require(chrome.chips[chip].visible(), "A single-source action chip was hidden");
                hit = app.hitTestAt(center(chrome.chips[chip]));
                require(hit.kind == OverlayHitKind::PaneChip && hit.pane == static_cast<int>(pane) &&
                            hit.item == static_cast<int>(chip),
                        "A single-source action chip lost its hit target");
            }
            require(chrome.volume.visible(), "The single-source volume rail was hidden");
            hit = app.hitTestAt(center(chrome.volume));
            require(hit.kind == OverlayHitKind::PaneVolume && hit.pane == static_cast<int>(pane),
                    "The single-source volume rail lost its hit target");
        };
        // A sole source is V1 regardless of which internal slot it occupies.
        for (std::size_t pane = 0; pane < kMaxPanes; ++pane) {
            app.sources_[pane] = std::make_unique<VideoSource>();
            app.hoverPane_ = static_cast<int>(pane);
            app.audioMask_ = 1U << pane;
            app.refreshChromeGeometry(client);
            requireSingleSourceControls(pane);

            const std::size_t other = (pane + 1) % kMaxPanes;
            app.sources_[other] = std::make_unique<VideoSource>();
            app.refreshChromeGeometry(client);
            for (const std::size_t loaded : {pane, other}) {
                const auto& chrome = app.paneChrome_[loaded];
                require(chrome.timeline.visible() && chrome.timeLabel.visible(),
                        "Adding a second source did not restore both pane timelines");
                const auto hit = app.hitTestAt(center(chrome.timeline));
                require(hit.kind == OverlayHitKind::PaneTimeline && hit.pane == static_cast<int>(loaded),
                        "A restored pane timeline lost its hit target");
            }
            app.soloPane_ = static_cast<int>(pane);
            app.refreshChromeGeometry(client);
            require(app.paneChrome_[pane].timeline.visible(), "Solo hid a loaded pane's local timeline");
            const auto soloHit = app.hitTestAt(center(app.paneChrome_[pane].timeline));
            require(soloHit.kind == OverlayHitKind::PaneTimeline && soloHit.pane == static_cast<int>(pane),
                    "Solo lost the local timeline hit target");

            app.sources_[other].reset();
            app.refreshChromeGeometry(client);
            requireSingleSourceControls(pane);
            app.soloPane_ = -1;
            app.sources_[pane].reset();
        }
    }

    static void pinnedDockTargetsThePointedPane() {
        App app;
        HiddenWindow window;
        app.window_ = app.videoWindow_ = window.window;
        app.layoutMode_ = LayoutMode::SideBySide;
        app.seekMode_ = SeekMode::Independent;
        app.controlsPinned_ = app.controlsVisible_ = true;
        app.dockProgress_ = 1.0F;
        app.duration_ = 200.0;
        app.clock_.seek(100.0);
        // Unopened decoders are sufficient to exercise the actual command
        // path and inspect its requested mapping without media or devices.
        for (std::size_t pane = 0; pane < 2; ++pane) {
            app.sources_[pane] = std::make_unique<VideoSource>();
        }
        app.updateHoverControls(window.screenPoint(50, 50));
        require(app.commandPane() == 0, "Pinned bar lost initial pane target");
        // The pointer has never moved, so it is stale and no pane is hot.
        require(app.hoverPane_ == -1, "Stale pointer should not hover a pane");
        require(app.barLayout_.row.visible() && app.barLayout_.row.y < 290.0F,
                "Pinned bar geometry missing");
        app.seekRelative(5.0);
        require(app.clock_.position() == 100.0, "Independent key moved master clock");
        require(app.syncAdjustments_[0] == 5.0 && app.syncAdjustments_[1] == 0.0,
            "Independent key did not seek only first pane");

        app.updateHoverControls(window.screenPoint(350, 50));
        require(app.commandPane() == 1, "Pinned dock kept stale pane target");
        app.seekRelative(5.0);
        require(app.syncAdjustments_[0] == 5.0 && app.syncAdjustments_[1] == 5.0,
            "Independent key sought stale pane");
        require(app.clock_.position() == 100.0, "Second independent key moved master");

        app.updateHoverControls(window.screenPoint(50, 290));
        require(app.commandPane() == 1, "Pointer over the bar selected the video underneath");
        app.soloPane_ = 0;
        require(app.commandPane() == 0, "Solo should override last pointed pane");
    }

    static void captionReplacesTheTitleBar() {
        App app;
        HiddenWindow window;
        app.window_ = app.videoWindow_ = window.window;
        app.layoutMode_ = LayoutMode::SideBySide;
        app.dockProgress_ = 1.0F;
        app.paneChromeAlpha_.fill(1.0F);
        app.sources_[0] = std::make_unique<VideoSource>();
        const POINT middle = window.screenPoint(200, 150);
        app.updateHoverControls(middle);
        const auto& close = app.captionLayout_[CaptionItem::Close];
        require(close.visible() && close.y == 0.0F && close.right() == 400.0F,
                "The caption has no Close button at the top right");
        const POINT closeCenter{static_cast<LONG>(close.x + close.width * 0.5F),
                                static_cast<LONG>(close.y + close.height * 0.5F)};
        auto hit = app.hitTestAt(closeCenter);
        require(hit.kind == OverlayHitKind::CaptionItem && hit.item == static_cast<int>(CaptionItem::Close),
                "Close on the caption is not hit");
        require(app.paneChrome_[0].pill.visible() && app.paneChrome_[0].pill.y >= app.captionInset(),
                "The pane's pill lies under the caption");
        // The strip drags the window through the main window; its buttons
        // and the picture below stay the client's.
        require(app.frameHitTest(window.screenPoint(100, 20)) == HTCAPTION, "The caption does not drag the window");
        require(app.frameHitTest(window.screenPoint(static_cast<int>(close.x) + 5, 20)) == HTCLIENT,
                "A caption button was handed to the window manager");
        require(app.frameHitTest(window.screenPoint(100, 100)) == HTCLIENT, "The picture drags the window");
        // Hidden with the bar, the strip is the picture again.
        app.dockProgress_ = 0.0F;
        require(app.hitTestAt(closeCenter).kind != OverlayHitKind::CaptionItem &&
                app.frameHitTest(window.screenPoint(100, 20)) == HTCLIENT,
                "A hidden caption still takes the pointer");
        // The sheet keeps it up, and starts below it.
        app.settingsOpen_ = true;
        app.settingsAlpha_ = 1.0F;
        app.updateHoverControls(middle);
        require(app.captionAlpha() == 1.0F && app.panelLayout_.header.y == app.captionInset(),
                "The sheet's header is not below the caption");
        hit = app.hitTestAt(closeCenter);
        require(hit.kind == OverlayHitKind::CaptionItem, "The sheet covers the window's Close");
        app.settingsOpen_ = false;
        app.settingsAlpha_ = 0.0F;
        // One pane has nothing to swap with, so the picture drags the window.
        app.updateHoverControls(middle);
        require(app.paneDragMovesWindow(), "One video does not drag the window");
        app.sources_[1] = std::make_unique<VideoSource>();
        app.updateHoverControls(middle);
        require(!app.paneDragMovesWindow(), "Two videos no longer swap by dragging");
        // Fullscreen has no caption.
        app.fullscreen_ = true;
        app.dockProgress_ = 1.0F;
        app.updateHoverControls(middle);
        require(!app.captionLayout_.row.visible() && app.captionAlpha() == 0.0F && app.captionInset() == 0.0F,
                "Fullscreen kept the caption");
        require(app.paneChrome_[0].pill.y == 12.0F, "Fullscreen kept the pill below a caption");
        require(app.frameHitTest(window.screenPoint(100, 20)) == HTCLIENT && !app.paneDragMovesWindow(),
                "Fullscreen drags the window");
        app.fullscreen_ = false;
    }

    // Dragging one picture onto another swaps the panes. The release lets
    // the capture go, and ReleaseCapture sends WM_CAPTURECHANGED back to the
    // video window at once; that handler cancels a drag, so the drop has to
    // be read before it. The fixture routes the message as Windows does.
    static void draggingThePictureSwapsPanes() {
        App app;
        HiddenWindow window;
        app.window_ = app.videoWindow_ = window.window;
        app.instance_ = GetModuleHandleW(nullptr);
        app.layoutMode_ = LayoutMode::SideBySide;
        app.sources_[0] = std::make_unique<VideoSource>();
        app.sources_[1] = std::make_unique<VideoSource>();
        app.paths_[0] = L"C:\\Videos\\left.mp4";
        app.paths_[1] = L"C:\\Videos\\right.mp4";
        CaptureRelay relay(window.window, app);
        RECT client{};
        GetClientRect(window.window, &client);
        app.refreshChromeGeometry(client);
        const auto& left = app.chromeCells_[0];
        const auto& right = app.chromeCells_[1];
        require(left.width > 0.0F && right.width > 0.0F && right.x > left.x, "Premise: two cells side by side");
        const LONG x0 = static_cast<LONG>(left.x + left.width * 0.5F);
        const LONG y0 = static_cast<LONG>(left.y + left.height * 0.5F);
        const LONG x1 = static_cast<LONG>(right.x + right.width * 0.5F);
        const LONG y1 = static_cast<LONG>(right.y + right.height * 0.5F);
        require(app.hitTestAt(POINT{x0, y0}).kind == OverlayHitKind::Video, "Premise: the press lands on the picture");
        require(!app.paneDragMovesWindow(), "Premise: two videos swap rather than move the window");

        app.handleVideoMessage(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x0, y0));
        require(app.dragSourcePane_ == 0 && !app.draggingPane_, "The press did not arm a pane drag");
        require(GetCapture() == window.window, "Premise: the press captured the pointer");
        app.handleVideoMessage(WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x1, y1));
        require(app.draggingPane_ && app.dropTargetPane_ == 1, "Moving onto the other pane did not become a drag");
        app.handleVideoMessage(WM_LBUTTONUP, 0, MAKELPARAM(x1, y1));
        require(relay.captureChanges == 1, "Premise: the release reached WM_CAPTURECHANGED through ReleaseCapture");
        require(app.paths_[0] == L"C:\\Videos\\right.mp4" && app.paths_[1] == L"C:\\Videos\\left.mp4",
                "Dropping one picture on another did not swap the panes");
        require(app.dragSourcePane_ == -1 && !app.draggingPane_ && app.dropTargetPane_ == -1 && GetCapture() == nullptr,
                "The release left drag state behind");

        // Dropped back where it started, nothing moves.
        app.handleVideoMessage(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x0, y0));
        app.handleVideoMessage(WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x1, y1));
        app.handleVideoMessage(WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x0, y0));
        app.handleVideoMessage(WM_LBUTTONUP, 0, MAKELPARAM(x0, y0));
        require(app.paths_[0] == L"C:\\Videos\\right.mp4" && app.paths_[1] == L"C:\\Videos\\left.mp4",
                "A drag released over its own pane swapped something");

        // The pointer taken by another window mid-drag cancels the drag:
        // the release that follows swaps nothing.
        app.handleVideoMessage(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x0, y0));
        app.handleVideoMessage(WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x1, y1));
        require(app.draggingPane_, "Premise: the second drag is under way");
        SetCapture(window.menu);
        require(!app.draggingPane_ && app.dragSourcePane_ == -1, "Losing the capture did not cancel the drag");
        ReleaseCapture();
        app.handleVideoMessage(WM_LBUTTONUP, 0, MAKELPARAM(x1, y1));
        require(app.paths_[0] == L"C:\\Videos\\right.mp4" && app.paths_[1] == L"C:\\Videos\\left.mp4",
                "A cancelled drag still swapped on release");
    }

    static void noticeShowsThenExpires() {
        App app;
        const auto visible = [&] { return !app.noticeText_.empty(); };
        require(!visible() && app.noticeUntil_ == 0, "Notice visible before any action");
        app.showNotice(L"Audio: V1 + V3");
        require(app.noticeText_ == L"Audio: V1 + V3", "Notice text not set");
        const ULONGLONG deadline = app.noticeUntil_;
        require(deadline > 0, "Notice deadline not armed");
        app.expireNotice(deadline - 1);
        require(visible(), "Notice hidden before its deadline");
        app.expireNotice(deadline);
        require(!visible() && app.noticeUntil_ == 0, "Notice did not expire");
        // A second notice re-arms rather than inheriting the old deadline.
        app.showNotice(L"Muted");
        require(visible() && app.noticeUntil_ >= deadline, "Second notice not re-armed");
    }

    static void sessionDropsOldSoloAndPointerSelection() {
        App app;
        // The supported recovery path restores the whole session while
        // deferring devices/media. It exercises applySession itself without
        // opening synthetic paths or calling settings persistence.
        app.deviceRecoveryPending_ = true;
        app.soloPane_ = 1;
        app.lastPointerPane_ = app.hoverPane_ = 1;
        SessionState state;
        state.layout = LayoutMode::SideBySide;
        state.timeline = 42.0;
        state.panes[0].path = L"synthetic-first.mp4";
        state.panes[1].path = L"synthetic-second.mp4";
        state.panes[1].adjustment = 3.5;
        state.panes[1].rate = 1.5;
        state.panes[1].volume = 0.4F;
        state.audioMask = 3;
        app.applySession(state);
        require(app.soloPane_ == -1 && app.lastPointerPane_ == -1 &&
            app.hoverPane_ == -1, "Session retained old transient selection");
        const auto cells = app.currentLayoutCells(400, 300, app.paneAspectRatios());
        require(cells[0].width == 200 && cells[1].width == 200,
            "Old Solo hid restored multi-pane layout");
        require(app.clock_.position() == 42.0 && app.audioMask_ == 3 &&
            app.syncAdjustments_[1] == 3.5 && app.playbackRates_[1] == 1.5 &&
            std::abs(app.audio_.paneVolume(1) - 0.4F) < 0.0001F,
            "Session reset changed persisted playback settings");
    }
};

int singleInstanceAppReceiver(SingleInstance& instance, const SingleInstance::Request& initial,
                              const std::wstring& testNamespace, const std::filesystem::path& directory) {
    return AppRegressionTests::receiveExternalProcess(instance, initial, testNamespace, directory);
}
}

int main(int argc, char** argv) {
    try {
        if (const auto processMode = quaddeck::singleInstanceProcessMode(argc, argv)) return *processMode;
        if (quaddeck::AppRegressionTests::renderLocalMultiPaneFixturesWhenRequested(argc, argv)) {
            std::cout << "Local multipane offscreen UI fixtures rendered\n";
            return 0;
        }
        if (quaddeck::AppRegressionTests::renderEmbyFixturesWhenRequested()) {
            std::cout << "Emby offscreen UI fixtures rendered\n";
            return 0;
        }
        quaddeck::AppRegressionTests::singleSourceUsesOnlyTheMasterTimeline();
        quaddeck::AppRegressionTests::pinnedDockTargetsThePointedPane();
        quaddeck::AppRegressionTests::captionReplacesTheTitleBar();
        quaddeck::AppRegressionTests::draggingThePictureSwapsPanes();
        quaddeck::AppRegressionTests::sessionDropsOldSoloAndPointerSelection();
        quaddeck::AppRegressionTests::nasCachePreferenceIsVisibleAndDeferred();
        quaddeck::AppRegressionTests::rtxVideoTogglesReachPersistence();
        quaddeck::AppRegressionTests::embyItemPreservesTheOtherPanes();
        quaddeck::AppRegressionTests::embyClientStartsAndStopsCleanly();
        quaddeck::AppRegressionTests::embyStateFollowsSignOutAndSessions();
        quaddeck::AppRegressionTests::swapPanesMovesMediaState();
        quaddeck::AppRegressionTests::panelPressSurvivesRowInsertion();
        quaddeck::AppRegressionTests::embyTilesAndPlaylistFollowTheList();
        quaddeck::AppRegressionTests::embyPlaylistPlaysInItsOwnOrder();
        quaddeck::AppRegressionTests::embyLibraryShowsWithoutItsFolders();
        quaddeck::AppRegressionTests::embyMovieAndTelevisionRowsKeepTheirContext();
        quaddeck::AppRegressionTests::embyTelevisionNavigationAndSearchReachEpisodes();
        quaddeck::AppRegressionTests::embyMovieAndEpisodeResumeUsesFreshProgress();
        quaddeck::AppRegressionTests::embyAddAndDedupPreserveOtherPanes();
        quaddeck::AppRegressionTests::embyFullDeckReplacementIsExplicitAndStable();
        quaddeck::AppRegressionTests::embyReportsUseEachPaneRegardlessOfAudio();
        quaddeck::AppRegressionTests::embyResumeAndEndStaySourceLocal();
        quaddeck::AppRegressionTests::embyDetailsPreserveSourceAndPlayback();
        quaddeck::AppRegressionTests::embyLibraryDetailTogglePreservesOriginalActions();
        quaddeck::AppRegressionTests::embyDetailsRequireKnownMovieOrTvLibrary();
        quaddeck::AppRegressionTests::embyMovieDetailActionsHonorFreshProgress();
        quaddeck::AppRegressionTests::embyDetailRepliesCannotReviveOldPagesOrAccounts();
        quaddeck::AppRegressionTests::embySeriesDetailsKeepBrowsingSeparateFromPlayback();
        quaddeck::AppRegressionTests::embySearchAndF5KeepTheirOwnContext();
        quaddeck::AppRegressionTests::embyFirstSignInRetiresOnlyTheCompletedPhase();
        quaddeck::AppRegressionTests::embyBrowserKeepsItselfFresh();
        quaddeck::AppRegressionTests::embyBrowserMarksVideosWithSubtitles();
        quaddeck::AppRegressionTests::embyRandomOrderSurvivesReopening();
        quaddeck::AppRegressionTests::embyNewlyFoundVideosComeFirst();
        quaddeck::AppRegressionTests::embyBrowserDocksBesideAVideo();
        quaddeck::AppRegressionTests::playOrderFollowsTheListAtTheEnd();
        quaddeck::AppRegressionTests::singleVideoFollowsTheMasterTimeline();
        quaddeck::AppRegressionTests::singleBrowserVideoEndsByTheOnlyVideoRule();
        quaddeck::AppRegressionTests::onlyVideoShowsNoTimeFromTheOneBefore();
        quaddeck::AppRegressionTests::localFolderFollowsTheSortKeys();
        quaddeck::AppRegressionTests::localFolderIsTheF6List();
        quaddeck::AppRegressionTests::localProbeFindsTextSubtitleStreams();
        quaddeck::AppRegressionTests::browserLinkedSeeksAlignSourceSeconds();
        quaddeck::AppRegressionTests::independentBrowserPanesHideFalseAggregateTime();
        quaddeck::AppRegressionTests::oneEmbyVideoKeepsItsExactSeekPath();
        quaddeck::AppRegressionTests::localPlayAndAddPreserveOtherPanes();
        quaddeck::AppRegressionTests::externalOpensPreservePlaybackAndDeferInteractions();
        quaddeck::AppRegressionTests::externalReplacementKeepsBatchOrderAndCapturedQueue();
        quaddeck::AppRegressionTests::externalLaunchIntoAnEmptyDeckOpensOneDeck();
        quaddeck::AppRegressionTests::externalSubtitleFilesFindTheirVideo();
        quaddeck::AppRegressionTests::localReplacementKeepsItsSnapshot();
        quaddeck::AppRegressionTests::localQueuesFollowTheirPanesAndEndIndependently();
        quaddeck::AppRegressionTests::localLengthRepliesRetirePressedIndices();
        quaddeck::AppRegressionTests::localThumbnailsAnswerOnTheCallingThread();
        quaddeck::AppRegressionTests::sheetScrollsByDragging();
        quaddeck::AppRegressionTests::rightEdgeOpensTheEmbyBrowser();
        quaddeck::AppRegressionTests::sidecarSubtitlesArriveOffTheWindowThread();
        quaddeck::AppRegressionTests::subtitlesAreChosenForTheViewer();
        quaddeck::AppRegressionTests::localSubtitlesFollowTheLanguageAndTheViewer();
        quaddeck::AppRegressionTests::assSubtitlesAreDrawnWhereTheScriptPutsThem();
        quaddeck::AppRegressionTests::noticeShowsThenExpires();
        std::cout << "App input and session regressions passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
