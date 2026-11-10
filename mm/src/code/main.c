#ifdef _WIN32
#include <Windows.h>
#include <stdio.h>
#include <locale.h>
#endif

#include "audiomgr.h"
#include "fault.h"
#include "idle.h"
#include "irqmgr.h"
#include "padmgr.h"
#include "scheduler.h"
#include "CIC6105.h"
#include "stack.h"
#include "stackcheck.h"
#include "BenPort.h"
#include "2s2h/Android/StartupLog.h"
#include <libultraship/bridge/crashhandlerbridge.h>

// Variables are put before most headers as a hacky way to bypass bss reordering
OSMesgQueue sSerialEventQueue;
OSMesg sSerialMsgBuf[1];
uintptr_t gSegments[NUM_SEGMENTS];
SchedContext gSchedContext;
IrqMgrClient sIrqClient;
OSMesgQueue sIrqMgrMsgQueue;
OSMesg sIrqMgrMsgBuf[60];
OSThread gGraphThread;
STACK(sGraphStack, 0x1800);
STACK(sSchedStack, 0x600);
STACK(sAudioStack, 0x800);
STACK(sPadMgrStack, 0x500);
StackEntry sGraphStackInfo;
StackEntry sSchedStackInfo;
StackEntry sAudioStackInfo;
StackEntry sPadMgrStackInfo;
AudioMgr sAudioMgr;
static s32 sBssPad;
PadMgr gPadMgr;

#include "main.h"
#include "buffers.h"
#include "global.h"
#include "system_heap.h"
#include "z64thread.h"

s32 gScreenWidth = SCREEN_WIDTH;
s32 gScreenHeight = SCREEN_HEIGHT;
size_t gSystemHeapSize = 0;

void InitOTR();
void Heaps_Free(void);

#ifdef __ANDROID__
#include <jni.h>
#include <stdbool.h>
#include <SDL2/SDL.h>
// MainActivity copies and checks the port's files on first run and after an update; the game must not start
// before that. The wait ends either way: true when the files are ready, false when the game must not start (the
// person left before the files were ready, the app closed, or the wait timed out).
static bool WaitForJavaSetup(void) {
    JNIEnv* env = SDL_AndroidGetJNIEnv();
    jclass mainActivityClass = (*env)->FindClass(env, "com/dishii/mm/MainActivity");
    if (mainActivityClass == NULL) {
        (*env)->ExceptionClear(env);
        return false;
    }
    jmethodID waitMethod = (*env)->GetStaticMethodID(env, mainActivityClass, "waitForSetupFromNative", "()Z");
    jboolean ready = JNI_FALSE;
    if (waitMethod != NULL) {
        ready = (*env)->CallStaticBooleanMethod(env, mainActivityClass, waitMethod);
    }
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        ready = JNI_FALSE;
    }
    (*env)->DeleteLocalRef(env, mainActivityClass);
    return ready == JNI_TRUE;
}
#endif

#ifdef __GNUC__
#define SDL_main main
#endif

