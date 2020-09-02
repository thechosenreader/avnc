/*
 * Copyright (c) 2020  Gaurav Ujjwal.
 *
 * SPDX-License-Identifier:  GPL-3.0-or-later
 *
 * See COPYING.txt for more details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <jni.h>
#include <android/log.h>
#include <GLES2/gl2.h>
#include <rfb/rfbclient.h>

#include "keymap.h"

/******************************************************************************
 * Logging
 *****************************************************************************/
static const char *TAG = "NativeVnc";

struct Logger {
    static void i(const char *fmt, ...) {
        va_list args;
        va_start(args, fmt);
        __android_log_vprint(ANDROID_LOG_INFO, TAG, fmt, args);
        va_end(args);
    }

    static void w(const char *fmt, ...) {
        va_list args;
        va_start(args, fmt);
        __android_log_vprint(ANDROID_LOG_WARN, TAG, fmt, args);
        va_end(args);
    }

    static void e(const char *fmt, ...) {
        va_list args;
        va_start(args, fmt);
        __android_log_vprint(ANDROID_LOG_ERROR, TAG, fmt, args);
        va_end(args);
    }
};


/******************************************************************************
 * Library Initialization
 *****************************************************************************/

struct JniContext {
    JavaVM *vm;                     //JVM Instance
    jclass managedCls;              //Managed `VncClient` class
    jmethodID cbFramebufferUpdated; //Cached reference to managed callback

    JNIEnv *getEnv() {
        JNIEnv *env = nullptr;

        if (vm != nullptr && vm->GetEnv((void **) &env, JNI_VERSION_1_6) == JNI_OK)
            return env;

        return nullptr; //Should not happen
    }
};

static JniContext context{};

/**
 * Called when our library is loaded.
 */
JNIEXPORT jint
JNI_OnLoad(JavaVM *vm, void *unused) {
    context.vm = vm;

    if (context.getEnv() == nullptr)
        return JNI_ERR;

    return JNI_VERSION_1_6;
}

JNIEXPORT void
JNI_OnUnload(JavaVM *vm, void *reserved) {
    if (context.managedCls != nullptr)
        context.getEnv()->DeleteGlobalRef(context.managedCls);
}


extern "C"
JNIEXPORT void JNICALL
Java_com_gaurav_avnc_vnc_VncClient_initLibrary(JNIEnv *env, jclass clazz) {
    context.managedCls = (jclass) env->NewGlobalRef(clazz);
    context.cbFramebufferUpdated = env->GetMethodID(clazz, "cbFinishedFrameBufferUpdate", "()V");
    //TODO: Cache more method IDs so we don't have to repeatedly search them

    rfbClientLog = &Logger::i;
    rfbClientErr = &Logger::e;
}

/******************************************************************************
 * Utilities
 *****************************************************************************/

/**
 * Returns a native copy of the given jstring.
 * Caller is responsible for releasing the memory.
 */
static char *getNativeStrCopy(JNIEnv *env, jstring jStr) {
    const char *cStr = env->GetStringUTFChars(jStr, nullptr);
    char *str = strdup(cStr);
    env->ReleaseStringUTFChars(jStr, cStr);
    return str;
}

/**
 * Returns reference to managed `VncClient` associated with given rfbClient.
 */
static jobject getManagedClient(rfbClient *client) {
    return (jobject) rfbClientGetClientData(client, context.managedCls);
}

/**
 * Associate given rfbClient with a managed `VncClient`.
 */
static void setManagedClient(rfbClient *client, jobject managedClient) {
    rfbClientSetClientData(client, context.managedCls, managedClient);
}

/******************************************************************************
 * rfbClient Callbacks
 *****************************************************************************/

static char *onGetPassword(rfbClient *client) {
    auto obj = getManagedClient(client);
    auto env = context.getEnv();
    auto cls = context.managedCls;

    auto mid = env->GetMethodID(cls, "cbGetPassword", "()Ljava/lang/String;");
    auto jPassword = (jstring) env->CallObjectMethod(obj, mid);

    return getNativeStrCopy(env, jPassword);
}

