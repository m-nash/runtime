// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

#include <mono/utils/mono-publib.h>
#include <mono/utils/mono-logger.h>
#include <mono/metadata/appdomain.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/class.h>
#include <mono/metadata/mono-debug.h>
#include <mono/metadata/mono-gc.h>
#include <mono/metadata/exception.h>
#include <mono/metadata/object.h>
#include <mono/jit/jit.h>
#include <mono/jit/mono-private-unstable.h>

#include <sys/stat.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <jni.h>
#include <android/log.h>
#include <sys/system_properties.h>
#include <sys/mman.h>
#include <assert.h>
#include <unistd.h>

/********* exported symbols *********/

/* JNI exports */

void
Java_net_dot_MonoRunner_setEnv (JNIEnv* env, jobject thiz, jstring j_key, jstring j_value);

int
Java_net_dot_MonoRunner_initRuntime (JNIEnv* env, jobject thiz, jstring j_files_dir, jstring j_entryPointLibName, long current_local_time);

int
Java_net_dot_MonoRunner_execEntryPoint (JNIEnv* env, jobject thiz, jstring j_entryPointLibName, jobjectArray j_args);

void
Java_net_dot_MonoRunner_freeNativeResources (JNIEnv* env, jobject thiz);

// called from C#
void
invoke_external_native_api (void (*callback)(void));

/********* implementation *********/

static const char* g_bundle_path = NULL;
static MonoDomain* g_domain = NULL;
static MonoAssembly* g_assembly = NULL;

#define LOG_INFO(fmt, ...) __android_log_print(ANDROID_LOG_DEBUG, "DOTNET", fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) __android_log_print(ANDROID_LOG_ERROR, "DOTNET", fmt, ##__VA_ARGS__)

#if defined(__arm__)
#define ANDROID_RUNTIME_IDENTIFIER "android-arm"
#elif defined(__aarch64__)
#define ANDROID_RUNTIME_IDENTIFIER "android-arm64"
#elif defined(__i386__)
#define ANDROID_RUNTIME_IDENTIFIER "android-x86"
#elif defined(__x86_64__)
#define ANDROID_RUNTIME_IDENTIFIER "android-x64"
#else
#error Unknown architecture
#endif

#define RUNTIMECONFIG_BIN_FILE "runtimeconfig.bin"

static MonoAssembly*
mono_droid_load_assembly (const char *name, const char *culture)
{
    assert (g_bundle_path);
    char filename [1024];
    char path [1024];
    int res;

    LOG_INFO ("assembly_preload_hook: %s %s %s\n", name, culture, g_bundle_path);

    int len = strlen (name);
    int has_extension = len > 3 && name [len - 4] == '.' && (!strcmp ("exe", name + (len - 3)) || !strcmp ("dll", name + (len - 3)));

    // add extensions if required.
    strlcpy (filename, name, sizeof (filename));
    if (!has_extension) {
        strlcat (filename, ".dll", sizeof (filename));
    }

    if (culture && strcmp (culture, ""))
        res = snprintf (path, sizeof (path) - 1, "%s/%s/%s", g_bundle_path, culture, filename);
    else
        res = snprintf (path, sizeof (path) - 1, "%s/%s", g_bundle_path, filename);
    assert (res > 0);

    struct stat buffer;
    if (stat (path, &buffer) == 0) {
        MonoAssembly *assembly = mono_assembly_open (path, NULL);
        assert (assembly);
        return assembly;
    }
    return NULL;
}

static MonoAssembly*
mono_droid_assembly_preload_hook (MonoAssemblyName *aname, char **assemblies_path, void* user_data)
{
    const char *name = mono_assembly_name_get_name (aname);
    const char *culture = mono_assembly_name_get_culture (aname);
    return mono_droid_load_assembly (name, culture);
}

