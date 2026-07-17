//
// Created by Perfare on 2020/7/4.
//

#include "hack.h"
#include "il2cpp_dump.h"
#include "log.h"
#include "xdl.h"
#include <cstring>
#include <cstdio>
#include <unistd.h>
#include <sys/system_properties.h>
#include <dlfcn.h>
#include <jni.h>
#include <thread>
#include <sys/mman.h>
#include <linux/unistd.h>
#include <array>

void scan_and_dump_metadata(const char *game_data_dir);

static void *g_il2cpp_handle = nullptr;
static uint64_t g_meta_addr = 0;
void hack_start(const char *game_data_dir) {
    for (int i = 0; i < 12; i++) {
        void *h = xdl_open("libniolange.so", 0);
        if (h) { g_il2cpp_handle = h; LOGI("libniolange.so loaded, base via xdl"); break; }
        sleep(1);
    }
    scan_and_dump_metadata(game_data_dir);
}

std::string GetLibDir(JavaVM *vms) {
    JNIEnv *env = nullptr;
    vms->AttachCurrentThread(&env, nullptr);
    jclass activity_thread_clz = env->FindClass("android/app/ActivityThread");
    if (activity_thread_clz != nullptr) {
        jmethodID currentApplicationId = env->GetStaticMethodID(activity_thread_clz,
                                                                "currentApplication",
                                                                "()Landroid/app/Application;");
        if (currentApplicationId) {
            jobject application = env->CallStaticObjectMethod(activity_thread_clz,
                                                              currentApplicationId);
            jclass application_clazz = env->GetObjectClass(application);
            if (application_clazz) {
                jmethodID get_application_info = env->GetMethodID(application_clazz,
                                                                  "getApplicationInfo",
                                                                  "()Landroid/content/pm/ApplicationInfo;");
                if (get_application_info) {
                    jobject application_info = env->CallObjectMethod(application,
                                                                     get_application_info);
                    jfieldID native_library_dir_id = env->GetFieldID(
                            env->GetObjectClass(application_info), "nativeLibraryDir",
                            "Ljava/lang/String;");
                    if (native_library_dir_id) {
                        auto native_library_dir_jstring = (jstring) env->GetObjectField(
                                application_info, native_library_dir_id);
                        auto path = env->GetStringUTFChars(native_library_dir_jstring, nullptr);
                        LOGI("lib dir %s", path);
                        std::string lib_dir(path);
                        env->ReleaseStringUTFChars(native_library_dir_jstring, path);
                        return lib_dir;
                    } else {
                        LOGE("nativeLibraryDir not found");
                    }
                } else {
                    LOGE("getApplicationInfo not found");
                }
            } else {
                LOGE("application class not found");
            }
        } else {
            LOGE("currentApplication not found");
        }
    } else {
        LOGE("ActivityThread not found");
    }
    return {};
}

static std::string GetNativeBridgeLibrary() {
    auto value = std::array<char, PROP_VALUE_MAX>();
    __system_property_get("ro.dalvik.vm.native.bridge", value.data());
    return {value.data()};
}

struct NativeBridgeCallbacks {
    uint32_t version;
    void *initialize;

    void *(*loadLibrary)(const char *libpath, int flag);

    void *(*getTrampoline)(void *handle, const char *name, const char *shorty, uint32_t len);

    void *isSupported;
    void *getAppEnv;
    void *isCompatibleWith;
    void *getSignalHandler;
    void *unloadLibrary;
    void *getError;
    void *isPathSupported;
    void *initAnonymousNamespace;
    void *createNamespace;
    void *linkNamespaces;

    void *(*loadLibraryExt)(const char *libpath, int flag, void *ns);
};