static rfbCredential *onGetCredential(rfbClient *client, int credentialType) {
    if (credentialType != rfbCredentialTypeUser) {
        //Only user credentials (i.e. username & password) are currently supported
        rfbClientErr("Unsupported credential type requested");
        return nullptr;
    }

    auto obj = getManagedClient(client);
    auto env = context.getEnv();
    auto cls = context.managedCls;

    //Retrieve credentials
    jmethodID mid = env->GetMethodID(cls, "cbGetCredential",
                                     "()Lcom/gaurav/avnc/vnc/UserCredential;");
    jobject jCredential = env->CallObjectMethod(obj, mid);
    if (jCredential == nullptr) {
        return nullptr;
    }

    //Extract username & password
    auto jCredentialCls = env->GetObjectClass(jCredential);
    auto usernameField = env->GetFieldID(jCredentialCls, "username", "Ljava/lang/String;");
    auto jUsername = env->GetObjectField(jCredential, usernameField);

    auto passwordField = env->GetFieldID(jCredentialCls, "password", "Ljava/lang/String;");
    auto jPassword = env->GetObjectField(jCredential, passwordField);

    //Create native rfbCredential
    auto credential = (rfbCredential *) malloc(sizeof(rfbCredential));
    credential->userCredential.username = getNativeStrCopy(env, (jstring) jUsername);
    credential->userCredential.password = getNativeStrCopy(env, (jstring) jPassword);

    return credential;
}

static void onBell(rfbClient *client) {
    auto obj = getManagedClient(client);
    auto env = context.getEnv();
    auto cls = context.managedCls;

    jmethodID mid = env->GetMethodID(cls, "cbBell", "()V");
    env->CallVoidMethod(obj, mid);
}

static void onGotXCutText(rfbClient *client, const char *text, int len) {
    auto obj = getManagedClient(client);
    auto env = context.getEnv();
    auto cls = context.managedCls;

    jmethodID mid = env->GetMethodID(cls, "cbGotXCutText", "(Ljava/lang/String;)V");
    jstring jText = env->NewStringUTF(text);
    env->CallVoidMethod(obj, mid, jText);
    env->DeleteLocalRef(jText);
}

static rfbBool onHandleCursorPos(rfbClient *client, int x, int y) {
    auto obj = getManagedClient(client);
    auto env = context.getEnv();
    auto cls = context.managedCls;

    jmethodID mid = env->GetMethodID(cls, "cbHandleCursorPos", "(II)Z");
    jboolean result = env->CallBooleanMethod(obj, mid, x, y);

    return result == JNI_TRUE ? TRUE : FALSE;
}

static void onFinishedFrameBufferUpdate(rfbClient *client) {
    auto obj = getManagedClient(client);
    auto env = context.getEnv();

    env->CallVoidMethod(obj, context.cbFramebufferUpdated);
}

/**
 * We need to use our own allocator to know when frame size has changed.
 */
static rfbBool onMallocFrameBuffer(rfbClient *client) {

    if (client->frameBuffer)
        free(client->frameBuffer);

    auto allocSize = (uint64_t) client->width * client->height * client->format.bitsPerPixel / 8;

    if (allocSize >= SIZE_MAX) {
        rfbClientErr("CRITICAL: cannot allocate frameBuffer, requested size is too large\n");
        return FALSE;
    }

    client->frameBuffer = static_cast<uint8_t *>(malloc((size_t) allocSize));

    if (client->frameBuffer == nullptr) {
        rfbClientErr("CRITICAL: frameBuffer allocation failed\n");
        return FALSE;
    }


    auto obj = getManagedClient(client);
    auto env = context.getEnv();
    auto cls = context.managedCls;

    auto mid = env->GetMethodID(cls, "cbFrameBufferSizeChanged", "()V");
    env->CallVoidMethod(obj, mid);

    return TRUE;
}

/**
 * Hooks callbacks to rfbClient.
 */
static void setCallbacks(rfbClient *client) {
    client->GetPassword = onGetPassword;
    client->GetCredential = onGetCredential;
    client->Bell = onBell;
    client->GotXCutText = onGotXCutText;
    client->HandleCursorPos = onHandleCursorPos;
    client->FinishedFrameBufferUpdate = onFinishedFrameBufferUpdate;
    client->MallocFrameBuffer = onMallocFrameBuffer;
}


/******************************************************************************
 * Native method Implementation
 *****************************************************************************/