static unsigned char *
load_aot_data (MonoAssembly *assembly, int size, void *user_data, void **out_handle)
{
    assert (g_bundle_path);
    *out_handle = NULL;

    char path [1024];
    int res;

    MonoAssemblyName *assembly_name = mono_assembly_get_name (assembly);
    const char *aname = mono_assembly_name_get_name (assembly_name);

    LOG_INFO ("Looking for aot data for assembly '%s'.", aname);
    res = snprintf (path, sizeof (path) - 1, "%s/%s.aotdata", g_bundle_path, aname);
    assert (res > 0);

    int fd = open (path, O_RDONLY);
    if (fd < 0) {
        LOG_INFO ("Could not load the aot data for %s from %s: %s\n", aname, path, strerror (errno));
        return NULL;
    }

    void *ptr = mmap (NULL, size, PROT_READ, MAP_FILE | MAP_PRIVATE, fd, 0);
    if (ptr == MAP_FAILED) {
        LOG_INFO ("Could not map the aot file for %s: %s\n", aname, strerror (errno));
        close (fd);
        return NULL;
    }

    close (fd);
    LOG_INFO ("Loaded aot data for %s.\n", aname);
    *out_handle = ptr;
    return (unsigned char *) ptr;
}

static void
free_aot_data (MonoAssembly *assembly, int size, void *user_data, void *handle)
{
    munmap (handle, size);
}

static char *
strdup_printf (const char *msg, ...)
{
    va_list args;
    char *formatted = NULL;
    va_start (args, msg);
    vasprintf (&formatted, msg, args);
    va_end (args);
    return formatted;
}

static MonoObject *
mono_droid_fetch_exception_property (MonoObject *obj, const char *name, bool is_virtual)
{
    MonoMethod *get = NULL;
    MonoMethod *get_virt = NULL;
    MonoObject *exc = NULL;

    get = mono_class_get_method_from_name (mono_get_exception_class (), name, 0);
    if (get) {
        if (is_virtual) {
            get_virt = mono_object_get_virtual_method (obj, get);
            if (get_virt)
                get = get_virt;
        }

        return (MonoObject *) mono_runtime_invoke (get, obj, NULL, &exc);
    } else {
        printf ("Could not find the property System.Exception.%s", name);
    }

    return NULL;
}

static char *
mono_droid_fetch_exception_property_string (MonoObject *obj, const char *name, bool is_virtual)
{
    MonoString *str = (MonoString *) mono_droid_fetch_exception_property (obj, name, is_virtual);
    return str ? mono_string_to_utf8 (str) : NULL;
}

static void
unhandled_exception_handler (MonoObject *exc, void *user_data)
{
    MonoClass *type = mono_object_get_class (exc);
    char *type_name = strdup_printf ("%s.%s", mono_class_get_namespace (type), mono_class_get_name (type));
    char *trace = mono_droid_fetch_exception_property_string (exc, "get_StackTrace", true);
    char *message = mono_droid_fetch_exception_property_string (exc, "get_Message", true);

    LOG_ERROR("UnhandledException: %s %s %s", type_name, message, trace);

    free (trace);
    free (message);
    free (type_name);
    exit (1);
}

static void
log_callback (const char *log_domain, const char *log_level, const char *message, mono_bool fatal, void *user_data)
{
    LOG_INFO ("(%s %s) %s", log_domain, log_level, message);
    if (fatal) {
        LOG_ERROR ("Exit code: %d.", 1);
        exit (1);
    }
}

#if defined(FORCE_AOT) && defined(STATIC_AOT)
void register_aot_modules (void);
#endif

static void
cleanup_runtime_config (MonovmRuntimeConfigArguments *args, void *user_data)
{
    free (args);
    free (user_data);
}

