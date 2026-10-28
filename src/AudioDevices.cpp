#include "AudioDevices.hpp"

#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>

#include <algorithm>

namespace quaddeck {

namespace {

// PKEY_Device_FriendlyName, as functiondiscoverykeys_devpkey.h has it; that
// header needs what WIN32_LEAN_AND_MEAN leaves out.
const PROPERTYKEY kFriendlyName{{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

}  // namespace

// Windows calls this on a thread of its own and must not be kept waiting:
// it only raises the flag the window's tick reads.
class AudioDevices::Listener final : public IMMNotificationClient {
public:
    explicit Listener(std::atomic<bool>& changed) : changed_(changed) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --references_;
        if (left == 0) delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override {
        if (!object) return E_POINTER;
        if (id == __uuidof(IUnknown) || id == __uuidof(IMMNotificationClient)) {
            *object = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return notify(); }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return notify(); }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return notify(); }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        return flow == eRender && role == eMultimedia ? notify() : S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY key) override {
        // A renamed output shows its new name; the many other properties
        // that change as a device runs are no news.
        const bool renamed = key.pid == kFriendlyName.pid &&
                             IsEqualGUID(key.fmtid, kFriendlyName.fmtid);
        return renamed ? notify() : S_OK;
    }

private:
    HRESULT notify() {
        changed_.store(true);
        return S_OK;
    }

    std::atomic<ULONG> references_{1};
    std::atomic<bool>& changed_;
};

AudioDevices::~AudioDevices() { stop(); }

void AudioDevices::stop() {
    if (enumerator_ && listener_) enumerator_->UnregisterEndpointNotificationCallback(listener_);
    if (listener_) listener_->Release();
    if (enumerator_) enumerator_->Release();
    listener_ = nullptr;
    enumerator_ = nullptr;
}

bool AudioDevices::start() {
    if (enumerator_) return true;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator_)))) {
        enumerator_ = nullptr;
        return false;
    }
    listener_ = new Listener(changed_);
    if (FAILED(enumerator_->RegisterEndpointNotificationCallback(listener_))) {
        listener_->Release();
        listener_ = nullptr;
    }
    return true;
}

std::vector<AudioDeviceInfo> AudioDevices::outputs() const {
    std::vector<AudioDeviceInfo> found;
    if (!enumerator_) return found;
    IMMDeviceCollection* collection = nullptr;
    if (FAILED(enumerator_->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))) return found;
    UINT count = 0;
    collection->GetCount(&count);
    for (UINT index = 0; index < count; ++index) {
        IMMDevice* device = nullptr;
        if (FAILED(collection->Item(index, &device))) continue;
        AudioDeviceInfo info;
        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id)) && id) {
            info.id = id;
            CoTaskMemFree(id);
        }
        IPropertyStore* properties = nullptr;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
            PROPVARIANT name;
            PropVariantInit(&name);
            if (SUCCEEDED(properties->GetValue(kFriendlyName, &name)) && name.vt == VT_LPWSTR &&
                name.pwszVal) {
                info.name = name.pwszVal;
            }
            PropVariantClear(&name);
            properties->Release();
        }
        device->Release();
        if (info.id.empty()) continue;
        if (info.name.empty()) info.name = L"Audio output";
        found.push_back(std::move(info));
    }
    collection->Release();
    std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return found;
}

std::wstring AudioDevices::defaultOutput() const {
    if (!enumerator_) return {};
    IMMDevice* device = nullptr;
    if (FAILED(enumerator_->GetDefaultAudioEndpoint(eRender, eMultimedia, &device))) return {};
    std::wstring result;
    LPWSTR id = nullptr;
    if (SUCCEEDED(device->GetId(&id)) && id) {
        result = id;
        CoTaskMemFree(id);
    }
    device->Release();
    return result;
}

}  // namespace quaddeck