extern "C"
JNIEXPORT jlong JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeClientCreate(JNIEnv *env, jobject thiz) {
    rfbClient *client = rfbGetClient(8, 3, 4);
    setCallbacks(client);
    client->canHandleNewFBSize = TRUE;

    //Attach reference to managed object
    auto obj = env->NewGlobalRef(thiz);
    setManagedClient(client, obj);

    return (jlong) client;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeInit(JNIEnv *env, jobject thiz, jlong client_ptr,
                                              jstring host, jint port) {
    auto client = (rfbClient *) client_ptr;

    client->serverHost = getNativeStrCopy(env, host);
    client->serverPort = port < 100 ? port + 5900 : port;

    if (!rfbInitClient(client, nullptr, nullptr)) {
        rfbClientErr("rfbInitClient() failed inside nativeInit().");
        return JNI_FALSE;
    }

    return JNI_TRUE;

}

extern "C"
JNIEXPORT void JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeCleanup(JNIEnv *env, jobject thiz,
                                                 jlong client_ptr) {
    auto client = (rfbClient *) client_ptr;

    if (client->frameBuffer) {
        free(client->frameBuffer);
        client->frameBuffer = nullptr;
    }

    auto managedClient = getManagedClient(client);
    env->DeleteGlobalRef(managedClient);

    rfbClientCleanup(client);
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeProcessServerMessage(JNIEnv *env, jobject thiz,
                                                              jlong client_ptr,
                                                              jint u_sec_timeout) {
    auto client = (rfbClient *) client_ptr;

    if (WaitForMessage(client, u_sec_timeout) >= 0) {
        if (HandleRFBServerMessage(client))
            return JNI_TRUE;
    }

    return JNI_FALSE;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeSendKeyEvent(JNIEnv *env, jobject thiz, jlong client_ptr,
                                                      jlong key, jboolean is_down, jboolean translate) {
    if (translate && key < KEYMAP_LENGTH) {
        key = KEYMAP[key];
    }

    return (jboolean) SendKeyEvent((rfbClient *) client_ptr, (uint32_t) key, is_down);
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeSendPointerEvent(JNIEnv *env, jobject thiz, jlong client_ptr, jint x, jint y,
                                                          jint mask) {
    return (jboolean) SendPointerEvent((rfbClient *) client_ptr, x, y, mask);
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeSendCutText(JNIEnv *env, jobject thiz, jlong client_ptr, jstring text) {
    char *cText = getNativeStrCopy(env, text);
    rfbBool result = SendClientCutText((rfbClient *) client_ptr, cText, strlen(cText));
    free(cText);
    return (jboolean) result;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeRefreshFrameBuffer(JNIEnv *env, jobject thiz, jlong clientPtr) {
    auto client = (rfbClient *) clientPtr;
    return (jboolean) SendFramebufferUpdateRequest(client, 0, 0, client->width, client->height, FALSE);
}

extern "C"
JNIEXPORT jstring JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeGetDesktopName(JNIEnv *env, jobject thiz, jlong client_ptr) {
    auto client = (rfbClient *) client_ptr;
    return env->NewStringUTF(client->desktopName ? client->desktopName : "");
}

extern "C"
JNIEXPORT jint JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeGetWidth(JNIEnv *env, jobject thiz, jlong client_ptr) {
    return ((rfbClient *) client_ptr)->width;
}

extern "C"
JNIEXPORT jint JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeGetHeight(JNIEnv *env, jobject thiz, jlong client_ptr) {
    return ((rfbClient *) client_ptr)->height;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeIsEncrypted(JNIEnv *env, jobject thiz, jlong client_ptr) {
    return ((rfbClient *) client_ptr)->tlsSession ? JNI_TRUE : JNI_FALSE;
}

extern "C"
JNIEXPORT void JNICALL
Java_com_gaurav_avnc_vnc_VncClient_nativeUploadFrameTexture(JNIEnv *env, jobject thiz,
                                                            jlong client_ptr) {
    auto client = (rfbClient *) client_ptr;

    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_RGBA,
                 client->width,
                 client->height,
                 0,
                 GL_RGBA,
                 GL_UNSIGNED_BYTE,
                 client->frameBuffer);
}