static int
mono_droid_runtime_init (const char* executable, int local_date_time_offset)
{
    LOG_INFO ("mono_droid_runtime_init (Mono) called with executable: %s", executable);
    // NOTE: these options can be set via command line args for adb or xharness, see AndroidSampleApp.csproj

    // uncomment for debug output:
    //
    //setenv ("XUNIT_VERBOSE", "true", true);
    //setenv ("MONO_LOG_LEVEL", "debug", true);
    //setenv ("MONO_LOG_MASK", "all", true);

    // build using DiagnosticPorts property in AndroidAppBuilder
    // or set DOTNET_DiagnosticPorts env via adb, xharness when undefined.
    // NOTE, using DOTNET_DiagnosticPorts requires app build using AndroidAppBuilder and RuntimeComponents to include 'diagnostics_tracing' component
#ifdef DIAGNOSTIC_PORTS
    setenv ("DOTNET_DiagnosticPorts", DIAGNOSTIC_PORTS, true);
#endif

    bool wait_for_debugger = false;
    chdir (g_bundle_path);

    // TODO: set TRUSTED_PLATFORM_ASSEMBLIES, APP_PATHS and NATIVE_DLL_SEARCH_DIRECTORIES

    const char* appctx_keys[3];
    appctx_keys[0] = "RUNTIME_IDENTIFIER";
    appctx_keys[1] = "APP_CONTEXT_BASE_DIRECTORY";
    appctx_keys[2] = "System.TimeZoneInfo.LocalDateTimeOffset";

    const char* appctx_values[3];
    appctx_values[0] = ANDROID_RUNTIME_IDENTIFIER;
    appctx_values[1] = g_bundle_path;
    char local_date_time_offset_buffer[32];
    snprintf (local_date_time_offset_buffer, sizeof(local_date_time_offset_buffer), "%d", local_date_time_offset);
    appctx_values[2] = strdup (local_date_time_offset_buffer);

    char *file_name = RUNTIMECONFIG_BIN_FILE;
    int str_len = strlen (g_bundle_path) + strlen (file_name) + 1; // +1 is for the "/"
    char *file_path = (char *)malloc (sizeof (char) * (str_len +1)); // +1 is for the terminating null character
    int num_char = snprintf (file_path, (str_len + 1), "%s/%s", g_bundle_path, file_name);
    struct stat buffer;

    LOG_INFO ("file_path: %s\n", file_path);
    assert (num_char > 0 && num_char == str_len);

    if (stat (file_path, &buffer) == 0) {
        MonovmRuntimeConfigArguments *arg = (MonovmRuntimeConfigArguments *)malloc (sizeof (MonovmRuntimeConfigArguments));
        arg->kind = 0;
        arg->runtimeconfig.name.path = file_path;
        monovm_runtimeconfig_initialize (arg, cleanup_runtime_config, file_path);
    } else {
        free (file_path);
    }

    LOG_INFO ("Calling monovm_initialize");
    int rv = monovm_initialize(3, appctx_keys, appctx_values);
    LOG_INFO ("monovm_initialize returned %d", rv);

    mono_debug_init (MONO_DEBUG_FORMAT_MONO);
    mono_install_assembly_preload_hook (mono_droid_assembly_preload_hook, NULL);
    mono_install_load_aot_data_hook (load_aot_data, free_aot_data, NULL);
    mono_install_unhandled_exception_hook (unhandled_exception_handler, NULL);
    mono_trace_set_log_handler (log_callback, NULL);
    mono_set_signal_chaining (true);
    mono_set_crash_chaining (true);

    if (wait_for_debugger) {
        char* options[] = { "--debugger-agent=transport=dt_socket,server=y,address=0.0.0.0:55556" };
        mono_jit_parse_options (1, options);
    }

#if FORCE_INTERPRETER
    LOG_INFO("Interp Enabled");
    mono_jit_set_aot_mode(MONO_AOT_MODE_INTERP_ONLY);
#elif FORCE_AOT
    LOG_INFO("AOT Enabled");
#if STATIC_AOT
    register_aot_modules();
#endif // STATIC_AOT

#if FULL_AOT
    mono_jit_set_aot_mode(MONO_AOT_MODE_FULL);
#else
    mono_jit_set_aot_mode(MONO_AOT_MODE_NORMAL);
#endif // FULL_AOT
#endif // FORCE_INTERPRETER
    
    g_domain = mono_jit_init_version ("dotnet.android", "mobile");
    if (g_domain == NULL) {
        LOG_ERROR ("mono_jit_init_version failed");
        return -1;
    }

    g_assembly = mono_droid_load_assembly (executable, NULL);
    if (g_assembly == NULL) {
        LOG_ERROR ("mono_droid_load_assembly failed");
        return -1;
    }

    return rv;
}

static void
free_resources ()
{
    if (g_bundle_path)
    {
        free (g_bundle_path);
        g_bundle_path = NULL;
    }
    if (g_assembly)
    {
        mono_assembly_close (g_assembly);
        g_assembly = NULL;
    }
    // Free g_domain
    if (g_domain)
    {
        mono_domain_set (g_domain, false);
        mono_domain_finalize (g_domain, 0);
        g_domain = NULL;
    }
}