bool NativeBridgeLoad(const char *game_data_dir, int api_level, void *data, size_t length) {
    //TODO 等待houdini初始化
    sleep(5);

    auto libart = dlopen("libart.so", RTLD_NOW);
    auto JNI_GetCreatedJavaVMs = (jint (*)(JavaVM **, jsize, jsize *)) dlsym(libart,
                                                                             "JNI_GetCreatedJavaVMs");
    LOGI("JNI_GetCreatedJavaVMs %p", JNI_GetCreatedJavaVMs);
    JavaVM *vms_buf[1];
    JavaVM *vms;
    jsize num_vms;
    jint status = JNI_GetCreatedJavaVMs(vms_buf, 1, &num_vms);
    if (status == JNI_OK && num_vms > 0) {
        vms = vms_buf[0];
    } else {
        LOGE("GetCreatedJavaVMs error");
        return false;
    }

    auto lib_dir = GetLibDir(vms);
    if (lib_dir.empty()) {
        LOGE("GetLibDir error");
        return false;
    }
    if (lib_dir.find("/lib/x86") != std::string::npos) {
        LOGI("no need NativeBridge");
        munmap(data, length);
        return false;
    }

    auto nb = dlopen("libhoudini.so", RTLD_NOW);
    if (!nb) {
        auto native_bridge = GetNativeBridgeLibrary();
        LOGI("native bridge: %s", native_bridge.data());
        nb = dlopen(native_bridge.data(), RTLD_NOW);
    }
    if (nb) {
        LOGI("nb %p", nb);
        auto callbacks = (NativeBridgeCallbacks *) dlsym(nb, "NativeBridgeItf");
        if (callbacks) {
            LOGI("NativeBridgeLoadLibrary %p", callbacks->loadLibrary);
            LOGI("NativeBridgeLoadLibraryExt %p", callbacks->loadLibraryExt);
            LOGI("NativeBridgeGetTrampoline %p", callbacks->getTrampoline);

            int fd = syscall(__NR_memfd_create, "anon", MFD_CLOEXEC);
            ftruncate(fd, (off_t) length);
            void *mem = mmap(nullptr, length, PROT_WRITE, MAP_SHARED, fd, 0);
            memcpy(mem, data, length);
            munmap(mem, length);
            munmap(data, length);
            char path[PATH_MAX];
            snprintf(path, PATH_MAX, "/proc/self/fd/%d", fd);
            LOGI("arm path %s", path);

            void *arm_handle;
            if (api_level >= 26) {
                arm_handle = callbacks->loadLibraryExt(path, RTLD_NOW, (void *) 3);
            } else {
                arm_handle = callbacks->loadLibrary(path, RTLD_NOW);
            }
            if (arm_handle) {
                LOGI("arm handle %p", arm_handle);
                auto init = (void (*)(JavaVM *, void *)) callbacks->getTrampoline(arm_handle,
                                                                                  "JNI_OnLoad",
                                                                                  nullptr, 0);
                LOGI("JNI_OnLoad %p", init);
                init(vms, (void *) game_data_dir);
                return true;
            }
            close(fd);
        }
    }
    return false;
}

void hack_prepare(const char *game_data_dir, void *data, size_t length) {
    LOGI("hack thread: %d", gettid());
    int api_level = android_get_device_api_level();
    LOGI("api level: %d", api_level);

#if defined(__i386__) || defined(__x86_64__)
    if (!NativeBridgeLoad(game_data_dir, api_level, data, length)) {
#endif
        hack_start(game_data_dir);
#if defined(__i386__) || defined(__x86_64__)
    }
#endif
}

#if defined(__arm__) || defined(__aarch64__)

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    auto game_data_dir = (const char *) reserved;
    std::thread hack_thread(hack_start, game_data_dir);
    hack_thread.detach();
    return JNI_VERSION_1_6;
}

#endif
// scan_and_dump_metadata: niolange 가 il2cpp export 심볼 stripped + /proc/self/mem 차단하므로,
// in-process 에서 메모리를 직접 읽어(빠른 memcmp) 복호된 metadata(매직 AF1BB1FA+version)을 덤프.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <unistd.h>
#include <csetjmp>
#include <csignal>
#include "log.h"

static sigjmp_buf g_jmp;
static void segv_handler(int) { siglongjmp(g_jmp, 1); }

static bool scan_region(uint64_t s, uint64_t e, const char *outpath) {
    if (sigsetjmp(g_jmp, 1)) return false;   // 영역 fault -> skip (1회)
    const unsigned char *b=(const unsigned char*)s;
    size_t len=(size_t)(e-s);
    static const unsigned char MG[4]={0xAF,0x1B,0xB1,0xFA};
    for (size_t i=0; i+8<len; i++) {
        if (b[i]==0xAF && b[i+1]==0x1B && b[i+2]==0xB1 && b[i+3]==0xFA) {
            uint32_t ver; memcpy(&ver, b+i+4, 4);
            if (ver>=20 && ver<=40) {
                g_meta_addr=(uint64_t)(b+i);
                size_t avail=len-i; size_t dsize=avail<(size_t)45*1024*1024?avail:(size_t)45*1024*1024;
                FILE *f=fopen(outpath,"wb");
                if (f) { fwrite(b+i,1,dsize,f); fclose(f);
                    LOGI("METADATA DUMPED %zu bytes ver=%u at 0x%llx", dsize, ver, (unsigned long long)(s+i)); }
                return true;
            }
        }
    }
    return false;
}


// dump_region_segv: 한 영역을 페이지 단위 segv-safe 로 파일에 append. 반환=쓴 바이트.
static size_t dump_region_segv(uint64_t start, size_t sz, FILE *f) {
    const size_t PG=4096; unsigned char zero[PG]; memset(zero,0,PG);
    size_t written=0;
    for (size_t off=0; off<sz; off+=PG) {
        const unsigned char *pg=(const unsigned char*)(start+off);
        size_t chunk=(sz-off<PG)?(sz-off):PG;
        if (sigsetjmp(g_jmp,1)) { fwrite(zero,1,chunk,f); written+=chunk; continue; }
        fwrite(pg,1,chunk,f); written+=chunk;
    }
    return written;
}

