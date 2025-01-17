//
//  ScriptingCore.cpp
//  testmonkey
//
//  Created by Rolando Abarca on 3/14/12.
//  Copyright (c) 2012 Zynga Inc. All rights reserved.
//

#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <vector>
#include <map>
#include "ScriptingCore.h"
#include "jsdbgapi.h"
#include "cocos2d.h"
#include "LocalStorage.h"
#include "cocos2d_specifics.hpp"
#include "js_bindings_config.h"
// for debug socket
#if (CC_TARGET_PLATFORM == CC_PLATFORM_WIN32)
#include <io.h>
#include <WS2tcpip.h>
#else
#include <sys/socket.h>
#include <unistd.h>
#include <netdb.h>
#endif

#include <thread>

#ifdef ANDROID
#include <android/log.h>
#include <jni/JniHelper.h>
#include <netinet/in.h>
#endif

#ifdef ANDROID
#define  LOG_TAG    "ScriptingCore.cpp"
#define  LOGD(...)  __android_log_print(ANDROID_LOG_DEBUG,LOG_TAG,__VA_ARGS__)
#else
#define  LOGD(...) js_log(__VA_ARGS__)
#endif

#include "js_bindings_config.h"

#if DEBUG
#define TRACE_DEBUGGER_SERVER(...) CCLOG(__VA_ARGS__)
#else
#define TRACE_DEBUGGER_SERVER(...)
#endif // #if DEBUG

#define BYTE_CODE_FILE_EXT ".jsc"

static string inData;
static string outData;
static vector<string> g_queue;
static std::mutex g_qMutex;
static std::mutex g_rwMutex;
static int clientSocket = -1;
static unsigned long s_nestedLoopLevel = 0;

// server entry point for the bg thread
static void serverEntryPoint(void);

js_proxy_t *_native_js_global_ht = NULL;
js_proxy_t *_js_native_global_ht = NULL;
js_type_class_t *_js_global_type_ht = NULL;
static char *_js_log_buf = NULL;

static std::vector<sc_register_sth> registrationList;

// name ~> JSScript map
static std::map<std::string, JSScript*> filename_script;
// port ~> socket map
static std::map<int,int> ports_sockets;
// name ~> globals
static std::map<std::string, js::RootedObject*> globals;


static void ReportException(JSContext *cx)
{
    if (JS_IsExceptionPending(cx)) {
        if (!JS_ReportPendingException(cx)) {
            JS_ClearPendingException(cx);
        }
    }
}

static void executeJSFunctionFromReservedSpot(JSContext *cx, JSObject *obj,
                                              jsval &dataVal, jsval &retval) {

    jsval func = JS_GetReservedSlot(obj, 0);

    if (func == JSVAL_VOID) { return; }
    jsval thisObj = JS_GetReservedSlot(obj, 1);
    JSAutoCompartment ac(cx, obj);
    if (thisObj == JSVAL_VOID) {
        JS_CallFunctionValue(cx, obj, func, 1, &dataVal, &retval);
    } else {
        assert(!JSVAL_IS_PRIMITIVE(thisObj));
        JS_CallFunctionValue(cx, JSVAL_TO_OBJECT(thisObj), func, 1, &dataVal, &retval);
    }
}

static void getTouchesFuncName(EventTouch::EventCode eventCode, std::string &funcName)
{
    switch(eventCode)
    {
        case EventTouch::EventCode::BEGAN:
            funcName = "onTouchesBegan";
            break;
        case EventTouch::EventCode::ENDED:
            funcName = "onTouchesEnded";
            break;
        case EventTouch::EventCode::MOVED:
            funcName = "onTouchesMoved";
            break;
        case EventTouch::EventCode::CANCELLED:
            funcName = "onTouchesCancelled";
            break;
    }
}

static void getTouchFuncName(EventTouch::EventCode eventCode, std::string &funcName)
{
    switch(eventCode) {
        case EventTouch::EventCode::BEGAN:
            funcName = "onTouchBegan";
            break;
        case EventTouch::EventCode::ENDED:
            funcName = "onTouchEnded";
            break;
        case EventTouch::EventCode::MOVED:
            funcName = "onTouchMoved";
            break;
        case EventTouch::EventCode::CANCELLED:
            funcName = "onTouchCancelled";
            break;
    }
}

static void rootObject(JSContext *cx, JSObject *obj) {
    JS_AddNamedObjectRoot(cx, &obj, "unnamed");
}


static void unRootObject(JSContext *cx, JSObject *obj) {
    JS_RemoveObjectRoot(cx, &obj);
}

static void getJSTouchObject(JSContext *cx, Touch *x, jsval &jsret) {
    js_proxy_t *proxy = js_get_or_create_proxy<cocos2d::Touch>(cx, x);
    jsret = OBJECT_TO_JSVAL(proxy->obj);
}

static void removeJSTouchObject(JSContext *cx, Touch *x, jsval &jsret) {
    js_proxy_t* nproxy;
    js_proxy_t* jsproxy;
    void *ptr = (void*)x;
    nproxy = jsb_get_native_proxy(ptr);
    if (nproxy) {
        jsproxy = jsb_get_js_proxy(nproxy->obj);
        JS_RemoveObjectRoot(cx, &jsproxy->obj);
        jsb_remove_proxy(nproxy, jsproxy);
    }
}

void ScriptingCore::executeJSFunctionWithThisObj(jsval thisObj,
                                                 jsval callback,
                                                 uint32_t argc/* = 0*/,
                                                 jsval* vp/* = NULL*/,
                                                 jsval* retVal/* = NULL*/)
{
    if (callback != JSVAL_VOID || thisObj != JSVAL_VOID)
    {
        // Very important: The last parameter 'retVal' passed to 'JS_CallFunctionValue' should not be a NULL pointer.
        // If it's a NULL pointer, crash will be triggered in 'JS_CallFunctionValue'. To find out the reason of this crash is very difficult.
        // So we have to check the availability of 'retVal'.
        if (retVal)
        {
            JS_CallFunctionValue(_cx, JSVAL_TO_OBJECT(thisObj), callback, argc, vp, retVal);
        }
        else
        {
            jsval jsRet;
            JS_CallFunctionValue(_cx, JSVAL_TO_OBJECT(thisObj), callback, argc, vp, &jsRet);
        }
    }
}

void js_log(const char *format, ...) {

    if (_js_log_buf == NULL)
    {
        _js_log_buf = (char *)calloc(sizeof(char), kMaxLogLen+1);
        _js_log_buf[kMaxLogLen] = '\0';
    }
    va_list vl;
    va_start(vl, format);
    int len = vsnprintf(_js_log_buf, kMaxLogLen, format, vl);
    va_end(vl);
    if (len > 0)
    {
        CCLOG("JS: %s\n", _js_log_buf);
    }
}

#define JSB_COMPATIBLE_WITH_COCOS2D_HTML5_BASIC_TYPES 1

JSBool JSBCore_platform(JSContext *cx, uint32_t argc, jsval *vp)
{
    if (argc!=0)
    {
        JS_ReportError(cx, "Invalid number of arguments in __getPlatform");
        return JS_FALSE;
    }

    JSString * platform;

    // config.deviceType: Device Type
    // 'mobile' for any kind of mobile devices, 'desktop' for PCs, 'browser' for Web Browsers
    // #if (CC_TARGET_PLATFORM == CC_PLATFORM_WIN32) || (CC_TARGET_PLATFORM == CC_PLATFORM_LINUX) || (CC_TARGET_PLATFORM == CC_PLATFORM_MAC)
    //     platform = JS_InternString(_cx, "desktop");
    // #else
    platform = JS_InternString(cx, "mobile");
    // #endif

    jsval ret = STRING_TO_JSVAL(platform);

    JS_SET_RVAL(cx, vp, ret);

    return JS_TRUE;
};

JSBool JSBCore_version(JSContext *cx, uint32_t argc, jsval *vp)
{
    if (argc!=0)
    {
        JS_ReportError(cx, "Invalid number of arguments in __getVersion");
        return JS_FALSE;
    }

    char version[256];
    snprintf(version, sizeof(version)-1, "%s - %s", cocos2dVersion(), JSB_version);
    JSString * js_version = JS_InternString(cx, version);

    jsval ret = STRING_TO_JSVAL(js_version);
    JS_SET_RVAL(cx, vp, ret);

    return JS_TRUE;
};

JSBool JSBCore_os(JSContext *cx, uint32_t argc, jsval *vp)
{
    if (argc!=0)
    {
        JS_ReportError(cx, "Invalid number of arguments in __getOS");
        return JS_FALSE;
    }

    JSString * os;

    // osx, ios, android, windows, linux, etc..
#if (CC_TARGET_PLATFORM == CC_PLATFORM_IOS)
    os = JS_InternString(cx, "ios");
#elif (CC_TARGET_PLATFORM == CC_PLATFORM_ANDROID)
    os = JS_InternString(cx, "android");
#elif (CC_TARGET_PLATFORM == CC_PLATFORM_WIN32)
    os = JS_InternString(cx, "windows");
#elif (CC_TARGET_PLATFORM == CC_PLATFORM_MARMALADE)
    os = JS_InternString(cx, "marmalade");
#elif (CC_TARGET_PLATFORM == CC_PLATFORM_LINUX)
    os = JS_InternString(cx, "linux");
#elif (CC_TARGET_PLATFORM == CC_PLATFORM_BADA)
    os = JS_InternString(cx, "bada");
#elif (CC_TARGET_PLATFORM == CC_PLATFORM_BLACKBERRY)
    os = JS_InternString(cx, "blackberry");
#elif (CC_TARGET_PLATFORM == CC_PLATFORM_MAC)
    os = JS_InternString(cx, "osx");
#else
    os = JS_InternString(cx, "unknown");
#endif

    jsval ret = STRING_TO_JSVAL(os);
    JS_SET_RVAL(cx, vp, ret);

    return JS_TRUE;
};

JSBool JSB_core_restartVM(JSContext *cx, uint32_t argc, jsval *vp)
{
	JSB_PRECONDITION2(argc==0, cx, JS_FALSE, "Invalid number of arguments in executeScript");
    ScriptingCore::getInstance()->reset();
    JS_SET_RVAL(cx, vp, JSVAL_VOID);
	return JS_TRUE;
};