static int 
mono_droid_execute_assembly (MonoDomain* domain, MonoAssembly* assembly, int managed_argc, char* managed_argv[])
{
    LOG_INFO ("Calling mono_jit_exec");
    int rv = mono_jit_exec (domain, assembly, managed_argc, managed_argv);
    LOG_INFO ("Exit code: %d.", rv);

    mono_jit_cleanup (domain);

    return rv;
}

static void
strncpy_str (JNIEnv *env, char *buff, jstring str, int nbuff)
{
    jboolean isCopy = 0;
    const char *copy_buff = (*env)->GetStringUTFChars (env, str, &isCopy);
    strncpy (buff, copy_buff, nbuff);
    buff[nbuff - 1] = '\0'; // ensure '\0' terminated
    if (isCopy)
        (*env)->ReleaseStringUTFChars (env, str, copy_buff);
}

void
Java_net_dot_MonoRunner_setEnv (JNIEnv* env, jobject thiz, jstring j_key, jstring j_value)
{
    LOG_INFO ("Java_net_dot_MonoRunner_setEnv:");
    assert (g_domain == NULL); // setenv should be only called before the runtime is initialized
    
    const char *key = (*env)->GetStringUTFChars(env, j_key, 0);
    const char *val = (*env)->GetStringUTFChars(env, j_value, 0);

    LOG_INFO ("Setting env var: %s=%s", key, val);
    setenv (key, val, true);
    (*env)->ReleaseStringUTFChars(env, j_key, key);
    (*env)->ReleaseStringUTFChars(env, j_value, val);
}

int
Java_net_dot_MonoRunner_initRuntime (JNIEnv* env, jobject thiz, jstring j_files_dir, jstring j_entryPointLibName, long current_local_time)
{
    LOG_INFO ("Java_net_dot_MonoRunner_initRuntime (Mono):");
    char file_dir[2048];
    char entryPointLibName[2048];
    strncpy_str (env, file_dir, j_files_dir, sizeof(file_dir));
    strncpy_str (env, entryPointLibName, j_entryPointLibName, sizeof(entryPointLibName));

    size_t file_dir_len = strlen(file_dir);
    char* bundle_path_tmp = (char*)malloc(sizeof(char) * (file_dir_len + 1)); // +1 for '\0'
    if (bundle_path_tmp == NULL)
    {
        LOG_ERROR("Failed to allocate memory for bundle_path");
        return -1;
    }
    strncpy(bundle_path_tmp, file_dir, file_dir_len + 1);
    g_bundle_path = bundle_path_tmp;

    return mono_droid_runtime_init (entryPointLibName, current_local_time);
}

int
Java_net_dot_MonoRunner_execEntryPoint (JNIEnv* env, jobject thiz, jstring j_entryPointLibName, jobjectArray j_args)
{
    LOG_INFO("Java_net_dot_MonoRunner_execEntryPoint (Mono):");

    if (g_bundle_path == NULL)
    {
        LOG_ERROR("Bundle path or executable name not set");
        return -1;
    }

    if (g_domain == NULL || g_assembly == NULL)
    {
        LOG_ERROR("Mono domain or assembly not initialized");
        return -1;
    }

    int args_len = (*env)->GetArrayLength(env, j_args);
    int managed_argc = args_len + 1;
    char** managed_argv = (char**)malloc(managed_argc * sizeof(char*));
    managed_argv[0] = g_bundle_path;
    for (int i = 0; i < args_len; ++i)
    {
        jstring j_arg = (*env)->GetObjectArrayElement(env, j_args, i);
        managed_argv[i + 1] = (char*)((*env)->GetStringUTFChars(env, j_arg, NULL));
    }

    int rv = mono_droid_execute_assembly (g_domain, g_assembly, managed_argc, managed_argv);

    for (int i = 0; i < args_len; ++i)
    {
        jstring j_arg = (*env)->GetObjectArrayElement(env, j_args, i);
        (*env)->ReleaseStringUTFChars(env, j_arg, managed_argv[i + 1]);
    }

    free(managed_argv);
    return rv;
}

void
Java_net_dot_MonoRunner_freeNativeResources (JNIEnv* env, jobject thiz)
{
    LOG_INFO ("Java_net_dot_MonoRunner_freeNativeResources (Mono):");
    free_resources ();
}

// called from C#
void
invoke_external_native_api (void (*callback)(void))
{
    if (callback)
        callback();
}