// dump_all_regions: /proc/self/maps 의 모든 읽기가능 anon(+niolange) 영역을 in-process 로 덤프.
// niolange 는 il2cpp 코드/데이터/registration 을 익명메모리에 언팩하므로, meta_addr 상하 위치를
// 가리지 않고 전부 캡처해야 Il2CppDumper 가 CodeRegistration/MetadataRegistration 을 찾는다.
// manifest(allmem.man: vaddr fileoff size perms path)로 offline 에서 정확한 VA 매핑 ELF 재구성.
static void dump_all_regions(const char *game_data_dir) {
    char binp[512], manp[512], infop[512];
    snprintf(binp,sizeof(binp),"%s/allmem.bin",game_data_dir);
    snprintf(manp,sizeof(manp),"%s/allmem.man",game_data_dir);
    snprintf(infop,sizeof(infop),"%s/dumpinfo.txt",game_data_dir);
    FILE *out=fopen(binp,"wb"); FILE *man=fopen(manp,"w");
    if(!out||!man){ LOGW("allmem open fail"); if(out)fclose(out); if(man)fclose(man); return; }

    uint64_t nio_base=0;
    { FILE *mp=fopen("/proc/self/maps","r"); char l[600];
      while(mp&&fgets(l,sizeof(l),mp)){ if(strstr(l,"libniolange.so")){ sscanf(l,"%llx",(unsigned long long*)&nio_base); break; } }
      if(mp)fclose(mp); }
    FILE *inf=fopen(infop,"w");
    if(inf){ fprintf(inf,"meta_addr=0x%llx\nniolange_base=0x%llx\n",
        (unsigned long long)g_meta_addr,(unsigned long long)nio_base); fclose(inf); }

    struct sigaction sa{},old{}; sa.sa_handler=segv_handler; sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV,&sa,&old); sigaction(SIGBUS,&sa,nullptr);

    FILE *maps=fopen("/proc/self/maps","r");
    char line[600]; uint64_t foff=0; int regions=0;
    while(maps&&fgets(line,sizeof(line),maps)){
        uint64_t s,e; char perms[8]={0}; char rest[512]={0};
        if(sscanf(line,"%llx-%llx %7s %*x %*x:%*x %*u %511[^\n]",
                  (unsigned long long*)&s,(unsigned long long*)&e,perms,rest)<3) continue;
        if(perms[0]!='r') continue;
        char *p=rest; while(*p==' ')p++;
        bool is_anon=(*p==0)||strncmp(p,"[anon",5)==0||strcmp(p,"[heap]")==0||strcmp(p,"[stack]")==0;
        bool is_nio=strstr(p,"niolange")!=nullptr;
        if(!is_anon&&!is_nio) continue;
        uint64_t sz=e-s;
        if(sz==0||sz>(uint64_t)450*1024*1024) continue;
        size_t w=dump_region_segv(s,(size_t)sz,out);
        fprintf(man,"0x%llx %llu %llu %s %s\n",(unsigned long long)s,
            (unsigned long long)foff,(unsigned long long)sz,perms,(*p?p:"[anon]"));
        fflush(man); foff+=w; regions++;
    }
    if(maps)fclose(maps);
    sigaction(SIGSEGV,&old,nullptr);
    fclose(out); fclose(man);
    LOGI("ALL REGIONS DUMPED regions=%d total=%llu meta=0x%llx nio=0x%llx",
        regions,(unsigned long long)foff,(unsigned long long)g_meta_addr,(unsigned long long)nio_base);
}

void scan_and_dump_metadata(const char *game_data_dir) {
    char outpath[512];
    snprintf(outpath,sizeof(outpath),"%s/global-metadata.dat",game_data_dir);
    struct sigaction sa{}, old{}; sa.sa_handler=segv_handler; sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV,&sa,&old); sigaction(SIGBUS,&sa,nullptr);
    for (int attempt=0; attempt<15; attempt++) {
        sleep(1);
        FILE *maps=fopen("/proc/self/maps","r");
        if (!maps) { LOGW("maps open fail (attempt %d)", attempt); continue; }
        char line[512]; bool done=false; int scanned=0;
        while (fgets(line,sizeof(line),maps)) {
            uint64_t s,e; char perms[8];
            if (sscanf(line,"%llx-%llx %7s",(unsigned long long*)&s,(unsigned long long*)&e,perms)!=3) continue;
            if (perms[0]!='r') continue;
            uint64_t sz=e-s; if (sz>(uint64_t)768*1024*1024) continue;
            scanned++;
            if (scan_region(s,e,outpath)) { done=true; break; }
        }
        fclose(maps);
        if (done) {
            LOGI("scan SUCCESS attempt=%d", attempt);
            sigaction(SIGSEGV,&old,nullptr);
            dump_all_regions(game_data_dir);
            LOGI("ALL DONE");
            return;
        }
        LOGI("attempt %d scanned %d regions, no magic", attempt, scanned);
    }
    sigaction(SIGSEGV,&old,nullptr);
    LOGW("metadata magic not found (on-demand decryption?)");
}