void registerDefaultClasses(JSContext* cx, JSObject* global) {
    // first, try to get the ns
    jsval nsval;
    JSObject *ns;
    JS_GetProperty(cx, global, "cc", &nsval);
    if (nsval == JSVAL_VOID) {
        ns = JS_NewObject(cx, NULL, NULL, NULL);
        nsval = OBJECT_TO_JSVAL(ns);
        JS_SetProperty(cx, global, "cc", &nsval);
    } else {
        JS_ValueToObject(cx, nsval, &ns);
    }

    //
    // Javascript controller (__jsc__)
    //
    JSObject *jsc = JS_NewObject(cx, NULL, NULL, NULL);
    jsval jscVal = OBJECT_TO_JSVAL(jsc);
    JS_SetProperty(cx, global, "__jsc__", &jscVal);

    JS_DefineFunction(cx, jsc, "garbageCollect", ScriptingCore::forceGC, 0, JSPROP_READONLY | JSPROP_PERMANENT | JSPROP_ENUMERATE );
    JS_DefineFunction(cx, jsc, "dumpRoot", ScriptingCore::dumpRoot, 0, JSPROP_READONLY | JSPROP_PERMANENT | JSPROP_ENUMERATE );
    JS_DefineFunction(cx, jsc, "addGCRootObject", ScriptingCore::addRootJS, 1, JSPROP_READONLY | JSPROP_PERMANENT | JSPROP_ENUMERATE );
    JS_DefineFunction(cx, jsc, "removeGCRootObject", ScriptingCore::removeRootJS, 1, JSPROP_READONLY | JSPROP_PERMANENT | JSPROP_ENUMERATE );
    JS_DefineFunction(cx, jsc, "executeScript", ScriptingCore::executeScript, 1, JSPROP_READONLY | JSPROP_PERMANENT | JSPROP_ENUMERATE );

    // register some global functions
    JS_DefineFunction(cx, global, "require", ScriptingCore::executeScript, 1, JSPROP_READONLY | JSPROP_PERMANENT);
    JS_DefineFunction(cx, global, "log", ScriptingCore::log, 0, JSPROP_READONLY | JSPROP_PERMANENT);
    JS_DefineFunction(cx, global, "executeScript", ScriptingCore::executeScript, 1, JSPROP_READONLY | JSPROP_PERMANENT);
    JS_DefineFunction(cx, global, "forceGC", ScriptingCore::forceGC, 0, JSPROP_READONLY | JSPROP_PERMANENT);

    JS_DefineFunction(cx, global, "__getPlatform", JSBCore_platform, 0, JSPROP_READONLY | JSPROP_PERMANENT);
    JS_DefineFunction(cx, global, "__getOS", JSBCore_os, 0, JSPROP_READONLY | JSPROP_PERMANENT);
    JS_DefineFunction(cx, global, "__getVersion", JSBCore_version, 0, JSPROP_READONLY | JSPROP_PERMANENT);
    JS_DefineFunction(cx, global, "__restartVM", JSB_core_restartVM, 0, JSPROP_READONLY | JSPROP_PERMANENT | JSPROP_ENUMERATE );
}

static void sc_finalize(JSFreeOp *freeOp, JSObject *obj) {
    CCLOGINFO("jsbindings: finalizing JS object %p (global class)", obj);
}

static JSClass global_class = {
    "global", JSCLASS_GLOBAL_FLAGS,
    JS_PropertyStub, JS_DeletePropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
    JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, sc_finalize,
    JSCLASS_NO_OPTIONAL_MEMBERS
};

ScriptingCore::ScriptingCore()
: _rt(nullptr)
, _cx(nullptr)
, _global(nullptr)
, _debugGlobal(nullptr)
{
    // set utf8 strings internally (we don't need utf16)
    // XXX: Removed in SpiderMonkey 19.0
    //JS_SetCStringsAreUTF8();
    this->addRegisterCallback(registerDefaultClasses);
    this->_runLoop = new SimpleRunLoop();
}

void ScriptingCore::string_report(jsval val) {
    if (JSVAL_IS_NULL(val)) {
        LOGD("val : (JSVAL_IS_NULL(val)");
        // return 1;
    } else if ((JSVAL_IS_BOOLEAN(val)) &&
               (JS_FALSE == (JSVAL_TO_BOOLEAN(val)))) {
        LOGD("val : (return value is JS_FALSE");
        // return 1;
    } else if (JSVAL_IS_STRING(val)) {
        JSString *str = JS_ValueToString(this->getGlobalContext(), val);
        if (NULL == str) {
            LOGD("val : return string is NULL");
        } else {
            JSStringWrapper wrapper(str);
            LOGD("val : return string =\n%s\n", (char *)wrapper);
        }
    } else if (JSVAL_IS_NUMBER(val)) {
        double number;
        if (JS_FALSE ==
            JS_ValueToNumber(this->getGlobalContext(), val, &number)) {
            LOGD("val : return number could not be converted");
        } else {
            LOGD("val : return number =\n%f", number);
        }
    }
}

JSBool ScriptingCore::evalString(const char *string, jsval *outVal, const char *filename, JSContext* cx, JSObject* global)
{
    if (cx == NULL)
        cx = _cx;
    if (global == NULL)
        global = _global;
    JSScript* script = JS_CompileScript(cx, global, string, strlen(string), filename, 1);
    if (script) {
        JSAutoCompartment ac(cx, global);
        JSBool evaluatedOK = JS_ExecuteScript(cx, global, script, outVal);
        if (JS_FALSE == evaluatedOK) {
            fprintf(stderr, "(evaluatedOK == JS_FALSE)\n");
        }
        return evaluatedOK;
    }
    return JS_FALSE;
}

void ScriptingCore::start() {
    // for now just this
    this->createGlobalContext();
}

void ScriptingCore::addRegisterCallback(sc_register_sth callback) {
    registrationList.push_back(callback);
}

void ScriptingCore::removeAllRoots(JSContext *cx) {
    js_proxy_t *current, *tmp;
    HASH_ITER(hh, _js_native_global_ht, current, tmp) {
        JS_RemoveObjectRoot(cx, &current->obj);
        HASH_DEL(_js_native_global_ht, current);
        free(current);
    }
    HASH_ITER(hh, _native_js_global_ht, current, tmp) {
        HASH_DEL(_native_js_global_ht, current);
        free(current);
    }
    HASH_CLEAR(hh, _js_native_global_ht);
    HASH_CLEAR(hh, _native_js_global_ht);
}

static JSPrincipals shellTrustedPrincipals = { 1 };

static JSBool
CheckObjectAccess(JSContext *cx, js::HandleObject obj, js::HandleId id, JSAccessMode mode,
                  js::MutableHandleValue vp)
{
    return JS_TRUE;
}

static JSSecurityCallbacks securityCallbacks = {
    CheckObjectAccess,
    NULL
};

void ScriptingCore::createGlobalContext() {
    if (this->_cx && this->_rt) {
        ScriptingCore::removeAllRoots(this->_cx);
        JS_DestroyContext(this->_cx);
        JS_DestroyRuntime(this->_rt);
        this->_cx = NULL;
        this->_rt = NULL;
    }
    // Removed from Spidermonkey 19.
    //JS_SetCStringsAreUTF8();
    this->_rt = JS_NewRuntime(8L * 1024L * 1024L, JS_USE_HELPER_THREADS);
    JS_SetGCParameter(_rt, JSGC_MAX_BYTES, 0xffffffff);
	
    JS_SetTrustedPrincipals(_rt, &shellTrustedPrincipals);
    JS_SetSecurityCallbacks(_rt, &securityCallbacks);
	JS_SetNativeStackQuota(_rt, JSB_MAX_STACK_QUOTA);
    
    this->_cx = JS_NewContext(_rt, 8192);
    JS_SetOptions(this->_cx, JSOPTION_TYPE_INFERENCE);
    JS_SetVersion(this->_cx, JSVERSION_LATEST);
    
    // Only disable METHODJIT on iOS.
#if (CC_TARGET_PLATFORM == CC_PLATFORM_IOS)
    JS_SetOptions(this->_cx, JS_GetOptions(this->_cx) & ~JSOPTION_METHODJIT);
    JS_SetOptions(this->_cx, JS_GetOptions(this->_cx) & ~JSOPTION_METHODJIT_ALWAYS);
#endif
    
    JS_SetErrorReporter(this->_cx, ScriptingCore::reportError);
#if defined(JS_GC_ZEAL) && defined(DEBUG)
    //JS_SetGCZeal(this->_cx, 2, JS_DEFAULT_ZEAL_FREQ);
#endif
    this->_global = NewGlobalObject(_cx);

    for (std::vector<sc_register_sth>::iterator it = registrationList.begin(); it != registrationList.end(); it++) {
        sc_register_sth callback = *it;
        callback(this->_cx, this->_global);
    }
}

static std::string RemoveFileExt(const std::string& filePath) {
    size_t pos = filePath.rfind('.');
    if (0 < pos) {
        return filePath.substr(0, pos);
    }
    else {
        return filePath;
    }
}

JSBool ScriptingCore::runScript(const char *path, JSObject* global, JSContext* cx)
{
    if (!path) {
        return false;
    }
    
    
    cocos2d::FileUtils *futil = cocos2d::FileUtils::getInstance();
    std::string fullPath = futil->fullPathForFilename(path);
    if (global == NULL) {
        global = _global;
    }
    if (cx == NULL) {
        cx = _cx;
    }
    JSScript *script = NULL;    
    js::RootedObject obj(cx, global);
	JS::CompileOptions options(cx);
	options.setUTF8(true).setFileAndLine(fullPath.c_str(), 1);
    
    // a) check jsc file first
    std::string byteCodePath = RemoveFileExt(std::string(path)) + BYTE_CODE_FILE_EXT;
    unsigned long length = 0;
    unsigned char* data = futil->getFileData(byteCodePath.c_str(),
                                    "rb",
                                    &length);
    
    if (data) {
        script = JS_DecodeScript(cx, data, length, NULL, NULL);
        CC_SAFE_DELETE_ARRAY(data);
    }
    
    // b) no jsc file, check js file
    if (!script) {
        /* Clear any pending exception from previous failed decoding.  */
        ReportException(cx);
        
#if (CC_TARGET_PLATFORM == CC_PLATFORM_ANDROID)
        String* content = String::createWithContentsOfFile(path);
        if (content) {
            // Not supported in SpiderMonkey 19.0
            //JSScript* script = JS_CompileScript(cx, global, (char*)content, contentSize, path, 1);
            const char* contentCStr = content->getCString();
            script = JS::Compile(cx, obj, options, contentCStr, strlen(contentCStr));
        }
#else
        script = JS::Compile(cx, obj, options, fullPath.c_str());
#endif
    }
    
    JSBool evaluatedOK = false;
    if (script) {
        jsval rval;
        filename_script[path] = script;
        JSAutoCompartment ac(cx, global);
        evaluatedOK = JS_ExecuteScript(cx, global, script, &rval);
        if (JS_FALSE == evaluatedOK) {
            cocos2d::log("(evaluatedOK == JS_FALSE)");
            JS_ReportPendingException(cx);
        }
    }
    return evaluatedOK;
}

void ScriptingCore::reset()
{
    cleanup();
    start();
}

ScriptingCore::~ScriptingCore()
{
    cleanup();
}

void ScriptingCore::cleanup()
{
    localStorageFree();
    removeAllRoots(_cx);
    if (_cx)
    {
        JS_DestroyContext(_cx);
        _cx = NULL;
    }
    if (_rt)
    {
        JS_DestroyRuntime(_rt);
        _rt = NULL;
    }
    JS_ShutDown();
    if (_js_log_buf) {
        free(_js_log_buf);
        _js_log_buf = NULL;
    }

    js_type_class_t* current, *tmp;
    HASH_ITER(hh, _js_global_type_ht, current, tmp)
    {
        HASH_DEL(_js_global_type_ht, current);
        free(current->jsclass);
        free(current);
    }
    HASH_CLEAR(hh, _js_global_type_ht);
}

void ScriptingCore::reportError(JSContext *cx, const char *message, JSErrorReport *report)
{
    js_log("%s:%u:%s\n",
            report->filename ? report->filename : "<no filename=\"filename\">",
            (unsigned int) report->lineno,
            message);
};


JSBool ScriptingCore::log(JSContext* cx, uint32_t argc, jsval *vp)
{
    if (argc > 0) {
        JSString *string = NULL;
        JS_ConvertArguments(cx, argc, JS_ARGV(cx, vp), "S", &string);
        if (string) {
            JSStringWrapper wrapper(string);
            js_log("%s", (char *)wrapper);
        }
    }
    return JS_TRUE;
}


void ScriptingCore::removeScriptObjectByObject(Object* pObj)
{
    js_proxy_t* nproxy;
    js_proxy_t* jsproxy;
    void *ptr = (void*)pObj;
    nproxy = jsb_get_native_proxy(ptr);
    if (nproxy) {
        JSContext *cx = ScriptingCore::getInstance()->getGlobalContext();
        jsproxy = jsb_get_js_proxy(nproxy->obj);
        JS_RemoveObjectRoot(cx, &jsproxy->obj);
        jsb_remove_proxy(nproxy, jsproxy);
    }
}


