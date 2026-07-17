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


// dump_module_memory: 런타임에 복호/언팩된 libniolange.so 의 메모리 영역(모든 세그먼트)을
// base~end 통째로 덤프한다. 정적 APK 의 libniolange.so 는 packed 라 CodeRegistration 을 못 찾지만,
// 런타임 메모리 이미지는 언팩돼 있어 Il2CppDumper/Inspector 가 등록부를 찾을 수 있다.
static void dump_module_memory(const char *game_data_dir) {
    if (!g_il2cpp_handle) { LOGW("no il2cpp handle"); return; }
    xdl_info_t xi; memset(&xi, 0, sizeof(xi));
    if (xdl_info(g_il2cpp_handle, XDL_DI_DLINFO, &xi) != 0 || xi.dli_fbase == nullptr) { LOGW("xdl_info fail"); return; }
    uint64_t base = (uint64_t)xi.dli_fbase, end = base;
    FILE *maps = fopen("/proc/self/maps", "r");
    if (maps) { char line[512]; while (fgets(line, sizeof(line), maps)) {
        uint64_t s2,e2; if (sscanf(line,"%llx-%llx",(unsigned long long*)&s2,(unsigned long long*)&e2)!=2) continue;
        if (s2 >= base && s2 <= end + 0x200000) { if (e2 > end) end = e2; } } fclose(maps); }
    if (end <= base) end = base + (uint64_t)200*1024*1024;
    size_t total = (size_t)(end - base);
    if (total > (size_t)400*1024*1024) total = (size_t)400*1024*1024;
    char outp[512]; snprintf(outp,sizeof(outp),"%s/libniolange_dump.so",game_data_dir);
    FILE *f=fopen(outp,"wb"); if(!f){LOGW("module dump open fail");return;}
    const size_t PG=4096; unsigned char zero[PG]; memset(zero,0,PG);
    struct sigaction sa{},old{}; sa.sa_handler=segv_handler; sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV,&sa,&old); sigaction(SIGBUS,&sa,nullptr);
    for(size_t off=0; off<total; off+=PG){ const unsigned char *pg=(const unsigned char*)(base+off);
        if(sigsetjmp(g_jmp,1)){ fwrite(zero,1,PG,f); continue; } fwrite(pg,1,PG,f); }
    sigaction(SIGSEGV,&old,nullptr); fclose(f);
    LOGI("MODULE DUMPED base=0x%llx size=%zu -> %s", (unsigned long long)base, total, outp);
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
        if (done) { LOGI("scan SUCCESS attempt=%d", attempt); sigaction(SIGSEGV,&old,nullptr); dump_module_memory(game_data_dir); return; }
        LOGI("attempt %d scanned %d regions, no magic", attempt, scanned);
    }
    sigaction(SIGSEGV,&old,nullptr);
    LOGW("metadata magic not found (on-demand decryption?)");
}
