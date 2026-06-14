#include "jni.h"
#include "jni_wrapper.h"
#include <Windows.h>
#include <consoleapi.h>
#include <cstdio>
#include <libloaderapi.h>
#include <minwindef.h>
#include <winnt.h>
#include <winuser.h>
#include <thread>

void MainThread(HMODULE module)
{
    JavaVM* jvm = nullptr;
    jint result = JNI_GetCreatedJavaVMs(&jvm, 1, nullptr);

    JNIEnv* env = nullptr;
    JavaVMAttachArgs attach_args;
    attach_args.version = JNI_VERSION_1_8;
    jvm->AttachCurrentThread((void**)&env, &attach_args);

    // call function
    jclass clazz = env->FindClass("com/example/MainActivity");
    if(clazz == nullptr){
        printf("Class not found\n");
        MessageBoxA(nullptr, "Error", "Class not found", MB_OK);
        return;
    }
    jmethodID method = env->GetStaticMethodID(clazz, "main", "([Ljava/lang/String;)V");
    env->CallStaticVoidMethod(clazz, method, nullptr);

    while (!GetAsyncKeyState(VK_END)) {
    
    }

    FreeLibrary(module);
}

int WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    static FILE* file = nullptr;
    static std::thread main_thread;

    switch (reason) {
    case DLL_PROCESS_ATTACH:
        AllocConsole();
        freopen_s(&file, "CONOUT$", "w", stdout);
        // start the main thread
        main_thread = std::thread([instance](){
            MainThread(instance);
        });
        if(main_thread.joinable()){
            main_thread.join();
        }
        break;
    case DLL_PROCESS_DETACH: 
        fclose(file);
        FreeConsole();
        break;
    case DLL_THREAD_ATTACH: break;
    case DLL_THREAD_DETACH: break;
    }

    return TRUE;
}