JSBool ScriptingCore::setReservedSpot(uint32_t i, JSObject *obj, jsval value) {
    JS_SetReservedSlot(obj, i, value);
    return JS_TRUE;
}

JSBool ScriptingCore::executeScript(JSContext *cx, uint32_t argc, jsval *vp)
{
    if (argc >= 1) {
        jsval* argv = JS_ARGV(cx, vp);
        JSString* str = JS_ValueToString(cx, argv[0]);
        JSStringWrapper path(str);
        JSBool res = false;
        if (argc == 2 && argv[1].isString()) {
            JSString* globalName = JSVAL_TO_STRING(argv[1]);
            JSStringWrapper name(globalName);
//            js::RootedObject* rootedGlobal = globals[name];
            JSObject* debugObj = ScriptingCore::getInstance()->getDebugGlobal();
            if (debugObj) {
                res = ScriptingCore::getInstance()->runScript(path, debugObj);
            } else {
                JS_ReportError(cx, "Invalid global object: %s", (char*)name);
                return JS_FALSE;
            }
        } else {
            JSObject* glob = JS_GetGlobalForScopeChain(cx);
            res = ScriptingCore::getInstance()->runScript(path, glob);
        }
        return res;
    }
    return JS_TRUE;
}

JSBool ScriptingCore::forceGC(JSContext *cx, uint32_t argc, jsval *vp)
{
    JSRuntime *rt = JS_GetRuntime(cx);
    JS_GC(rt);
    return JS_TRUE;
}

//static void dumpNamedRoot(const char *name, void *addr,  JSGCRootType type, void *data)
//{
//    CCLOG("Root: '%s' at %p", name, addr);
//}

JSBool ScriptingCore::dumpRoot(JSContext *cx, uint32_t argc, jsval *vp)
{
    // JS_DumpNamedRoots is only available on DEBUG versions of SpiderMonkey.
    // Mac and Simulator versions were compiled with DEBUG.
#if DEBUG
//    JSContext *_cx = ScriptingCore::getInstance()->getGlobalContext();
//    JSRuntime *rt = JS_GetRuntime(_cx);
//    JS_DumpNamedRoots(rt, dumpNamedRoot, NULL);
//    JS_DumpHeap(rt, stdout, NULL, JSTRACE_OBJECT, NULL, 2, NULL);
#endif
    return JS_TRUE;
}

JSBool ScriptingCore::addRootJS(JSContext *cx, uint32_t argc, jsval *vp)
{
    if (argc == 1) {
        JSObject *o = NULL;
        if (JS_ConvertArguments(cx, argc, JS_ARGV(cx, vp), "o", &o) == JS_TRUE) {
            if (JS_AddNamedObjectRoot(cx, &o, "from-js") == JS_FALSE) {
                LOGD("something went wrong when setting an object to the root");
            }
        }
        return JS_TRUE;
    }
    return JS_FALSE;
}

JSBool ScriptingCore::removeRootJS(JSContext *cx, uint32_t argc, jsval *vp)
{
    if (argc == 1) {
        JSObject *o = NULL;
        if (JS_ConvertArguments(cx, argc, JS_ARGV(cx, vp), "o", &o) == JS_TRUE) {
            JS_RemoveObjectRoot(cx, &o);
        }
        return JS_TRUE;
    }
    return JS_FALSE;
}

void ScriptingCore::pauseSchedulesAndActions(js_proxy_t* p)
{
    Array * arr = JSScheduleWrapper::getTargetForJSObject(p->obj);
    if (! arr) return;
    
    Node* node = (Node*)p->ptr;
    for(unsigned int i = 0; i < arr->count(); ++i) {
        if (arr->getObjectAtIndex(i)) {
            node->getScheduler()->pauseTarget(arr->getObjectAtIndex(i));
        }
    }
}


void ScriptingCore::resumeSchedulesAndActions(js_proxy_t* p)
{
    Array * arr = JSScheduleWrapper::getTargetForJSObject(p->obj);
    if (!arr) return;
    
    Node* node = (Node*)p->ptr;
    for(unsigned int i = 0; i < arr->count(); ++i) {
        if (!arr->getObjectAtIndex(i)) continue;
        node->getScheduler()->resumeTarget(arr->getObjectAtIndex(i));
    }
}

void ScriptingCore::cleanupSchedulesAndActions(js_proxy_t* p)
{
    Array * arr = JSCallFuncWrapper::getTargetForNativeNode((Node*)p->ptr);
    if (arr) {
        arr->removeAllObjects();
    }
    
    arr = JSScheduleWrapper::getTargetForJSObject(p->obj);
    if (arr) {
        Scheduler* pScheduler = Director::getInstance()->getScheduler();
        Object* pObj = NULL;
        CCARRAY_FOREACH(arr, pObj)
        {
            pScheduler->unscheduleAllForTarget(pObj);
        }

        JSScheduleWrapper::removeAllTargetsForJSObject(p->obj);
    }
}

int ScriptingCore::handleNodeEvent(void* data)
{
    if (NULL == data)
        return 0;
    
    BasicScriptData* basicScriptData = static_cast<BasicScriptData*>(data);
    if (NULL == basicScriptData->nativeObject || NULL == basicScriptData->value)
        return 0;
    
    Node* node = static_cast<Node*>(basicScriptData->nativeObject);
    int action = *((int*)(basicScriptData->value));
                                                         
    js_proxy_t * p = jsb_get_native_proxy(node);
    if (!p) return 0;

    jsval retval;
    jsval dataVal = INT_TO_JSVAL(1);

    if (action == kNodeOnEnter)
    {
        executeFunctionWithOwner(OBJECT_TO_JSVAL(p->obj), "onEnter", 1, &dataVal, &retval);
        resumeSchedulesAndActions(p);
    }
    else if (action == kNodeOnExit)
    {
        executeFunctionWithOwner(OBJECT_TO_JSVAL(p->obj), "onExit", 1, &dataVal, &retval);
        pauseSchedulesAndActions(p);
    }
    else if (action == kNodeOnEnterTransitionDidFinish)
    {
        executeFunctionWithOwner(OBJECT_TO_JSVAL(p->obj), "onEnterTransitionDidFinish", 1, &dataVal, &retval);
    }
    else if (action == kNodeOnExitTransitionDidStart)
    {
        executeFunctionWithOwner(OBJECT_TO_JSVAL(p->obj), "onExitTransitionDidStart", 1, &dataVal, &retval);
    }
    else if (action == kNodeOnCleanup) {
        cleanupSchedulesAndActions(p);
    }

    return 1;
}

int ScriptingCore::handleMenuClickedEvent(void* data)
{
    if (NULL == data)
        return 0;
    
    BasicScriptData* basicScriptData = static_cast<BasicScriptData*>(data);
    if (NULL == basicScriptData->nativeObject)
        return 0;
    
    MenuItem* menuItem = static_cast<MenuItem*>(basicScriptData->nativeObject);
    
    js_proxy_t * p = jsb_get_native_proxy(menuItem);
    if (!p) return 0;

    jsval retval;
    jsval dataVal;
    js_proxy_t *proxy = jsb_get_native_proxy(menuItem);
    dataVal = (proxy ? OBJECT_TO_JSVAL(proxy->obj) : JSVAL_NULL);

    executeJSFunctionFromReservedSpot(this->_cx, p->obj, dataVal, retval);

    return 1;
}

int ScriptingCore::handleTouchesEvent(void* data)
{
    if (NULL == data)
        return 0;
    
    TouchesScriptData* touchesScriptData = static_cast<TouchesScriptData*>(data);
    if (NULL == touchesScriptData->nativeObject || touchesScriptData->touches.empty())
        return 0;
    
    Layer* pLayer = static_cast<Layer*>(touchesScriptData->nativeObject);
    EventTouch::EventCode eventType = touchesScriptData->actionType;
    const std::vector<Touch*>& touches = touchesScriptData->touches;
    
    std::string funcName = "";
    getTouchesFuncName(eventType, funcName);

    JSObject *jsretArr = JS_NewArrayObject(this->_cx, 0, NULL);

    JS_AddNamedObjectRoot(this->_cx, &jsretArr, "touchArray");
    int count = 0;
    
    for (auto& touch : touches)
    {
        jsval jsret;
        getJSTouchObject(this->_cx, touch, jsret);
        if (!JS_SetElement(this->_cx, jsretArr, count, &jsret))
        {
            break;
        }
        ++count;
    }

    executeFunctionWithObjectData(pLayer,  funcName.c_str(), jsretArr);

    JS_RemoveObjectRoot(this->_cx, &jsretArr);

    for (auto& touch : touches)
    {
        jsval jsret;
        removeJSTouchObject(this->_cx, touch, jsret);
    }

    return 1;
}

int ScriptingCore::handleTouchEvent(void* data)
{
    if (NULL == data)
        return 0;
    
    TouchScriptData* touchScriptData = static_cast<TouchScriptData*>(data);
    if (NULL == touchScriptData->nativeObject || NULL == touchScriptData->touch)
        return 0;
    
    Layer* pLayer = static_cast<Layer*>(touchScriptData->nativeObject);
    EventTouch::EventCode eventType = touchScriptData->actionType;
    Touch *pTouch = touchScriptData->touch;

    
    std::string funcName = "";
    getTouchFuncName(eventType, funcName);

    jsval jsret;
    getJSTouchObject(this->getGlobalContext(), pTouch, jsret);
    JSObject *jsObj = JSVAL_TO_OBJECT(jsret);
    bool retval = executeFunctionWithObjectData(pLayer,  funcName.c_str(), jsObj);

    removeJSTouchObject(this->getGlobalContext(), pTouch, jsret);

    return retval;
}

bool ScriptingCore::executeFunctionWithObjectData(Node *self, const char *name, JSObject *obj) {

    js_proxy_t * p = jsb_get_native_proxy(self);
    if (!p) return false;

    jsval retval;
    jsval dataVal = OBJECT_TO_JSVAL(obj);

    executeFunctionWithOwner(OBJECT_TO_JSVAL(p->obj), name, 1, &dataVal, &retval);
    if (JSVAL_IS_NULL(retval)) {
        return false;
    }
    else if (JSVAL_IS_BOOLEAN(retval)) {
        return JSVAL_TO_BOOLEAN(retval);
    }
    return false;
}

JSBool ScriptingCore::executeFunctionWithOwner(jsval owner, const char *name, uint32_t argc /* = 0 */, jsval *vp /* = NULL */, jsval* retVal /* = NULL */)
{
    JSBool bRet = JS_FALSE;
    JSBool hasAction;
    jsval temp_retval;
    JSContext* cx = this->_cx;
    JSObject* obj = JSVAL_TO_OBJECT(owner);
    
    do
    {
        JSAutoCompartment ac(cx, obj);
        
        if (JS_HasProperty(cx, obj, name, &hasAction) && hasAction) {
            if (!JS_GetProperty(cx, obj, name, &temp_retval)) {
                break;
            }
            if (temp_retval == JSVAL_VOID) {
                break;
            }            
            
            if (retVal) {
                bRet = JS_CallFunctionName(cx, obj, name, argc, vp, retVal);
            }
            else {
                jsval jsret;
                bRet = JS_CallFunctionName(cx, obj, name, argc, vp, &jsret);
            }
        }
    }while(0);
    return bRet;
}

