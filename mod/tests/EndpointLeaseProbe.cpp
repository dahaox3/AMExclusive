// Explicit silence-only hardware regression. Never run by unattended CTest.
#include "audio_v2/AudioCoreCoordinator.h"
#include <functiondiscoverykeys_devpkey.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>

using namespace ammod::audio_v2;
void Require(bool ok, const char* stage) { if(!ok) throw std::runtime_error(stage); }
struct Silence final : IIntegerPcmSource {
    HRESULT Fill(std::int32_t* data,std::uint32_t frames,std::uint32_t& written,bool& eos) noexcept override {
        std::memset(data,0,frames*2*sizeof(*data)); written=frames; eos=false; return S_OK;
    }
};
struct VideoClient {
    IMMDeviceEnumerator* enumerator{}; IMMDevice* device{};
    IAudioClient* client{}; IAudioRenderClient* render{};
    ~VideoClient() {
        if(client) client->Stop();
        if(render) render->Release(); if(client) client->Release();
        if(device) device->Release(); if(enumerator) enumerator->Release();
    }
    void Open(const wchar_t* endpoint) {
        Require(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator))),"video enumerator");
        Require(SUCCEEDED(enumerator->GetDevice(endpoint,&device)),"video endpoint");
        Require(SUCCEEDED(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(&client))),"video activate");
        WAVEFORMATEX* format{}; Require(SUCCEEDED(client->GetMixFormat(&format)),"video format");
        const auto initialized=client->Initialize(AUDCLNT_SHAREMODE_SHARED,0,1000000,0,format,nullptr);
        CoTaskMemFree(format); Require(SUCCEEDED(initialized),"video shared Initialize after music pause");
        Require(SUCCEEDED(client->GetService(IID_PPV_ARGS(&render))),"video render service");
        UINT32 frames{}; BYTE* data{};
        Require(SUCCEEDED(client->GetBufferSize(&frames)),"video buffer size");
        Require(SUCCEEDED(render->GetBuffer(frames,&data)),"video get buffer");
        Require(SUCCEEDED(render->ReleaseBuffer(frames,AUDCLNT_BUFFERFLAGS_SILENT)),"video release buffer");
        Require(SUCCEEDED(client->Start()),"video shared Start");
        Sleep(30);
    }
};

int wmain(int argc,wchar_t**argv) {
    const auto initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    if(FAILED(initialized)) return 2;
    IMMDeviceEnumerator* enumerator{};
    if(FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator)))) return 2;
    if(argc==2 && wcscmp(argv[1],L"--list-devices")==0) {
        IMMDeviceCollection* devices{}; enumerator->EnumAudioEndpoints(eRender,DEVICE_STATE_ACTIVE,&devices);
        UINT count{}; if(devices) devices->GetCount(&count);
        for(UINT i=0;i<count;++i) {
            IMMDevice* device{}; LPWSTR id{}; IPropertyStore* properties{}; PROPVARIANT name{};
            devices->Item(i,&device); device->GetId(&id); device->OpenPropertyStore(STGM_READ,&properties);
            if(properties) properties->GetValue(PKEY_Device_FriendlyName,&name);
            std::wprintf(L"%s | %s\n",id,name.vt==VT_LPWSTR?name.pwszVal:L"");
            PropVariantClear(&name); if(properties) properties->Release(); CoTaskMemFree(id); device->Release();
        }
        if(devices) devices->Release(); enumerator->Release(); CoUninitialize(); return 0;
    }
    if(argc!=3 || wcscmp(argv[1],L"--run-on-endpoint")!=0) {
        std::puts("Use --list-devices or --run-on-endpoint <ID>; stop other playback on that endpoint first.");
        enumerator->Release(); CoUninitialize(); return 2;
    }
    enumerator->Release();
    try {
      for(const auto sampleRate : {48000u,44100u,96000u}) {
        AudioCoreCoordinator music;
        AudioCoreCoordinatorConfig config{};
        const std::uint16_t bits=sampleRate==48000?16:24;
        config.sink.format={sampleRate,2,bits,bits,PcmEncoding::SignedInteger,true};
        config.sink.allowResampling=sampleRate!=48000;
        config.sink.endpointId=argv[2]; config.sink.periodFrames=sampleRate/50; config.queueCapacity=96;
        Require(SUCCEEDED(music.Enable(config)),"music enable");
        float samples[2048]{};
        for(unsigned i=0;i<64;++i) Require(music.PushP0Interleaved(i*1024,samples,1024,false,false),"prebuffer");
        Require(SUCCEEDED(music.OpenEndpoint()) && SUCCEEDED(music.Start()),"initial music output");
        Sleep(30);
        for(unsigned cycle=0;cycle<3;++cycle) {
            music.PauseEndpoint();
            Require(music.EndpointReleased() && music.SinkState()==WasapiSinkState::Closed,"pause releases physical endpoint");
            Require(SUCCEEDED(music.Sink().Stats().lastError),"pause drains submitted PCM before releasing device");
            const auto retained=music.PendingSourceFrames();
            Require(retained>1920,"music queue retained");
            {
                VideoClient video; video.Open(argv[2]);
                Require(music.PendingSourceFrames()==retained,"video does not consume queued music");
                // Browsers may retain a shared stream when returning to music.
                const auto resume=music.ResumeEndpoint();
                if(SUCCEEDED(resume)) { Sleep(10); music.PauseEndpoint(); }
                else Require(music.PendingSourceFrames()==retained,"shared-client contention preserves queued music");
            }
            const auto beforeBusy=music.PendingSourceFrames();
            // Another exclusive application still owns the device on return.
            {
                WasapiExclusiveSink other;
                auto otherConfig=config.sink;
                Require(SUCCEEDED(other.Open(otherConfig)),"other player acquires released endpoint");
                Require(FAILED(music.ResumeEndpoint()),"busy endpoint is reported on resume");
                Require(music.EndpointReleased() && music.PendingSourceFrames()==beforeBusy,"busy retry retains source and releases failed client");
                other.Close();
            }
            // No fresh producer callback is supplied: retained PCM must resume.
            Require(SUCCEEDED(music.ResumeEndpoint()),"reacquire after video/exclusive application");
            Sleep(25);
            Require(music.Sink().Stats().submittedFrames>0 && music.Sink().Stats().startsBeforeFirstBuffer==0,"resume primes and submits music");
        }
        music.PauseEndpoint(); music.Disable();
        std::printf("PASS: %u Hz, %u bits; pause releases endpoint; shared video and another exclusive player; retained-queue resume (3 cycles)\n",sampleRate,bits);
      }
    } catch(const std::exception& error) {
        std::fprintf(stderr,"FAIL: %s\n",error.what()); CoUninitialize(); return 1;
    }
    CoUninitialize(); return 0;
}
