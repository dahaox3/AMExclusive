// Explicit controlled replay of REAL Apple candidate arrays. Include the
// production translation unit so the actual Create/ApplyTree callbacks run,
// while calling them directly on temporary filters without installing hooks.
#include "../src/LosslessQualityLock.cpp"

namespace {
std::wstring probeLog;
void ProbeLog(const std::wstring& text) { probeLog += text + L"\n"; }
struct Objects {
    void* objects[12]{}; unsigned count{};
    void* Keep(void* object) { objects[count++]=object; return object; }
    ~Objects() { while(count) if(auto object=objects[--count]) ammod::quality::release(object); }
};
}

extern "C" __declspec(dllexport) const wchar_t* SourceSelectionProbeLog() { return probeLog.c_str(); }
extern "C" __declspec(dllexport) int SelectAppleSources(void* input,unsigned limit,void** output) {
    using namespace ammod::quality;
    probeLog.clear(); if(output) *output=nullptr;
    if(!output || !IsSourceSampleRateLimit(limit)) return -1;
    auto media=GetModuleHandleW(L"CoreMedia.dll"), foundation=GetModuleHandleW(L"CoreFoundation.dll");
    if(!media || !foundation) return -2;
#define BIND(variable,module,name) variable=reinterpret_cast<decltype(variable)>(GetProcAddress(module,name))
    BIND(maxSampleRate,media,"FigAlternateGetMaxAudioSampleRate");
    BIND(arrayCreate,foundation,"CFArrayCreate"); BIND(arrayValue,foundation,"CFArrayGetValueAtIndex");
    BIND(arrayCount,foundation,"CFArrayGetCount"); BIND(dataCreate,foundation,"CFDataCreateMutable");
    BIND(dataSetLength,foundation,"CFDataSetLength"); BIND(dataBytes,foundation,"CFDataGetMutableBytePtr");
    BIND(dataLength,foundation,"CFDataGetLength"); BIND(release,foundation,"CFRelease");
    BIND(retain,foundation,"CFRetain"); BIND(typeId,foundation,"CFGetTypeID");
    BIND(arrayTypeId,foundation,"CFArrayGetTypeID"); BIND(dataTypeId,foundation,"CFDataGetTypeID");
    BIND(originalCreate,media,"FigSimpleAlternateFilterCreate");
#undef BIND
    arrayCallbacks=GetProcAddress(foundation,"kCFTypeArrayCallBacks");
    if(!maxSampleRate || !arrayCreate || !arrayCount || !originalCreate ||
       !input || typeId(input)!=arrayTypeId()) return -3;
    const auto* base=reinterpret_cast<const unsigned char*>(media);
    const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    verifiedBuild=nullptr;
    for(const auto& build:ammod::apple_private::core_media_filter::kBuilds)
        if(build.timestamp && build.timestamp==nt->FileHeader.TimeDateStamp &&
           build.imageSize==nt->OptionalHeader.SizeOfImage) verifiedBuild=&build;
    if(!verifiedBuild) return -4; // Explicit replay fixture only supports the verified 1.1792 image.
    mediaBase=reinterpret_cast<std::uintptr_t>(media); writeLog=ProbeLog;
    SetEnabled(true); SetMaximumSourceSampleRate(limit);
    auto number=reinterpret_cast<void*(__cdecl*)(void*,int,const void*)>(GetProcAddress(foundation,"CFNumberCreate"));
    auto subtype=reinterpret_cast<SubtypeFactory>(GetProcAddress(media,"FigAlternateAllowableMediaSubtypeFilterCreate"));
    auto treeCreate=reinterpret_cast<int(__cdecl*)(void*,void**)>(GetProcAddress(media,"FigAlternateFilterTreeCreate"));
    auto insert=reinterpret_cast<int(__cdecl*)(void*,void*,unsigned)>(GetProcAddress(media,"FigAlternateFilterTreeInsertLeaf"));
    auto treeFilter=reinterpret_cast<int(__cdecl*)(void*,void*,unsigned,void**)>(GetProcAddress(media,"FigAlternateFilterTreeCreateFilter"));
    Objects objects;
    const int alac=0x616c6163,qlac=0x716c6163;
    const void* codecs[]{objects.Keep(number(nullptr,3,&alac)),objects.Keep(number(nullptr,3,&qlac))};
    auto allowed=objects.Keep(arrayCreate(nullptr,codecs,2,arrayCallbacks));
    void* native{};
    if(subtype(nullptr,allowed,nullptr,&native)!=0 || !native) return -5;
    objects.Keep(native);
    Callbacks callbacks{};
    std::memcpy(&callbacks,static_cast<char*>(native)+0x30,sizeof(callbacks));
    void* context=*reinterpret_cast<void**>(static_cast<char*>(native)+0x68);
    void* name=*reinterpret_cast<void**>(GetProcAddress(media,"kFigAlternateFilterName_AllowableMediaSubtypes"));
    void* strict{};
    creatingStrictFilter=true; creatingStrictTier=20; wrappedStrictFilter=false;
    const auto created=Create(nullptr,name,0x375,&callbacks,context,&strict);
    creatingStrictFilter=false; creatingStrictTier=0;
    if(created!=0 || !strict) return -6;
    objects.Keep(strict);
    void* tree{}; if(treeCreate(nullptr,&tree)!=0 || !tree) return -7;
    objects.Keep(tree); if(insert(tree,strict,0x375)!=0) return -8;
    void* filter{}; if(treeFilter(tree,name,0,&filter)!=0 || !filter) return -9;
    objects.Keep(filter);
    auto table=*reinterpret_cast<char**>(static_cast<char*>(filter)+0x18);
    auto methods=*reinterpret_cast<char**>(table+0x10);
    originalTreeApply=*reinterpret_cast<TreeApply*>(methods+0x20);
    return ApplyTree(filter,input,output,nullptr);
}