int ScriptingCore::handleAccelerometerEvent(void* data)
{
    if (NULL == data)
        return 0;
    
    BasicScriptData* basicScriptData = static_cast<BasicScriptData*>(data);
    if (NULL == basicScriptData->nativeObject || NULL == basicScriptData->value)
        return 0;
    
    Acceleration* accelerationValue = static_cast<Acceleration*>(basicScriptData->value);
    Layer* layer = static_cast<Layer*>(basicScriptData->nativeObject);
    
    jsval value = ccacceleration_to_jsval(this->getGlobalContext(), *accelerationValue);
    JS_AddValueRoot(this->getGlobalContext(), &value);

    executeFunctionWithObjectData(layer, "onAccelerometer", JSVAL_TO_OBJECT(value));

    JS_RemoveValueRoot(this->getGlobalContext(), &value);
    return 1;
}

int ScriptingCore::handleKeypadEvent(void* data)
{
    if (NULL == data)
        return 0;
    
    KeypadScriptData* keypadScriptData = static_cast<KeypadScriptData*>(data);
    if (NULL == keypadScriptData->nativeObject)
        return 0;
    
    EventKeyboard::KeyCode action = keypadScriptData->actionType;
    
	js_proxy_t * p = jsb_get_native_proxy(keypadScriptData->nativeObject);

	if (p)
    {
        JSBool ret = JS_FALSE;
        switch(action)
        {
        case EventKeyboard::KeyCode::KEY_BACKSPACE:
			ret = executeFunctionWithOwner(OBJECT_TO_JSVAL(p->obj), "onBackClicked");
            if (!ret)
            {
                ret = executeFunctionWithOwner(OBJECT_TO_JSVAL(p->obj), "backClicked");
                if (ret)
                {
                    CCLOG("backClicked will be deprecated, please use onBackClicked instead.");
                }
            }
			break;
		case EventKeyboard::KeyCode::KEY_MENU:
            ret = executeFunctionWithOwner(OBJECT_TO_JSVAL(p->obj), "onMenuClicked");
            if (!ret)
            {
                ret = executeFunctionWithOwner(OBJECT_TO_JSVAL(p->obj), "menuClicked");
                if (ret)
                {
                    CCLOG("menuClicked will be deprecated, please use onMenuClicked instead.");
                }
            }
			break;
		default:
			break;
		}
		return 1;
	}

	return 0;
}


int ScriptingCore::executeCustomTouchesEvent(EventTouch::EventCode eventType,
                                       const std::vector<Touch*>& touches, JSObject *obj)
{
    jsval retval;
    std::string funcName;
    getTouchesFuncName(eventType, funcName);

    JSObject *jsretArr = JS_NewArrayObject(this->_cx, 0, NULL);
    JS_AddNamedObjectRoot(this->_cx, &jsretArr, "touchArray");
    int count = 0;
    for (auto& touch : touches)
    {
        jsval jsret;
        getJSTouchObject(this->_cx, touch, jsret);
        if (!JS_SetElement(this->_cx, jsretArr, count, &jsret)) {
            break;
        }
        ++count;
    }

    jsval jsretArrVal = OBJECT_TO_JSVAL(jsretArr);
    executeFunctionWithOwner(OBJECT_TO_JSVAL(obj), funcName.c_str(), 1, &jsretArrVal, &retval);
    JS_RemoveObjectRoot(this->_cx, &jsretArr);

    for (auto& touch : touches)
    {
        jsval jsret;
        removeJSTouchObject(this->_cx, touch, jsret);
    }

    return 1;
}


int ScriptingCore::executeCustomTouchEvent(EventTouch::EventCode eventType,
                                           Touch *pTouch, JSObject *obj)
{
    jsval retval;
    std::string funcName;
    getTouchFuncName(eventType, funcName);

    jsval jsTouch;
    getJSTouchObject(this->_cx, pTouch, jsTouch);

    executeFunctionWithOwner(OBJECT_TO_JSVAL(obj), funcName.c_str(), 1, &jsTouch, &retval);

    // Remove touch object from global hash table and unroot it.
    removeJSTouchObject(this->_cx, pTouch, jsTouch);
    return 1;

}


int ScriptingCore::executeCustomTouchEvent(EventTouch::EventCode eventType,
                                           Touch *pTouch, JSObject *obj,
                                           jsval &retval)
{

    std::string funcName;
    getTouchFuncName(eventType, funcName);

    jsval jsTouch;
    getJSTouchObject(this->_cx, pTouch, jsTouch);

    executeFunctionWithOwner(OBJECT_TO_JSVAL(obj), funcName.c_str(), 1, &jsTouch, &retval);

    // Remove touch object from global hash table and unroot it.
    removeJSTouchObject(this->_cx, pTouch, jsTouch);

    return 1;

}

int ScriptingCore::sendEvent(ScriptEvent* evt)
{
    if (NULL == evt)
        return 0;
 
    JSAutoCompartment ac(_cx, _global);
    
    switch (evt->type)
    {
        case kNodeEvent:
            {
                return handleNodeEvent(evt->data);
            }
            break;
        case kMenuClickedEvent:
            {
                return handleMenuClickedEvent(evt->data);
            }
            break;
        case kTouchEvent:
            {
                return handleTouchEvent(evt->data);
            }
            break;
        case kTouchesEvent:
            {
                return handleTouchesEvent(evt->data);
            }
            break;
        case kKeypadEvent:
            {
                return handleKeypadEvent(evt->data);
            }
            break;
        case kAccelerometerEvent:
            {
                return handleAccelerometerEvent(evt->data);
            }
            break;
        default:
            break;
    }
    
    return 0;
}

#pragma mark - Conversion Routines
JSBool jsval_to_int32( JSContext *cx, jsval vp, int32_t *outval )
{
    JSBool ok = JS_TRUE;
    double dp;
    ok &= JS_ValueToNumber(cx, vp, &dp);
    JSB_PRECONDITION3(ok, cx, JS_FALSE, "Error processing arguments");
    ok &= !isnan(dp);
    JSB_PRECONDITION3(ok, cx, JS_FALSE, "Error processing arguments");

    *outval = (int32_t)dp;

    return ok;
}

JSBool jsval_to_uint32( JSContext *cx, jsval vp, uint32_t *outval )
{
    JSBool ok = JS_TRUE;
    double dp;
    ok &= JS_ValueToNumber(cx, vp, &dp);
    JSB_PRECONDITION3(ok, cx, JS_FALSE, "Error processing arguments");
    ok &= !isnan(dp);
    JSB_PRECONDITION3(ok, cx, JS_FALSE, "Error processing arguments");

    *outval = (uint32_t)dp;

    return ok;
}

JSBool jsval_to_uint16( JSContext *cx, jsval vp, uint16_t *outval )
{
    JSBool ok = JS_TRUE;
    double dp;
    ok &= JS_ValueToNumber(cx, vp, &dp);
    JSB_PRECONDITION3(ok, cx, JS_FALSE, "Error processing arguments");
    ok &= !isnan(dp);
    JSB_PRECONDITION3(ok, cx, JS_FALSE, "Error processing arguments");

    *outval = (uint16_t)dp;

    return ok;
}

JSBool jsval_to_long_long(JSContext *cx, jsval vp, long long* r) {
	JSObject *tmp_arg;
	JSBool ok = JS_ValueToObject( cx, vp, &tmp_arg );
	JSB_PRECONDITION3( ok, cx, JS_FALSE, "Error converting value to object");
	JSB_PRECONDITION3( tmp_arg && JS_IsTypedArrayObject( tmp_arg ), cx, JS_FALSE, "Not a TypedArray object");
	JSB_PRECONDITION3( JS_GetTypedArrayByteLength( tmp_arg ) == sizeof(long long), cx, JS_FALSE, "Invalid Typed Array length");
	
	uint32_t* arg_array = (uint32_t*)JS_GetArrayBufferViewData( tmp_arg );
	long long ret =  arg_array[0];
	ret = ret << 32;
	ret |= arg_array[1];
	
	*r = ret;
	return JS_TRUE;
}

JSBool jsval_to_std_string(JSContext *cx, jsval v, std::string* ret) {
    JSString *tmp = JS_ValueToString(cx, v);
    JSB_PRECONDITION3(tmp, cx, JS_FALSE, "Error processing arguments");

    JSStringWrapper str(tmp);
    *ret = str.get();
    return JS_TRUE;
}

JSBool jsval_to_ccpoint(JSContext *cx, jsval v, Point* ret) {
    JSObject *tmp;
    jsval jsx, jsy;
    double x, y;
    JSBool ok = v.isObject() &&
        JS_ValueToObject(cx, v, &tmp) &&
        JS_GetProperty(cx, tmp, "x", &jsx) &&
        JS_GetProperty(cx, tmp, "y", &jsy) &&
        JS_ValueToNumber(cx, jsx, &x) &&
        JS_ValueToNumber(cx, jsy, &y);

    JSB_PRECONDITION3(ok, cx, JS_FALSE, "Error processing arguments");

    ret->x = (float)x;
    ret->y = (float)y;
    return JS_TRUE;
}

JSBool jsval_to_ccacceleration(JSContext* cx,jsval v, Acceleration* ret) {
    JSObject *tmp;
    jsval jsx, jsy, jsz, jstimestamp;
    double x, y, timestamp, z;
    JSBool ok = v.isObject() &&
        JS_ValueToObject(cx, v, &tmp) &&
        JS_GetProperty(cx, tmp, "x", &jsx) &&
        JS_GetProperty(cx, tmp, "y", &jsy) &&
        JS_GetProperty(cx, tmp, "z", &jsz) &&
        JS_GetProperty(cx, tmp, "timestamp", &jstimestamp) &&
        JS_ValueToNumber(cx, jsx, &x) &&
        JS_ValueToNumber(cx, jsy, &y) &&
        JS_ValueToNumber(cx, jsz, &z) &&
        JS_ValueToNumber(cx, jstimestamp, &timestamp);

    JSB_PRECONDITION3(ok, cx, JS_FALSE, "Error processing arguments");

    ret->x = x;
    ret->y = y;
    ret->z = z;
    ret->timestamp = timestamp;
    return JS_TRUE;
}

JSBool jsvals_variadic_to_ccarray( JSContext *cx, jsval *vp, int argc, Array** ret)
{
    JSBool ok = JS_TRUE;
    Array* pArray = Array::create();
    for( int i=0; i < argc; i++ )
    {
        double num = 0.0;
        // optimization: JS_ValueToNumber is expensive. And can convert an string like "12" to a number
        if ( JSVAL_IS_NUMBER(*vp)) {
            ok &= JS_ValueToNumber(cx, *vp, &num );
            if (!ok) {
                break;
            }
            pArray->addObject(Integer::create((int)num));
        }
        else if (JSVAL_IS_STRING(*vp))
        {
            JSStringWrapper str(JSVAL_TO_STRING(*vp), cx);
            pArray->addObject(String::create(str));
        }
        else
        {
            js_proxy_t* p;
            JSObject* obj = JSVAL_TO_OBJECT(*vp);
            p = jsb_get_js_proxy(obj);
            if (p) {
                pArray->addObject((Object*)p->ptr);
            }
        }
        // next
        vp++;
    }
    *ret = pArray;
    JSB_PRECONDITION3(ok, cx, JS_FALSE, "Error processing arguments");
    return ok;
}

