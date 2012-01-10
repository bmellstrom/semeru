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
#include <signal.h>
#include <pthread.h>
#include <jni.h>
#include <err.h>

#ifdef CAPS_SUPPORT
#include <pwd.h>
#include <sys/prctl.h>
#include <sys/capability.h>
static char* userName = NULL;
static char* capsText = NULL;
#endif

static pthread_mutex_t exit_lock = PTHREAD_MUTEX_INITIALIZER;

static int vmArgsCount;
static char** vmArgs;
static char* className;
static int mainArgsCount;
static char** mainArgs;

static JavaVM* jvm;
static JNIEnv* mainEnv;
static jclass mainClass;
static jmethodID mainMethod;
static jmethodID shutdownMethod;

static void syntax()
{
#ifdef CAPS_SUPPORT
  printf("syntax: jexec [-u user] [-c caps] [jvm options...] <classname> [params...]\n");
#else
  printf("syntax: jexec [jvm options...] <classname> [params...]\n");
#endif
  exit(1);
}

static void parse_args(int argc, char *argv[])
{
  int i = 1;
  int vmArgsIndex;
  int classNameIndex = 0;

#ifdef CAPS_SUPPORT
  for (; i < argc; i++) {
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
#endif
  vmArgsIndex = i;
  for (; i < argc; i++) {
    if (argv[i][0] != '-') {
      classNameIndex = i;
      break;
    }
  }
  if (classNameIndex == 0) {
    syntax();
  }
  vmArgsCount = classNameIndex - vmArgsIndex;
  vmArgs = &argv[vmArgsIndex];
  className = argv[classNameIndex];
  for (i = 0; className[i] != '\0'; i++) {
    if (className[i] == '.')
      className[i] = '/';
  }
  mainArgsCount = argc - (classNameIndex + 1);
  mainArgs = &argv[classNameIndex + 1];
}

#ifdef CAPS_SUPPORT
static void set_keepcaps(int enabled)
{
  if ((capsText != NULL) && (prctl(PR_SET_KEEPCAPS, enabled) != 0))
    err(50, "prctl(PR_SET_KEEPCAPS, %d) failed", enabled);
}

static void set_user()
{
  if (userName != NULL) {
    struct passwd *pwEnt = getpwnam(userName);
    if (pwEnt == NULL)
      errx(50, "User not found: %s", userName);
    set_keepcaps(1);
    if (setregid(pwEnt->pw_gid, pwEnt->pw_gid) != 0)
      err(50, "Failed to change group");
    if (setreuid(pwEnt->pw_uid, pwEnt->pw_uid) != 0)
      err(50, "Failed to change user");
    set_keepcaps(0);
  }
}

static void set_caps()
{
  if (capsText != NULL) {
    cap_t caps = cap_from_text(capsText);
    if (caps == NULL)
      errx(50, "Failed to parse capabilities: %s", capsText);
    if (cap_set_proc(caps) < 0)
      err(50, "Failed to set capabilities");
    cap_free(caps);
  }
}
#endif

static void* thread_func(void* arg)
{
  JNIEnv *env;
  pthread_mutex_lock(&exit_lock);
  if ((*jvm)->AttachCurrentThreadAsDaemon(jvm, (void**)&env, NULL) != JNI_OK) {
    errx(50, "AttachCurrentThreadAsDaemon() failed");
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
    errx(50, "pthread_create() failed");
  if (pthread_detach(hthread) != 0)
    errx(50, "pthread_detach() failed");
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

  if (JNI_CreateJavaVM(&jvm, (void**)&mainEnv, &vm_args) != 0)
    errx(50, "JNI_CreateJavaVM() failed");

  mainClass = (*mainEnv)->FindClass(mainEnv, className);
  if (mainClass == NULL)
    errx(50, "Main class not found: %s", className);
  mainMethod = (*mainEnv)->GetStaticMethodID(mainEnv, mainClass, "main", "([Ljava/lang/String;)V");
  if (mainMethod == NULL)
    errx(50, "Main method not found in: %s", className);
  shutdownMethod = (*mainEnv)->GetStaticMethodID(mainEnv, mainClass, "shutdown", "()V");
  if (shutdownMethod == NULL)
    errx(50, "Shutdown method not found in: %s", className);
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
    err(50, "sigaction(SIGINT) failed");
  if (sigaction(SIGTERM, &sa, NULL) != 0)
    err(50, "sigaction(SIGTERM) failed");
}

static void run()
{
  int i;
  jclass stringClass;
  jobjectArray args;

  stringClass = (*mainEnv)->FindClass(mainEnv, "java/lang/String");
  args = (*mainEnv)->NewObjectArray(mainEnv, mainArgsCount, stringClass, NULL);
  for (i = 0; i < mainArgsCount; i++) {
    jstring jstr = (*mainEnv)->NewStringUTF(mainEnv, mainArgs[i]);
    (*mainEnv)->SetObjectArrayElement(mainEnv, args, i, jstr);
  }

  (*mainEnv)->CallStaticVoidMethod(mainEnv, mainClass, mainMethod, args);
  /* Will never be reached if System.exit() is called */
  if ((*mainEnv)->ExceptionOccurred(mainEnv)) {
    (*mainEnv)->ExceptionDescribe(mainEnv);
  }
  (*jvm)->DestroyJavaVM(jvm);
}

int main(int argc, char *argv[])
{
  parse_args(argc, argv);
#ifdef CAPS_SUPPORT
  set_user();
  set_caps();
#endif
  create_thread();
  create_jvm();
  install_sighandler();
  run();
  return 0;
}
