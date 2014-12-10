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
#include <dirent.h>

#ifdef CAPS_SUPPORT
#include <pwd.h>
#include <sys/prctl.h>
#include <sys/capability.h>
static char* userName = NULL;
static char* capsText = NULL;
#endif

static pthread_mutex_t threadLock = PTHREAD_MUTEX_INITIALIZER;

static int vmArgsCount;
static char** vmArgs;
static char* classPath = NULL;
static char* className;
static int mainArgsCount;
static char** mainArgs;

static JavaVM* jvm;
static JNIEnv* mainEnv;
static jclass mainClass;
static jmethodID mainMethod;
static jmethodID shutdownMethod;
static jmethodID reloadMethod;

static volatile int lastSignal = 0;

static void syntax()
{
#ifdef CAPS_SUPPORT
  printf("syntax: semeru [-u user] [-c caps] [-cp classpath] [jvm options...] <classname> [params...]\n");
#else
  printf("syntax: semeru [-cp classpath] [jvm options...] <classname> [params...]\n");
#endif
  exit(1);
}

struct strlist {
  char **items;
  size_t size;
  size_t capacity;
};

static void strlist_init(struct strlist *p, size_t capacity)
{
  p->items = calloc(capacity, sizeof(char*));
  p->size = 0;
  p->capacity = capacity;
}

static void strlist_destroy(struct strlist *p)
{
  size_t i;
  for (i = 0; i < p->size; i++) {
    free(p->items[i]);
  }
  free(p->items);
  p->items = NULL;
}

static void strlist_add(struct strlist *p, char *item)
{
  if (p->size >= p->capacity) {
    p->capacity *= 2;
    p->items = realloc(p->items, p->capacity * sizeof(char*));
  }
  p->items[p->size] = strdup(item);
  p->size++;
}

static void strlist_remove_last(struct strlist *p)
{
  if (p->size > 0) {
    p->size--;
    free(p->items[p->size]);
    p->items[p->size] = NULL;
  }
}

static char* strlist_concat(struct strlist *p)
{
  size_t i, size = 0;
  char *result, *pos;
  for (i = 0; i < p->size; i++) {
    size += strlen(p->items[i]);
  }
  result = calloc(size + 1, 1);
  pos = result;
  for (i = 0; i < p->size; i++) {
    size_t len = strlen(p->items[i]);
    memcpy(pos, p->items[i], len);
    pos += len;
  }
  return result;
}

static void split(char *str, char ch, struct strlist *dst)
{
  char *p = str;
  while ((p = strchr(str, ch)) != NULL) {
    *p = '\0';
    strlist_add(dst, str);
    *p = ch;
    str = p + 1;
  }
  strlist_add(dst, str);
}

static int is_jarfile(char *filename)
{
  size_t len = strlen(filename);
  char *ext = filename + len - 4;
  return ((len <= 4) || (strchr(filename, ':') != NULL) || strcasecmp(ext, ".jar")) ? 0 : 1;
}

static void expand_jars(char *path, struct strlist *dst)
{
  struct dirent *entry;
  DIR* dir;
  size_t path_len = strlen(path);

  if ((path_len < 2) || (path[path_len - 2] != '/') || (path[path_len - 1] != '*')) {
    strlist_add(dst, path);
    strlist_add(dst, ":");
    return;
  }

  path[path_len - 1] = '\0'; /* Cut off trailing star */
  dir = opendir(path);
  if (dir == NULL) {
    path[path_len - 1] = '*'; /* Restore original value */
    return;
  }

  while ((entry = readdir(dir)) != NULL) {
#ifdef _DIRENT_HAVE_D_TYPE
    if (entry->d_type != DT_REG)
      continue;
#endif
    if (is_jarfile(entry->d_name)) {
      strlist_add(dst, path);
      strlist_add(dst, entry->d_name);
      strlist_add(dst, ":");
    }
  }
  closedir(dir);
  path[path_len - 1] = '*'; /* Restore original value */
}

static void expand_class_path(char *cp, struct strlist *dst) {
  struct strlist wildcards;
  size_t i;
  strlist_init(&wildcards, 16);
  split(cp, ':', &wildcards);
  for (i = 0; i < wildcards.size; i++) {
    expand_jars(wildcards.items[i], dst);
  }
  strlist_destroy(&wildcards);
}

static char* create_class_path_option(char *cp) {
  struct strlist parts;
  char *result;
  strlist_init(&parts, 128);
  strlist_add(&parts, "-Djava.class.path=");
  expand_class_path(cp, &parts);
  if (parts.size > 1) {
    strlist_remove_last(&parts); /* last entry is a ':' */
  }
  result = strlist_concat(&parts);
  strlist_destroy(&parts);
  return result;
}

static void parse_args(int argc, char *argv[])
{
  int i = 1;
  int vmArgsIndex;
  int classNameIndex = 0;

  for (; i < argc; i++) {
    if (!strcmp("-cp", argv[i])) {
      if (++i >= argc)
        syntax();
      classPath = argv[i];
    }
#ifdef CAPS_SUPPORT
    else if (!strcmp("-u", argv[i])) {
      if (++i >= argc)
        syntax();
      userName = argv[i];
    }
    else if (!strcmp("-c", argv[i])) {
      if (++i >= argc)
        syntax();
      capsText = argv[i];
    }
#endif
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
  jmethodID method;
  while (1) {
    pthread_mutex_lock(&threadLock);
    method = (lastSignal == SIGHUP) ? reloadMethod : shutdownMethod;
    if ((*jvm)->AttachCurrentThreadAsDaemon(jvm, (void**)&env, NULL) != JNI_OK) {
      errx(50, "AttachCurrentThreadAsDaemon() failed");
    }
    (*env)->CallStaticVoidMethod(env, mainClass, method);
    if ((*env)->ExceptionOccurred(env) != NULL) {
      (*env)->ExceptionDescribe(env);
    }
    (*jvm)->DetachCurrentThread(jvm);
  }
  return NULL;
}

static void create_thread()
{
  pthread_t hthread;
  pthread_mutex_lock(&threadLock);
  if (pthread_create(&hthread, NULL, thread_func, jvm) != 0)
    errx(50, "pthread_create() failed");
  if (pthread_detach(hthread) != 0)
    errx(50, "pthread_detach() failed");
}

static void create_jvm()
{
  int i, option_count;
  JavaVMInitArgs vm_args;

  option_count = vmArgsCount + ((classPath != NULL) ? 1 : 0);
  vm_args.version = JNI_VERSION_1_4;
  vm_args.ignoreUnrecognized = JNI_FALSE;
  vm_args.nOptions = option_count;
  vm_args.options = calloc(option_count, sizeof(JavaVMOption));
  for (i = 0; i < vmArgsCount; i++) {
    vm_args.options[i].optionString = vmArgs[i];
    vm_args.options[i].extraInfo = NULL;
  }
  if (classPath != NULL) {
    vm_args.options[i].optionString = create_class_path_option(classPath);
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
  reloadMethod = (*mainEnv)->GetStaticMethodID(mainEnv, mainClass, "reload", "()V");
  (*mainEnv)->ExceptionClear(mainEnv); /* Ignore possible exception caused by missing reload() */
}

static void sighandler(int num)
{
  lastSignal = num;
  pthread_mutex_unlock(&threadLock);
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
  if ((reloadMethod != NULL) && (sigaction(SIGHUP, &sa, NULL) != 0))
    err(50, "sigaction(SIGHUP) failed");
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