JSBool jsval_to_ccrect(JSContext *cx, jsval v, Rect* ret) {
    JSObject *tmp;
    jsval jsx, jsy, jswidth, jsheight;
    double x, y, width, height;
    JSBool ok = v.isObject() &&
        JS_ValueToObject(cx, v, &tmp) &&
        JS_GetProperty(cx, tmp, "x", &jsx) &&
        JS_GetProperty(cx, tmp, "y", &jsy) &&
        JS_GetProperty(cx, tmp, "width", &jswidth) &&
        JS_GetProperty(cx, tmp, "height", &jsheight) &&
        JS_ValueToNumber(cx, jsx, &x) &&
        JS_ValueToNumber(cx, jsy, &y) &&
        JS_ValueToNumber(cx, jswidth, &width) &&
        JS_ValueToNumber(cx, jsheight, &height);

    JSB_PRECONDITION3(ok, cx, JS_FALSE, "Error processing arguments");

    ret->origin.x = x;
    ret->origin.y = y;
    ret->size.width = width;
    ret->size.height = height;
    return JS_TRUE;
}

JSBool jsval_to_ccsize(JSContext *cx, jsval v, Size* ret) {
    JSObject *tmp;
    jsval jsw, jsh;
    double w, h;
    JSBool ok = v.isObject() &&
        JS_ValueToObject(cx, v, &tmp) &&
        JS_GetProperty(cx, tmp, "width", &jsw) &&
        JS_GetProperty(cx, tmp, "height", &jsh) &&
        JS_ValueToNumber(cx, jsw, &w) &&
        JS_ValueToNumber(cx, jsh, &h);

    JSB_PRECONDITION3(ok, cx, JS_FALSE, "Error processing arguments");
    ret->width = w;
    ret->height = h;
    return JS_TRUE;
}

JSBool jsval_to_cccolor4b(JSContext *cx, jsval v, Color4B* ret) {
    JSObject *tmp;
    jsval jsr, jsg, jsb, jsa;
    double r, g, b, a;
    JSBool ok = v.isObject() &&
        JS_ValueToObject(cx, v, &tmp) &&
        JS_GetProperty(cx, tmp, "r", &jsr) &&
        JS_GetProperty(cx, tmp, "g", &jsg) &&
        JS_GetProperty(cx, tmp, "b", &jsb) &&
        JS_GetProperty(cx, tmp, "a", &jsa) &&
        JS_ValueToNumber(cx, jsr, &r) &&
        JS_ValueToNumber(cx, jsg, &g) &&
        JS_ValueToNumber(cx, jsb, &b) &&
        JS_ValueToNumber(cx, jsa, &a);

    JSB_PRECONDITION3(ok, cx, JS_FALSE, "Error processing arguments");

    ret->r = r;
    ret->g = g;
    ret->b = b;
    ret->a = a;
    return JS_TRUE;
}

JSBool jsval_to_cccolor4f(JSContext *cx, jsval v, Color4F* ret) {
    JSObject *tmp;
    jsval jsr, jsg, jsb, jsa;
    double r, g, b, a;
    JSBool ok = v.isObject() &&
        JS_ValueToObject(cx, v, &tmp) &&
        JS_GetProperty(cx, tmp, "r", &jsr) &&
        JS_GetProperty(cx, tmp, "g", &jsg) &&
        JS_GetProperty(cx, tmp, "b", &jsb) &&
        JS_GetProperty(cx, tmp, "a", &jsa) &&
        JS_ValueToNumber(cx, jsr, &r) &&
        JS_ValueToNumber(cx, jsg, &g) &&
        JS_ValueToNumber(cx, jsb, &b) &&
        JS_ValueToNumber(cx, jsa, &a);

    JSB_PRECONDITION3(ok, cx, JS_FALSE, "Error processing arguments");
    ret->r = r;
    ret->g = g;
    ret->b = b;
    ret->a = a;
    return JS_TRUE;
}

JSBool jsval_to_cccolor3b(JSContext *cx, jsval v, Color3B* ret) {
    JSObject *tmp;
    jsval jsr, jsg, jsb;
    double r, g, b;
    JSBool ok = v.isObject() &&
        JS_ValueToObject(cx, v, &tmp) &&
        JS_GetProperty(cx, tmp, "r", &jsr) &&
        JS_GetProperty(cx, tmp, "g", &jsg) &&
        JS_GetProperty(cx, tmp, "b", &jsb) &&
        JS_ValueToNumber(cx, jsr, &r) &&
        JS_ValueToNumber(cx, jsg, &g) &&
        JS_ValueToNumber(cx, jsb, &b);

    JSB_PRECONDITION3(ok, cx, JS_FALSE, "Error processing arguments");

    ret->r = r;
    ret->g = g;
    ret->b = b;
    return JS_TRUE;	
}

JSBool jsval_to_ccarray_of_CCPoint(JSContext* cx, jsval v, Point **points, int *numPoints) {
    // Parsing sequence
    JSObject *jsobj;
    JSBool ok = v.isObject() && JS_ValueToObject( cx, v, &jsobj );
    JSB_PRECONDITION3( ok, cx, JS_FALSE, "Error converting value to object");
    JSB_PRECONDITION3( jsobj && JS_IsArrayObject( cx, jsobj), cx, JS_FALSE, "Object must be an array");

    uint32_t len;
    JS_GetArrayLength(cx, jsobj, &len);

    Point *array = (Point*)malloc( sizeof(Point) * len);

    for( uint32_t i=0; i< len;i++ ) {
        jsval valarg;
        JS_GetElement(cx, jsobj, i, &valarg);

        ok = jsval_to_ccpoint(cx, valarg, &array[i]);
        JSB_PRECONDITION3(ok, cx, JS_FALSE, "Error processing arguments");
    }

    *numPoints = len;
    *points = array;

    return JS_TRUE;
}


JSBool jsval_to_ccarray(JSContext* cx, jsval v, Array** ret) {
    JSObject *jsobj;
    JSBool ok = v.isObject() && JS_ValueToObject( cx, v, &jsobj );
    JSB_PRECONDITION3( ok, cx, JS_FALSE, "Error converting value to object");
    JSB_PRECONDITION3( jsobj && JS_IsArrayObject( cx, jsobj),  cx, JS_FALSE, "Object must be an array");

    uint32_t len = 0;
    JS_GetArrayLength(cx, jsobj, &len);
    Array* arr = Array::createWithCapacity(len);
    for (uint32_t i=0; i < len; i++) {
        jsval value;
        if (JS_GetElement(cx, jsobj, i, &value)) {
            if (value.isObject())
            {
                js_proxy_t *proxy;
                JSObject *tmp = JSVAL_TO_OBJECT(value);
                proxy = jsb_get_js_proxy(tmp);
                cocos2d::Object* cobj = (cocos2d::Object *)(proxy ? proxy->ptr : NULL);
                // Don't test it.
                //TEST_NATIVE_OBJECT(cx, cobj)
                if (cobj) {
                    // It's a native js object.
                    arr->addObject(cobj);
                }
                else if (!JS_IsArrayObject(cx, tmp)){
                    // It's a normal js object.
                    Dictionary* dictVal = NULL;
                    JSBool ok = jsval_to_ccdictionary(cx, value, &dictVal);
                    if (ok) {
                        arr->addObject(dictVal);
                    }
                }
                else {
                    // It's a js array object.
                    Array* arrVal = NULL;
                    JSBool ok = jsval_to_ccarray(cx, value, &arrVal);
                    if (ok) {
                        arr->addObject(arrVal);
                    }
                }
            }
            else if (JSVAL_IS_STRING(value)) {
                JSStringWrapper valueWapper(JSVAL_TO_STRING(value), cx);
                arr->addObject(String::create(valueWapper.get()));
//                CCLOG("iterate array: value = %s", valueWapper.get().c_str());
            }
            else if (JSVAL_IS_NUMBER(value)) {
                double number = 0.0;
                JSBool ok = JS_ValueToNumber(cx, value, &number);
                if (ok) {
                    arr->addObject(Double::create(number));
//                    CCLOG("iterate array: value = %lf", number);
                }
            }
            else if (JSVAL_IS_BOOLEAN(value)) {
                JSBool boolVal = JS_FALSE;
                JSBool ok = JS_ValueToBoolean(cx, value, &boolVal);
                if (ok) {
                    arr->addObject(Bool::create(boolVal));
//                    CCLOG("iterate object: value = %d", boolVal);
                }
            }
            else {
                CCASSERT(false, "not supported type");
            }
        }
    }
    *ret = arr;
    return JS_TRUE;
}


jsval ccarray_to_jsval(JSContext* cx, Array *arr)
{
    JSObject *jsretArr = JS_NewArrayObject(cx, 0, NULL);

    Object* obj;
    int i = 0;
    CCARRAY_FOREACH(arr, obj)
    {
        jsval arrElement;

        //First, check whether object is associated with js object.
        js_proxy_t* jsproxy = js_get_or_create_proxy<cocos2d::Object>(cx, obj);
        if (jsproxy) {
            arrElement = OBJECT_TO_JSVAL(jsproxy->obj);
        }
        else {
            String* strVal = NULL;
            Dictionary* dictVal = NULL;
            Array* arrVal = NULL;
            Double* doubleVal = NULL;
            Bool* boolVal = NULL;
            Float* floatVal = NULL;
            Integer* intVal = NULL;
            
            if ((strVal = dynamic_cast<cocos2d::String *>(obj))) {
                arrElement = c_string_to_jsval(cx, strVal->getCString());
            } else if ((dictVal = dynamic_cast<cocos2d::Dictionary*>(obj))) {
                arrElement = ccdictionary_to_jsval(cx, dictVal);
            } else if ((arrVal = dynamic_cast<cocos2d::Array*>(obj))) {
                arrElement = ccarray_to_jsval(cx, arrVal);
            } else if ((doubleVal = dynamic_cast<Double*>(obj))) {
                arrElement = DOUBLE_TO_JSVAL(doubleVal->getValue());
            } else if ((floatVal = dynamic_cast<Float*>(obj))) {
                arrElement = DOUBLE_TO_JSVAL(floatVal->getValue());
            } else if ((intVal = dynamic_cast<Integer*>(obj))) {
                arrElement = INT_TO_JSVAL(intVal->getValue());
            }  else if ((boolVal = dynamic_cast<Bool*>(obj))) {
                arrElement = BOOLEAN_TO_JSVAL(boolVal->getValue() ? JS_TRUE : JS_FALSE);
            } else {
                CCASSERT(false, "the type isn't suppored.");
            }
        }
        if (!JS_SetElement(cx, jsretArr, i, &arrElement)) {
            break;
        }
        ++i;
    }
    return OBJECT_TO_JSVAL(jsretArr);
}

jsval ccdictionary_to_jsval(JSContext* cx, Dictionary* dict)
{
    JSObject* jsRet = JS_NewObject(cx, NULL, NULL, NULL);
    DictElement* pElement = NULL;
    CCDICT_FOREACH(dict, pElement)
    {
        jsval dictElement;
        Object* obj = pElement->getObject();
        //First, check whether object is associated with js object.
        js_proxy_t* jsproxy = js_get_or_create_proxy<cocos2d::Object>(cx, obj);
        if (jsproxy) {
            dictElement = OBJECT_TO_JSVAL(jsproxy->obj);
        }
        else {
            String* strVal = NULL;
            Dictionary* dictVal = NULL;
            Array* arrVal = NULL;
            Double* doubleVal = NULL;
            Bool* boolVal = NULL;
            Float* floatVal = NULL;
            Integer* intVal = NULL;
            
            if ((strVal = dynamic_cast<cocos2d::String *>(obj))) {
                dictElement = c_string_to_jsval(cx, strVal->getCString());
            } else if ((dictVal = dynamic_cast<Dictionary*>(obj))) {
                dictElement = ccdictionary_to_jsval(cx, dictVal);
            } else if ((arrVal = dynamic_cast<Array*>(obj))) {
                dictElement = ccarray_to_jsval(cx, arrVal);
            } else if ((doubleVal = dynamic_cast<Double*>(obj))) {
                dictElement = DOUBLE_TO_JSVAL(doubleVal->getValue());
            } else if ((floatVal = dynamic_cast<Float*>(obj))) {
                dictElement = DOUBLE_TO_JSVAL(floatVal->getValue());
            } else if ((intVal = dynamic_cast<Integer*>(obj))) {
                dictElement = INT_TO_JSVAL(intVal->getValue());
            } else if ((boolVal = dynamic_cast<Bool*>(obj))) {
                dictElement = BOOLEAN_TO_JSVAL(boolVal->getValue() ? JS_TRUE : JS_FALSE);
            } else {
                CCASSERT(false, "the type isn't suppored.");
            }
        }
        const char* key = pElement->getStrKey();
        if (key && strlen(key) > 0)
        {
            JS_SetProperty(cx, jsRet, key, &dictElement);
        }
    }
    return OBJECT_TO_JSVAL(jsRet);
}

