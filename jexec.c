/*
 * Program for loading a JVM.
 * Notable environment variables: LD_LIBRARY_PATH
 * Notable jvm parameters: -Djava.class.path=...
 * Main method signature: public static void main(String[] args);
 * Shutdown method signature: public static void shutdown();
 */
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dlfcn.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/capability.h>
#include <pwd.h>
#include <pthread.h>
#include <jni.h>

#define fatal(str...) do { fprintf(stderr, str); exit(50); } while (0)

static pthread_mutex_t exit_lock = PTHREAD_MUTEX_INITIALIZER;

static char* capsText = NULL;
static char* userName = NULL;
static int vmArgsCount;
static char** vmArgs;
static char* className;
static int mainArgsCount;
static char** mainArgs;

static jint (*jni_createvm)(JavaVM**, JNIEnv**, JavaVMInitArgs*);

static JavaVM* jvm;
static JNIEnv* mainEnv;
static jclass mainClass;
static jmethodID mainMethod;
static jmethodID shutdownMethod;

static void syntax()
{
  printf("syntax: jexec [-u user] [-c caps] [jvm options...] <classname> [params...]\n");
  exit(1);
}

static void parse_args(int argc, char *argv[])
{
  int i;
  int vmArgsIndex;
  int classNameIndex;

  for (i = 1; i < argc; i++) {
    if (!strcmp("-u", argv[i])) {
      if (++i >= argc)
	syntax();
      userName = argv[i];
    }
    else if (!strcmp("-c", argv[i])) {
      if (++i >= argc)
	syntax();
      capsText = argv[i];
    }
    else {
      break;
    }
  }
  vmArgsIndex = i;
  for (; i < argc; i++) {
    if (argv[i][0] != '-') {
      classNameIndex = i;
      break;
    }
  }
  if (i >= argc) {
    syntax();
  }
  vmArgsCount = classNameIndex - vmArgsIndex;
  vmArgs = &argv[vmArgsIndex];
  className = argv[classNameIndex];
  mainArgsCount = argc - (classNameIndex + 1);
  mainArgs = &argv[classNameIndex + 1];
}

static void set_user()
{
  if (userName != NULL) {
    struct passwd *pwEnt = getpwnam(userName);
    if (pwEnt == NULL)
      fatal("User not found: %s\n", userName);
    if ((capsText != NULL) && (prctl(PR_SET_KEEPCAPS, 1) != 0))
      fatal("prctl(PR_SET_KEEPCAPS, 1) failed\n");
    if (setregid(pwEnt->pw_gid, pwEnt->pw_gid) != 0)
      fatal("Failed to change group: %s\n", strerror(errno));
    if (setreuid(pwEnt->pw_uid, pwEnt->pw_uid) != 0)
      fatal("Failed to change user: %s\n", strerror(errno));
    if ((capsText != NULL) && (prctl(PR_SET_KEEPCAPS, 0) != 0))
      fatal("prctl(PR_SET_KEEPCAPS, 0) failed\n");
  }
}

static void set_caps()
{
  if (capsText != NULL) {
    cap_t caps = cap_from_text(capsText);
    if (caps == NULL)
      fatal("Failed to parse capabilities: %s\n", capsText);
    if (cap_set_proc(caps) < 0)
      fatal("Failed to set capabilities: %s\n", strerror(errno));
    cap_free(caps);
  }
}

static void load_lib()
{
  void* lib = dlopen("libjvm.so", RTLD_GLOBAL | RTLD_NOW);
  if (lib == NULL)
    fatal("Could not open libjvm.so: %s\n", dlerror());
  jni_createvm = dlsym(lib, "JNI_CreateJavaVM");
  if (jni_createvm == NULL)
    fatal("Could not find symbol JNI_CreateJavaVM: %s\n", dlerror());
}

static void* thread_func(void* arg)
{
  JNIEnv *env;
  pthread_mutex_lock(&exit_lock);
  if ((*jvm)->AttachCurrentThreadAsDaemon(jvm, (void**)&env, NULL) != JNI_OK) {
    fatal("AttachCurrentThreadAsDaemon() failed\n");
  }
  (*env)->CallStaticVoidMethod(env, mainClass, shutdownMethod);
  if ((*env)->ExceptionOccurred(env) != NULL) {
    (*env)->ExceptionDescribe(env);
  }
  (*jvm)->DetachCurrentThread(jvm);
  return NULL;
}

static void create_thread()
{
  pthread_t hthread;
  pthread_mutex_lock(&exit_lock);
  if (pthread_create(&hthread, NULL, thread_func, jvm) != 0)
    fatal("pthread_create() failed\n");
  if (pthread_detach(hthread) != 0)
    fatal("pthread_detach() failed\n");
}

static void create_jvm()
{
  int i;
  JavaVMInitArgs vm_args;

  vm_args.version = JNI_VERSION_1_4;
  vm_args.ignoreUnrecognized = JNI_FALSE;
  vm_args.nOptions = vmArgsCount;
  vm_args.options = malloc(vmArgsCount * sizeof(JavaVMOption));
  for (i = 0; i < vmArgsCount; i++) {
    vm_args.options[i].optionString = vmArgs[i];
    vm_args.options[i].extraInfo = NULL;
  }

  if (jni_createvm(&jvm, &mainEnv, &vm_args) != 0)
    fatal("JNI_CreateJavaVM() failed\n");
  mainClass = (*mainEnv)->FindClass(mainEnv, className);
  if (mainClass == NULL)
    fatal("Main class not found: %s\n", className);
  mainMethod = (*mainEnv)->GetStaticMethodID(mainEnv, mainClass, "main", "([Ljava/lang/String;)V");
  if (mainMethod == NULL)
    fatal("Main method not found in: %s\n", className);
  shutdownMethod = (*mainEnv)->GetStaticMethodID(mainEnv, mainClass, "shutdown", "()V");
  if (shutdownMethod == NULL)
    fatal("Shutdown method not found in: %s\n", className);
}

static void sighandler(int num)
{
  pthread_mutex_unlock(&exit_lock);
}

static void install_sighandler()
{
  struct sigaction sa;
  sa.sa_flags = 0;
  sigemptyset(&sa.sa_mask);
  sa.sa_handler = sighandler;
  if (sigaction(SIGINT, &sa, NULL) != 0)
    fatal("sigaction(SIGINT) failed\n");
  if (sigaction(SIGTERM, &sa, NULL) != 0)
    fatal("sigaction(SIGTERM) failed\n");
}

static void run()
{
  jclass stringClass = (*mainEnv)->FindClass(mainEnv, "java/lang/String");
  jobjectArray args = (*mainEnv)->NewObjectArray(mainEnv, mainArgsCount, stringClass, NULL);
  int i;
  for (i = 0; i < mainArgsCount; i++) {
    jstring jstr = (*mainEnv)->NewStringUTF(mainEnv, mainArgs[i]);
    (*mainEnv)->SetObjectArrayElement(mainEnv, args, i, jstr);
  }

  (*mainEnv)->CallStaticVoidMethod(mainEnv, mainClass, mainMethod, args);
  // Will never be reached if System.exit() is called
  if ((*mainEnv)->ExceptionOccurred(mainEnv)) {
    (*mainEnv)->ExceptionDescribe(mainEnv);
  }
  (*jvm)->DestroyJavaVM(jvm);
}

int main(int argc, char *argv[])
{
  parse_args(argc, argv);
  set_user();
  set_caps();
  load_lib();
  create_thread();
  create_jvm();
  install_sighandler();
  run();
  return 0;
}