#ifdef __ANDROID__
// SDLActivity looks up SDL_main (SDL_main.h maps main back to it) and expects an int result.
int SDL_main(int argc, char** argv /* void* arg*/) {
    StartupLog_Init();
    StartupLog_Step("Esperando a preparação dos arquivos (Java)");
    if (!WaitForJavaSetup()) {
        StartupLog_Step("Preparação dos arquivos cancelada: o jogo não inicia");
        return 0;
    }
    StartupLog_Step("Arquivos prontos");
#else
void SDL_main(int argc, char** argv /* void* arg*/) {
#endif
    intptr_t fb;
    intptr_t sysHeap;
    s32 exit;
    s16* msg;

// Attach console for windows so we can conditionally display it when running the extractor
#ifdef _WIN32
    AllocConsole();
    (void)freopen("CONIN$", "r", stdin);
    (void)freopen("CONOUT$", "w", stdout);
    (void)freopen("CONOUT$", "w", stderr);
#ifndef _DEBUG
    ShowWindow(GetConsoleWindow(), SW_HIDE);
#endif
    // Allow non-ascii characters for Windows
    setlocale(LC_ALL, ".UTF8");
#endif // _WIN32

    InitOTR();
    StartupLog_Step("InitOTR terminou");
    CrashHandlerRegisterCallback(CrashHandler_PrintExt);
    Heaps_Alloc();

    gScreenWidth = SCREEN_WIDTH;
    gScreenHeight = SCREEN_HEIGHT;

    Nmi_Init();
    Fault_Init();
    Check_RegionIsSupported();
    Check_ExpansionPak();
    sysHeap = gSystemHeap;
    // fb = FRAMEBUFFERS_START_ADDR;
    // gSystemHeapSize = fb - sysHeap;
    SystemHeap_Init(sysHeap, SYSTEM_HEAP_SIZE);

    Regs_Init();

    R_ENABLE_ARENA_DBG = 0;

    osCreateMesgQueue(&sSerialEventQueue, sSerialMsgBuf, ARRAY_COUNT(sSerialMsgBuf));
    osSetEventMesg(OS_EVENT_SI, &sSerialEventQueue, OS_MESG_PTR(NULL));

    osCreateMesgQueue(&sIrqMgrMsgQueue, sIrqMgrMsgBuf, ARRAY_COUNT(sIrqMgrMsgBuf));
    PadMgr_Init(&sSerialEventQueue, &gIrqMgr, Z_THREAD_ID_PADMGR, Z_PRIORITY_PADMGR, STACK_TOP(sPadMgrStack));

    AudioMgr_Init(&sAudioMgr, STACK_TOP(sAudioStack), Z_PRIORITY_AUDIOMGR, Z_THREAD_ID_AUDIOMGR, &gSchedContext,
                  &gIrqMgr);
#if 0
    StackCheck_Init(&sSchedStackInfo, sSchedStack, STACK_TOP(sSchedStack), 0, 0x100, "sched");
    Sched_Init(&gSchedContext, STACK_TOP(sSchedStack), Z_PRIORITY_SCHED, gViConfigModeType, 1, &gIrqMgr);

    CIC6105_AddRomInfoFaultPage();

    IrqMgr_AddClient(&gIrqMgr, &sIrqClient, &sIrqMgrMsgQueue);

    StackCheck_Init(&sAudioStackInfo, sAudioStack, STACK_TOP(sAudioStack), 0, 0x100, "audio");
    AudioMgr_Init(&sAudioMgr, STACK_TOP(sAudioStack), Z_PRIORITY_AUDIOMGR, Z_THREAD_ID_AUDIOMGR, &gSchedContext,
                  &gIrqMgr);

    StackCheck_Init(&sPadMgrStackInfo, sPadMgrStack, STACK_TOP(sPadMgrStack), 0, 0x100, "padmgr");

    AudioMgr_Unlock(&sAudioMgr);
    StackCheck_Init(&sGraphStackInfo, sGraphStack, STACK_TOP(sGraphStack), 0, 0x100, "graph");
    osCreateThread(&gGraphThread, Z_THREAD_ID_GRAPH, Graph_ThreadEntry, NULL, STACK_TOP(sGraphStack), Z_PRIORITY_GRAPH);
    osStartThread(&gGraphThread);
#endif

    Graph_ThreadEntry(0);

    exit = false;

    while (!exit) {
        msg = NULL;
        osRecvMesg(&sIrqMgrMsgQueue, (OSMesg*)&msg, OS_MESG_BLOCK);
        if (msg == NULL) {
            break;
        }

        switch (*msg) {
            case OS_SC_PRE_NMI_MSG:
                Nmi_SetPrenmiStart();
                break;

            case OS_SC_NMI_MSG:
                exit = true;
                break;
        }
    }

    IrqMgr_RemoveClient(&gIrqMgr, &sIrqClient);
    osDestroyThread(&gGraphThread);

    DeinitOTR();

#ifdef _WIN32
    FreeConsole();
#endif
    Heaps_Free();
#ifdef __ANDROID__
    return 0;
#endif
}