JSBool jsval_to_ccdictionary(JSContext* cx, jsval v, Dictionary** ret) {

    if (JSVAL_IS_NULL(v) || JSVAL_IS_VOID(v))
    {
        *ret = NULL;
        return JS_TRUE;
    }

    JSObject* tmp = JSVAL_TO_OBJECT(v);
    if (!tmp) {
        LOGD("jsval_to_ccdictionary: the jsval is not an object.");
        return JS_FALSE;
    }
    
    JSObject* it = JS_NewPropertyIterator(cx, tmp);
    Dictionary* dict = NULL;

    while (true)
    {
        jsid idp;
        jsval key;
        if (! JS_NextProperty(cx, it, &idp) || ! JS_IdToValue(cx, idp, &key)) {
            return JS_FALSE; // error
        }

        if (key == JSVAL_VOID) {
            break; // end of iteration
        }
        
        if (!JSVAL_IS_STRING(key)) {
            continue; // ignore integer properties
        }
        
        JSStringWrapper keyWrapper(JSVAL_TO_STRING(key), cx);
        if (!dict) {
            dict = Dictionary::create();
        }
        
        jsval value;
        JS_GetPropertyById(cx, tmp, idp, &value);
        if (value.isObject())
        {
            js_proxy_t *proxy;
            JSObject *tmp = JSVAL_TO_OBJECT(value);
            proxy = jsb_get_js_proxy(tmp);
            cocos2d::Object* cobj = (cocos2d::Object *)(proxy ? proxy->ptr : NULL);
            // Don't test it.
            //TEST_NATIVE_OBJECT(cx, cobj)
            if (cobj) {
                // It's a native <-> js glue object.
                dict->setObject(cobj, keyWrapper.get());
            }
            else if (!JS_IsArrayObject(cx, tmp)){
                // It's a normal js object.
                Dictionary* dictVal = NULL;
                JSBool ok = jsval_to_ccdictionary(cx, value, &dictVal);
                if (ok) {
                    dict->setObject(dictVal, keyWrapper.get());
                }
            }
            else {
                // It's a js array object.
                Array* arrVal = NULL;
                JSBool ok = jsval_to_ccarray(cx, value, &arrVal);
                if (ok) {
                    dict->setObject(arrVal, keyWrapper.get());
                }
            }
        }
        else if (JSVAL_IS_STRING(value)) {
            JSStringWrapper valueWapper(JSVAL_TO_STRING(value), cx);
            dict->setObject(String::create(valueWapper.get()), keyWrapper.get());
//            CCLOG("iterate object: key = %s, value = %s", keyWrapper.get().c_str(), valueWapper.get().c_str());
        }
        else if (JSVAL_IS_NUMBER(value)) {
            double number = 0.0;
            JSBool ok = JS_ValueToNumber(cx, value, &number);
            if (ok) {
                dict->setObject(Double::create(number), keyWrapper.get());
//                CCLOG("iterate object: key = %s, value = %lf", keyWrapper.get().c_str(), number);
            }
        }
        else if (JSVAL_IS_BOOLEAN(value)) {
            JSBool boolVal = JS_FALSE;
            JSBool ok = JS_ValueToBoolean(cx, value, &boolVal);
            if (ok) {
                dict->setObject(Bool::create(boolVal), keyWrapper.get());
//                CCLOG("iterate object: key = %s, value = %d", keyWrapper.get().c_str(), boolVal);
            }
        }
        else {
            CCASSERT(false, "not supported type");
        }
    }

    *ret = dict;
    return JS_TRUE;
}

JSBool jsval_to_ccaffinetransform(JSContext* cx, jsval v, AffineTransform* ret)
{
    JSObject *tmp;
    jsval jsa, jsb, jsc, jsd, jstx, jsty;
    double a, b, c, d, tx, ty;
    JSBool ok = JS_ValueToObject(cx, v, &tmp) &&
    JS_GetProperty(cx, tmp, "a", &jsa) &&
    JS_GetProperty(cx, tmp, "b", &jsb) &&
    JS_GetProperty(cx, tmp, "c", &jsc) &&
    JS_GetProperty(cx, tmp, "d", &jsd) &&
    JS_GetProperty(cx, tmp, "tx", &jstx) &&
    JS_GetProperty(cx, tmp, "ty", &jsty) &&
    JS_ValueToNumber(cx, jsa, &a) &&
    JS_ValueToNumber(cx, jsb, &b) &&
    JS_ValueToNumber(cx, jsc, &c) &&
    JS_ValueToNumber(cx, jsd, &d) &&
    JS_ValueToNumber(cx, jstx, &tx) &&
    JS_ValueToNumber(cx, jsty, &ty);
    
    JSB_PRECONDITION3(ok, cx, JS_FALSE, "Error processing arguments");
    
    *ret = AffineTransformMake(a, b, c, d, tx, ty);
    return JS_TRUE;
}

// From native type to jsval
jsval int32_to_jsval( JSContext *cx, int32_t number )
{
    return INT_TO_JSVAL(number);
}

jsval uint32_to_jsval( JSContext *cx, uint32_t number )
{
    return UINT_TO_JSVAL(number);
}

jsval long_long_to_jsval(JSContext* cx, long long v) {
    JSObject *tmp = JS_NewUint32Array(cx, 2);
    uint32_t *data = (uint32_t *)JS_GetArrayBufferViewData(tmp);
    data[0] = ((uint32_t *)(&v))[0];
    data[1] = ((uint32_t *)(&v))[1];
    return OBJECT_TO_JSVAL(tmp);
}

jsval std_string_to_jsval(JSContext* cx, const std::string& v)
{
    return c_string_to_jsval(cx, v.c_str(), v.size());
}

jsval c_string_to_jsval(JSContext* cx, const char* v, size_t length /* = -1 */)
{
    if (v == NULL)
    {
        return JSVAL_NULL;
    }
    
    if (0 == length)
    {
        auto emptyStr = JS_NewStringCopyZ(cx, "");
        return STRING_TO_JSVAL(emptyStr);
    }
    
    jsval ret = JSVAL_NULL;
    int utf16_size = 0;
    jschar* strUTF16 = (jschar*)cc_utf8_to_utf16(v, length, &utf16_size);

    if (strUTF16 && utf16_size > 0) {
        JSString* str = JS_NewUCStringCopyN(cx, strUTF16, utf16_size);
        if (str) {
            ret = STRING_TO_JSVAL(str);
        }
        delete[] strUTF16;
    }
    return ret;
}

jsval ccpoint_to_jsval(JSContext* cx, const Point& v) {
    JSObject *tmp = JS_NewObject(cx, NULL, NULL, NULL);
    if (!tmp) return JSVAL_NULL;
    JSBool ok = JS_DefineProperty(cx, tmp, "x", DOUBLE_TO_JSVAL(v.x), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT) &&
                JS_DefineProperty(cx, tmp, "y", DOUBLE_TO_JSVAL(v.y), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT);
    if (ok) {
        return OBJECT_TO_JSVAL(tmp);
    }
    return JSVAL_NULL;
}

jsval ccacceleration_to_jsval(JSContext* cx, const Acceleration& v) {
    JSObject *tmp = JS_NewObject(cx, NULL, NULL, NULL);
    if (!tmp) return JSVAL_NULL;
    JSBool ok = JS_DefineProperty(cx, tmp, "x", DOUBLE_TO_JSVAL(v.x), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT) &&
    JS_DefineProperty(cx, tmp, "y", DOUBLE_TO_JSVAL(v.y), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT) &&
    JS_DefineProperty(cx, tmp, "z", DOUBLE_TO_JSVAL(v.z), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT) &&
    JS_DefineProperty(cx, tmp, "timestamp", DOUBLE_TO_JSVAL(v.timestamp), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT);
    if (ok) {
        return OBJECT_TO_JSVAL(tmp);
    }
    return JSVAL_NULL;
}

jsval ccrect_to_jsval(JSContext* cx, const Rect& v) {
    JSObject *tmp = JS_NewObject(cx, NULL, NULL, NULL);
    if (!tmp) return JSVAL_NULL;
    JSBool ok = JS_DefineProperty(cx, tmp, "x", DOUBLE_TO_JSVAL(v.origin.x), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT) &&
                JS_DefineProperty(cx, tmp, "y", DOUBLE_TO_JSVAL(v.origin.y), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT) &&
                JS_DefineProperty(cx, tmp, "width", DOUBLE_TO_JSVAL(v.size.width), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT) &&
                JS_DefineProperty(cx, tmp, "height", DOUBLE_TO_JSVAL(v.size.height), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT);
    if (ok) {
        return OBJECT_TO_JSVAL(tmp);
    }
    return JSVAL_NULL;
}

jsval ccsize_to_jsval(JSContext* cx, const Size& v) {
    JSObject *tmp = JS_NewObject(cx, NULL, NULL, NULL);
    if (!tmp) return JSVAL_NULL;
    JSBool ok = JS_DefineProperty(cx, tmp, "width", DOUBLE_TO_JSVAL(v.width), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT) &&
                JS_DefineProperty(cx, tmp, "height", DOUBLE_TO_JSVAL(v.height), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT);
    if (ok) {
        return OBJECT_TO_JSVAL(tmp);
    }
    return JSVAL_NULL;
}

jsval cccolor4b_to_jsval(JSContext* cx, const Color4B& v) {
    JSObject *tmp = JS_NewObject(cx, NULL, NULL, NULL);
    if (!tmp) return JSVAL_NULL;
    JSBool ok = JS_DefineProperty(cx, tmp, "r", INT_TO_JSVAL(v.r), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT) &&
                JS_DefineProperty(cx, tmp, "g", INT_TO_JSVAL(v.g), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT) &&
                JS_DefineProperty(cx, tmp, "b", INT_TO_JSVAL(v.g), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT) &&
                JS_DefineProperty(cx, tmp, "a", INT_TO_JSVAL(v.g), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT);
    if (ok) {
        return OBJECT_TO_JSVAL(tmp);
    }
    return JSVAL_NULL;
}

jsval cccolor4f_to_jsval(JSContext* cx, const Color4F& v) {
    JSObject *tmp = JS_NewObject(cx, NULL, NULL, NULL);
    if (!tmp) return JSVAL_NULL;
    JSBool ok = JS_DefineProperty(cx, tmp, "r", DOUBLE_TO_JSVAL(v.r), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT) &&
                JS_DefineProperty(cx, tmp, "g", DOUBLE_TO_JSVAL(v.g), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT) &&
                JS_DefineProperty(cx, tmp, "b", DOUBLE_TO_JSVAL(v.g), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT) &&
                JS_DefineProperty(cx, tmp, "a", DOUBLE_TO_JSVAL(v.g), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT);
    if (ok) {
        return OBJECT_TO_JSVAL(tmp);
    }
    return JSVAL_NULL;
}

jsval cccolor3b_to_jsval(JSContext* cx, const Color3B& v) {
    JSObject *tmp = JS_NewObject(cx, NULL, NULL, NULL);
    if (!tmp) return JSVAL_NULL;
    JSBool ok = JS_DefineProperty(cx, tmp, "r", INT_TO_JSVAL(v.r), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT) &&
                JS_DefineProperty(cx, tmp, "g", INT_TO_JSVAL(v.g), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT) &&
                JS_DefineProperty(cx, tmp, "b", INT_TO_JSVAL(v.g), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT);
    if (ok) {
        return OBJECT_TO_JSVAL(tmp);
    }
    return JSVAL_NULL;
}

jsval ccaffinetransform_to_jsval(JSContext* cx, const AffineTransform& t)
{
    JSObject *tmp = JS_NewObject(cx, NULL, NULL, NULL);
    if (!tmp) return JSVAL_NULL;
    JSBool ok = JS_DefineProperty(cx, tmp, "a", DOUBLE_TO_JSVAL(t.a), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT) &&
    JS_DefineProperty(cx, tmp, "b", DOUBLE_TO_JSVAL(t.b), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT) &&
    JS_DefineProperty(cx, tmp, "c", DOUBLE_TO_JSVAL(t.c), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT) &&
    JS_DefineProperty(cx, tmp, "d", DOUBLE_TO_JSVAL(t.d), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT) &&
    JS_DefineProperty(cx, tmp, "tx", DOUBLE_TO_JSVAL(t.tx), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT) &&
    JS_DefineProperty(cx, tmp, "ty", DOUBLE_TO_JSVAL(t.ty), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT);
    if (ok) {
        return OBJECT_TO_JSVAL(tmp);
    }
    return JSVAL_NULL;
}

jsval FontDefinition_to_jsval(JSContext* cx, const FontDefinition& t)
{
    JSObject *tmp = JS_NewObject(cx, NULL, NULL, NULL);
    if (!tmp) return JSVAL_NULL;
    JSBool ok = JS_TRUE;
    
    ok &= JS_DefineProperty(cx, tmp, "fontName", std_string_to_jsval(cx, t._fontName), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT);
    
    ok &= JS_DefineProperty(cx, tmp, "fontSize", int32_to_jsval(cx, t._fontSize), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT);
    
    ok &= JS_DefineProperty(cx, tmp, "fontAlignmentH", int32_to_jsval(cx, (int32_t)t._alignment), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT);
    
    ok &= JS_DefineProperty(cx, tmp, "fontAlignmentV", int32_to_jsval(cx, (int32_t)t._vertAlignment), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT);

    ok &= JS_DefineProperty(cx, tmp, "fontFillColor", cccolor3b_to_jsval(cx, t._fontFillColor), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT);
    
    ok &= JS_DefineProperty(cx, tmp, "fontDimensions", ccsize_to_jsval(cx, t._dimensions), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT);
    
    // Shadow
    ok &= JS_DefineProperty(cx, tmp, "shadowEnabled", BOOLEAN_TO_JSVAL(t._shadow._shadowEnabled), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT);
    
    ok &= JS_DefineProperty(cx, tmp, "shadowOffset", ccsize_to_jsval(cx, t._shadow._shadowOffset), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT);
 
    ok &= JS_DefineProperty(cx, tmp, "shadowBlur", DOUBLE_TO_JSVAL(t._shadow._shadowBlur), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT);
 
    ok &= JS_DefineProperty(cx, tmp, "shadowOpacity", DOUBLE_TO_JSVAL(t._shadow._shadowOpacity), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT);
    
    // Stroke
    ok &= JS_DefineProperty(cx, tmp, "strokeEnabled", BOOLEAN_TO_JSVAL(t._stroke._strokeEnabled), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT);

    ok &= JS_DefineProperty(cx, tmp, "strokeColor", cccolor3b_to_jsval(cx, t._stroke._strokeColor), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT);
    
    ok &= JS_DefineProperty(cx, tmp, "strokeSize", DOUBLE_TO_JSVAL(t._stroke._strokeSize), NULL, NULL, JSPROP_ENUMERATE | JSPROP_PERMANENT);
    
    if (ok) {
        return OBJECT_TO_JSVAL(tmp);
    }
    return JSVAL_NULL;
}

#pragma mark - Debug

void SimpleRunLoop::update(float dt)
{
    g_qMutex.lock();
    size_t size = g_queue.size();
    g_qMutex.unlock();
    
    while (size > 0)
    {
        g_qMutex.lock();
        vector<string>::iterator first = g_queue.begin();
        string str = *first;
        g_queue.erase(first);
        size = g_queue.size();
        g_qMutex.unlock();
        
        ScriptingCore::getInstance()->debugProcessInput(str);
    }
}

void ScriptingCore::debugProcessInput(string str) {
    JSAutoCompartment ac(_cx, _debugGlobal);
    
    JSString* jsstr = JS_NewStringCopyZ(_cx, str.c_str());
    jsval argv[] = {
        STRING_TO_JSVAL(jsstr)
    };
    jsval outval;
    
    JS_CallFunctionName(_cx, _debugGlobal, "processInput", 1, argv, &outval);
}

static bool NS_ProcessNextEvent()
{
    g_qMutex.lock();
    size_t size = g_queue.size();
    g_qMutex.unlock();
    
    while (size > 0)
    {
        g_qMutex.lock();
        vector<string>::iterator first = g_queue.begin();
        string str = *first;
        g_queue.erase(first);
        size = g_queue.size();
        g_qMutex.unlock();
        
        ScriptingCore::getInstance()->debugProcessInput(str);
    }
//    std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    return true;
}

JSBool JSBDebug_enterNestedEventLoop(JSContext* cx, unsigned argc, jsval* vp)
{
    enum {
        NS_OK = 0,
        NS_ERROR_UNEXPECTED
    };
    
#define NS_SUCCEEDED(v) ((v) == NS_OK)
    
    int rv = NS_OK;
    
    uint32_t nestLevel = ++s_nestedLoopLevel;

    while (NS_SUCCEEDED(rv) && s_nestedLoopLevel >= nestLevel) {
        if (!NS_ProcessNextEvent())
            rv = NS_ERROR_UNEXPECTED;
    }
    
    CCASSERT(s_nestedLoopLevel <= nestLevel,
             "nested event didn't unwind properly");
    
    JS_SET_RVAL(cx, vp, UINT_TO_JSVAL(s_nestedLoopLevel));
    return JS_TRUE;
}

JSBool JSBDebug_exitNestedEventLoop(JSContext* cx, unsigned argc, jsval* vp)
{
    if (s_nestedLoopLevel > 0) {
        --s_nestedLoopLevel;
    } else {
        JS_SET_RVAL(cx, vp, UINT_TO_JSVAL(0));
        return JS_TRUE;
    }
    
    JS_SET_RVAL(cx, vp, UINT_TO_JSVAL(s_nestedLoopLevel));
    return JS_TRUE;
}

JSBool JSBDebug_getEventLoopNestLevel(JSContext* cx, unsigned argc, jsval* vp)
{
    JS_SET_RVAL(cx, vp, UINT_TO_JSVAL(s_nestedLoopLevel));
    return JS_TRUE;
}

//#pragma mark - Debugger

static void _clientSocketWriteAndClearString(std::string& s)
{
    ::send(clientSocket, s.c_str(), s.length(), 0);
    s.clear();
}

static void processInput(string data) {
    std::lock_guard<std::mutex> lk(g_qMutex);
    g_queue.push_back(data);
}

static void clearBuffers() {
    std::lock_guard<std::mutex> lk(g_rwMutex);
    // only process input if there's something and we're not locked
    if (inData.length() > 0) {
        processInput(inData);
        inData.clear();
    }
    if (outData.length() > 0) {
        _clientSocketWriteAndClearString(outData);
    }
}

static void serverEntryPoint(void)
{
    // start a server, accept the connection and keep reading data from it
    struct addrinfo hints, *result = nullptr, *rp = nullptr;
    int s = 0;
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_INET;       // IPv4
    hints.ai_socktype = SOCK_STREAM; // TCP stream sockets
    hints.ai_flags = AI_PASSIVE;     // fill in my IP for me
    
    stringstream portstr;
    portstr << JSB_DEBUGGER_PORT;
    
    int err = 0;
    
#if (CC_TARGET_PLATFORM == CC_PLATFORM_WIN32)
    WSADATA wsaData;
    err = WSAStartup(MAKEWORD(2, 2),&wsaData);
#endif
    
    if ((err = getaddrinfo(NULL, portstr.str().c_str(), &hints, &result)) != 0) {
        LOGD("getaddrinfo error : %s\n", gai_strerror(err));
    }
    
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        if ((s = socket(rp->ai_family, rp->ai_socktype, 0)) < 0) {
            continue;
        }
        int optval = 1;
        if ((setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char*)&optval, sizeof(optval))) < 0) {
            close(s);
			TRACE_DEBUGGER_SERVER("debug server : error setting socket option SO_REUSEADDR");
            return;
        }
        
#if (CC_TARGET_PLATFORM == CC_PLATFORM_IOS)
		if ((setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &optval, sizeof(optval))) < 0) {
			close(s);
			TRACE_DEBUGGER_SERVER("debug server : error setting socket option SO_NOSIGPIPE");
			return;
		}
#endif //(CC_TARGET_PLATFORM == CC_PLATFORM_IOS)
        
        if ((::bind(s, rp->ai_addr, rp->ai_addrlen)) == 0) {
            break;
        }
        close(s);
        s = -1;
    }
    if (s < 0 || rp == NULL) {
		TRACE_DEBUGGER_SERVER("debug server : error creating/binding socket");
        return;
    }
    
    freeaddrinfo(result);
    
    listen(s, 1);
    
	while (true) {
        clientSocket = accept(s, NULL, NULL);
        
        if (clientSocket < 0)
        {
            TRACE_DEBUGGER_SERVER("debug server : error on accept");
            return;
        }
        else
        {
            // read/write data
            TRACE_DEBUGGER_SERVER("debug server : client connected");
            
            inData = "connected";
            // process any input, send any output
            clearBuffers();
            
            char buf[1024] = {0};
            int readBytes = 0;
            while ((readBytes = ::recv(clientSocket, buf, sizeof(buf), 0)) > 0)
            {
                buf[readBytes] = '\0';
                // TRACE_DEBUGGER_SERVER("debug server : received command >%s", buf);
                
                // no other thread is using this
                inData.append(buf);
                // process any input, send any output
                clearBuffers();
            } // while(read)
            
            close(clientSocket);
        }
	} // while(true)
}

JSBool JSBDebug_BufferWrite(JSContext* cx, unsigned argc, jsval* vp)
{
    if (argc == 1) {
        jsval* argv = JS_ARGV(cx, vp);
        JSStringWrapper strWrapper(argv[0]);
        // this is safe because we're already inside a lock (from clearBuffers)
        outData.append(strWrapper.get());
        _clientSocketWriteAndClearString(outData);
    }
    return JS_TRUE;
}

void ScriptingCore::enableDebugger()
{
    JS_SetDebugMode(_cx, JS_TRUE);
    
    if (_debugGlobal == NULL)
    {
        JSAutoCompartment ac0(_cx, _global);
        _debugGlobal = NewGlobalObject(_cx, true);
        JS_WrapObject(_cx, &_debugGlobal);
        JSAutoCompartment ac(_cx, _debugGlobal);
        // these are used in the debug program
        JS_DefineFunction(_cx, _debugGlobal, "log", ScriptingCore::log, 0, JSPROP_READONLY | JSPROP_PERMANENT);
        JS_DefineFunction(_cx, _debugGlobal, "_bufferWrite", JSBDebug_BufferWrite, 1, JSPROP_READONLY | JSPROP_PERMANENT);
        JS_DefineFunction(_cx, _debugGlobal, "_enterNestedEventLoop", JSBDebug_enterNestedEventLoop, 0, JSPROP_READONLY | JSPROP_PERMANENT);
        JS_DefineFunction(_cx, _debugGlobal, "_exitNestedEventLoop", JSBDebug_exitNestedEventLoop, 0, JSPROP_READONLY | JSPROP_PERMANENT);
        JS_DefineFunction(_cx, _debugGlobal, "_getEventLoopNestLevel", JSBDebug_getEventLoopNestLevel, 0, JSPROP_READONLY | JSPROP_PERMANENT);
        
        
        runScript("jsb_debugger.js", _debugGlobal);
        
        // prepare the debugger
        jsval argv = OBJECT_TO_JSVAL(_global);
        jsval outval;
        JSBool ok = JS_CallFunctionName(_cx, _debugGlobal, "_prepareDebugger", 1, &argv, &outval);
        if (!ok) {
            JS_ReportPendingException(_cx);
        }
        
        // start bg thread
        auto t = std::thread(&serverEntryPoint);
        t.detach();

        Scheduler* scheduler = Director::getInstance()->getScheduler();
        scheduler->scheduleUpdateForTarget(this->_runLoop, 0, false);
    }
}

JSObject* NewGlobalObject(JSContext* cx, bool debug)
{
    JSObject* glob = JS_NewGlobalObject(cx, &global_class, NULL);
    if (!glob) {
        return NULL;
    }
    JSAutoCompartment ac(cx, glob);
    JSBool ok = JS_TRUE;
    ok = JS_InitStandardClasses(cx, glob);
    if (ok)
        JS_InitReflect(cx, glob);
    if (ok && debug)
        ok = JS_DefineDebuggerObject(cx, glob);
    if (!ok)
        return NULL;

    return glob;
}

JSBool jsb_set_reserved_slot(JSObject *obj, uint32_t idx, jsval value)
{
    JSClass *klass = JS_GetClass(obj);
    unsigned int slots = JSCLASS_RESERVED_SLOTS(klass);
    if ( idx >= slots )
        return JS_FALSE;

    JS_SetReservedSlot(obj, idx, value);

    return JS_TRUE;
}

JSBool jsb_get_reserved_slot(JSObject *obj, uint32_t idx, jsval& ret)
{
    JSClass *klass = JS_GetClass(obj);
    unsigned int slots = JSCLASS_RESERVED_SLOTS(klass);
    if ( idx >= slots )
        return JS_FALSE;

    ret = JS_GetReservedSlot(obj, idx);

    return JS_TRUE;
}

js_proxy_t* jsb_new_proxy(void* nativeObj, JSObject* jsObj)
{
    js_proxy_t* p;
    JS_NEW_PROXY(p, nativeObj, jsObj);
    return p;
}

js_proxy_t* jsb_get_native_proxy(void* nativeObj)
{
    js_proxy_t* p;
    JS_GET_PROXY(p, nativeObj);
    return p;
}

js_proxy_t* jsb_get_js_proxy(JSObject* jsObj)
{
    js_proxy_t* p;
    JS_GET_NATIVE_PROXY(p, jsObj);
    return p;
}

void jsb_remove_proxy(js_proxy_t* nativeProxy, js_proxy_t* jsProxy)
{
    JS_REMOVE_PROXY(nativeProxy, jsProxy);
}

static Color3B getColorFromJSObject(JSContext *cx, JSObject *colorObject)
{
    jsval jsr;
    Color3B out;
    JS_GetProperty(cx, colorObject, "r", &jsr);
    double fontR = 0.0;
    JS_ValueToNumber(cx, jsr, &fontR);
    
    JS_GetProperty(cx, colorObject, "g", &jsr);
    double fontG = 0.0;
    JS_ValueToNumber(cx, jsr, &fontG);
    
    JS_GetProperty(cx, colorObject, "b", &jsr);
    double fontB = 0.0;
    JS_ValueToNumber(cx, jsr, &fontB);
    
    // the out
    out.r = (unsigned char)fontR;
    out.g = (unsigned char)fontG;
    out.b = (unsigned char)fontB;
    
    return out;
}

Size getSizeFromJSObject(JSContext *cx, JSObject *sizeObject)
{
    jsval jsr;
    Size out;
    JS_GetProperty(cx, sizeObject, "width", &jsr);
    double width = 0.0;
    JS_ValueToNumber(cx, jsr, &width);
    
    JS_GetProperty(cx, sizeObject, "height", &jsr);
    double height = 0.0;
    JS_ValueToNumber(cx, jsr, &height);
    
    
    // the out
    out.width  = width;
    out.height = height;
    
    return out;
}

JSBool jsval_to_FontDefinition( JSContext *cx, jsval vp, FontDefinition *out )
{
    JSObject *jsobj;
    
	if (!JS_ValueToObject( cx, vp, &jsobj ) )
		return JS_FALSE;
	
	JSB_PRECONDITION( jsobj, "Not a valid JS object");
    
    // defaul values
    const char *            defautlFontName         = "Arial";
    const int               defaultFontSize         = 32;
    TextHAlignment         defaultTextAlignment    = TextHAlignment::LEFT;
    TextVAlignment defaultTextVAlignment   = TextVAlignment::TOP;
    
    // by default shadow and stroke are off
    out->_shadow._shadowEnabled = false;
    out->_stroke._strokeEnabled = false;
    
    // white text by default
    out->_fontFillColor = Color3B::WHITE;
    
    // font name
    jsval jsr;
    JS_GetProperty(cx, jsobj, "fontName", &jsr);
    JS_ValueToString(cx, jsr);
    JSStringWrapper wrapper(jsr);
    if ( wrapper )
    {
        out->_fontName  = (char*)wrapper;
    }
    else
    {
        out->_fontName  = defautlFontName;
    }
    
    // font size
    JSBool hasProperty;
    JS_HasProperty(cx, jsobj, "fontSize", &hasProperty);
    if ( hasProperty )
    {
        JS_GetProperty(cx, jsobj, "fontSize", &jsr);
        double fontSize = 0.0;
        JS_ValueToNumber(cx, jsr, &fontSize);
        out->_fontSize  = fontSize;
    }
    else
    {
        out->_fontSize  = defaultFontSize;
    }
    
    // font alignment horizontal
    JS_HasProperty(cx, jsobj, "fontAlignmentH", &hasProperty);
    if ( hasProperty )
    {
        JS_GetProperty(cx, jsobj, "fontAlignmentH", &jsr);
        double fontAlign = 0.0;
        JS_ValueToNumber(cx, jsr, &fontAlign);
        out->_alignment = (TextHAlignment)(int)fontAlign;
    }
    else
    {
        out->_alignment  = defaultTextAlignment;
    }
    
    // font alignment vertical
    JS_HasProperty(cx, jsobj, "fontAlignmentV", &hasProperty);
    if ( hasProperty )
    {
        JS_GetProperty(cx, jsobj, "fontAlignmentV", &jsr);
        double fontAlign = 0.0;
        JS_ValueToNumber(cx, jsr, &fontAlign);
        out->_vertAlignment = (TextVAlignment)(int)fontAlign;
    }
    else
    {
        out->_vertAlignment  = defaultTextVAlignment;
    }
    
    // font fill color
    JS_HasProperty(cx, jsobj, "fontFillColor", &hasProperty);
    if ( hasProperty )
    {
        JS_GetProperty(cx, jsobj, "fontFillColor", &jsr);
        
        JSObject *jsobjColor;
        if (!JS_ValueToObject( cx, jsr, &jsobjColor ) )
            return JS_FALSE;
        
        out->_fontFillColor = getColorFromJSObject(cx, jsobjColor);
    }
    
    // font rendering box dimensions
    JS_HasProperty(cx, jsobj, "fontDimensions", &hasProperty);
    if ( hasProperty )
    {
        JS_GetProperty(cx, jsobj, "fontDimensions", &jsr);
        
        JSObject *jsobjSize;
        if (!JS_ValueToObject( cx, jsr, &jsobjSize ) )
            return JS_FALSE;
        
        out->_dimensions = getSizeFromJSObject(cx, jsobjSize);
    }
    
    // shadow
    JS_HasProperty(cx, jsobj, "shadowEnabled", &hasProperty);
    if ( hasProperty )
    {
        JS_GetProperty(cx, jsobj, "shadowEnabled", &jsr);
        out->_shadow._shadowEnabled  = ToBoolean(jsr);
        
        if ( out->_shadow._shadowEnabled )
        {
            // default shadow values
            out->_shadow._shadowOffset  = Size(5, 5);
            out->_shadow._shadowBlur    = 1;
            out->_shadow._shadowOpacity = 1;
            
            // shado offset
            JS_HasProperty(cx, jsobj, "shadowOffset", &hasProperty);
            if ( hasProperty )
            {
                JS_GetProperty(cx, jsobj, "shadowOffset", &jsr);
                
                JSObject *jsobjShadowOffset;
                if (!JS_ValueToObject( cx, jsr, &jsobjShadowOffset ) )
                    return JS_FALSE;
                out->_shadow._shadowOffset = getSizeFromJSObject(cx, jsobjShadowOffset);
            }
            
            // shadow blur
            JS_HasProperty(cx, jsobj, "shadowBlur", &hasProperty);
            if ( hasProperty )
            {
                JS_GetProperty(cx, jsobj, "shadowBlur", &jsr);
                double shadowBlur = 0.0;
                JS_ValueToNumber(cx, jsr, &shadowBlur);
                out->_shadow._shadowBlur = shadowBlur;
            }
            
            // shadow intensity
            JS_HasProperty(cx, jsobj, "shadowOpacity", &hasProperty);
            if ( hasProperty )
            {
                JS_GetProperty(cx, jsobj, "shadowOpacity", &jsr);
                double shadowOpacity = 0.0;
                JS_ValueToNumber(cx, jsr, &shadowOpacity);
                out->_shadow._shadowOpacity = shadowOpacity;
            }
        }
    }
    
    // stroke
    JS_HasProperty(cx, jsobj, "strokeEnabled", &hasProperty);
    if ( hasProperty )
    {
        JS_GetProperty(cx, jsobj, "strokeEnabled", &jsr);
        out->_stroke._strokeEnabled  = ToBoolean(jsr);
        
        if ( out->_stroke._strokeEnabled )
        {
            // default stroke values
            out->_stroke._strokeSize  = 1;
            out->_stroke._strokeColor = Color3B::BLUE;
            
            // stroke color
            JS_HasProperty(cx, jsobj, "strokeColor", &hasProperty);
            if ( hasProperty )
            {
                JS_GetProperty(cx, jsobj, "strokeColor", &jsr);
                
                JSObject *jsobjStrokeColor;
                if (!JS_ValueToObject( cx, jsr, &jsobjStrokeColor ) )
                    return JS_FALSE;
                out->_stroke._strokeColor = getColorFromJSObject(cx, jsobjStrokeColor);
            }
            
            // stroke size
            JS_HasProperty(cx, jsobj, "strokeSize", &hasProperty);
            if ( hasProperty )
            {
                JS_GetProperty(cx, jsobj, "strokeSize", &jsr);
                double strokeSize = 0.0;
                JS_ValueToNumber(cx, jsr, &strokeSize);
                out->_stroke._strokeSize = strokeSize;
            }
        }
    }
    
    // we are done here
	return JS_TRUE;
}
