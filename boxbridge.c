/*
 * boxbridge.c - interceptor transparente de binarios x86/x86_64 -> box64.
 *
 * Se carga via LD_PRELOAD en el proceso que lanza otros procesos. Cada
 * execve/execvp/execv/execvpe/posix_spawn/posix_spawnp que se haga desde
 * ese proceso es interceptado: si el ejecutable es un ELF x86 o x86_64,
 * se reemplaza la invocacion por `box64 <binario> <args...>`.
 *
 * No requiere root, no requiere binfmt_misc, no toca el kernel. Es el
 * mismo patron que usa Termux-exec para reescribir execve.
 *
 * Variables:
 *   BOXBRIDGE_OFF=1        desactiva la interceptacion
 *   BOXBRIDGE_BOX64=PATH   ruta del binario box64 (default /usr/bin/box64)
 *   BOXBRIDGE_VERBOSE=1    log a stderr de cada reescritura
 *   BOXBRIDGE_TRACE=1      log tambien de cada exec no interceptado
 *
 * Compilar:
 *   gcc -O2 -fPIC -shared -o libboxbridge.so boxbridge.c -ldl -pthread
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <limits.h>

#define MAX_REDIRECT_ARGS 4096

/* ---------- configuracion (una sola lectura, cacheada) ---------- */
static int  cfg_off     = -1;
static int  cfg_verbose = -1;
static int  cfg_trace   = -1;
static char cfg_box64[PATH_MAX] = {0};

static void load_cfg(void)
{
    if (cfg_off >= 0) return;
    const char* e;
    e = getenv("BOXBRIDGE_OFF");
    cfg_off = (e && *e == '1');
    e = getenv("BOXBRIDGE_VERBOSE");
    cfg_verbose = (e && *e == '1');
    e = getenv("BOXBRIDGE_TRACE");
    cfg_trace = (e && *e == '1');
    e = getenv("BOXBRIDGE_BOX64");
    snprintf(cfg_box64, sizeof cfg_box64, "%s", (e && *e) ? e : "/usr/bin/box64");
}

#define LOGV(...) do { if (cfg_verbose) { \
    fprintf(stderr, "[boxbridge] " __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define LOGT(...) do { if (cfg_trace) { \
    fprintf(stderr, "[boxbridge] " __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* ---------- deteccion de ELF ---------- */
/* Devuelve EM_X86_64, EM_386, o 0 si no aplica. */
static unsigned short elf_machine(const char *path)
{
    unsigned char hdr[64];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t n = read(fd, hdr, sizeof hdr);
    close(fd);
    if (n < (ssize_t)(EI_NIDENT + 2)) return 0;
    if (memcmp(hdr, ELFMAG, SELFMAG) != 0) return 0;
    /* little-endian, cualquier clase */
    if (hdr[EI_DATA] != ELFDATA2LSB) return 0;
    /* leer e_machine segun clase */
    if (hdr[EI_CLASS] == ELFCLASS64) {
        if (n < 20) return 0;
        unsigned short m;
        memcpy(&m, hdr + 18, 2);
        return m;
    } else if (hdr[EI_CLASS] == ELFCLASS32) {
        if (n < 20) return 0;
        unsigned short m;
        memcpy(&m, hdr + 18, 2);
        return m;
    }
    return 0;
}

static int needs_box64(const char *path)
{
    unsigned short m = elf_machine(path);
    return m == EM_X86_64 || m == EM_386;
}

/* ---------- resolucion en PATH ---------- */
/* Llena out con el path absoluto del ejecutable, o deja out vacio si no lo encuentra. */
static int resolve_path(const char *file, char *out, size_t outn)
{
    if (!file || !*file) return 0;

    /* path absoluto o relativo con '/' -> usar tal cual */
    if (strchr(file, '/')) {
        struct stat st;
        if (stat(file, &st) == 0 && !S_ISDIR(st.st_mode)) {
            snprintf(out, outn, "%s", file);
            return 1;
        }
        return 0;
    }

    const char *p = getenv("PATH");
    if (!p) p = "/usr/local/bin:/usr/bin:/bin";
    size_t flen = strlen(file);

    while (*p) {
        const char *q = strchr(p, ':');
        size_t n = q ? (size_t)(q - p) : strlen(p);
        if (n == 0) n = 1; /* entrada vacia = "." */
        if (n + 1 + flen + 1 <= outn) {
            memcpy(out, p, n);
            out[n] = '/';
            memcpy(out + n + 1, file, flen);
            out[n + 1 + flen] = 0;
            if (access(out, X_OK) == 0) return 1;
        }
        p = q ? q + 1 : p + n;
    }
    out[0] = 0;
    return 0;
}

/* ---------- real execve / helpers ---------- */
typedef int (*execve_t)(const char *, char *const[], char *const[]);
typedef int (*execvp_t)(const char *, char *const[]);
typedef int (*execv_t)(const char *, char *const[]);
typedef int (*execvpe_t)(const char *, char *const[], char *const[]);

static execve_t  real_execve  = NULL;
static execvp_t  real_execvp  = NULL;
static execv_t   real_execv   = NULL;
static execvpe_t real_execvpe = NULL;

static void init_real(void)
{
    /* no hace falta lock: la asignacion es idempotente y atómica */
    if (!real_execve)  real_execve  = (execve_t)dlsym(RTLD_NEXT, "execve");
    if (!real_execvp)  real_execvp  = (execvp_t)dlsym(RTLD_NEXT, "execvp");
    if (!real_execv)   real_execv   = (execv_t)dlsym(RTLD_NEXT, "execv");
    if (!real_execvpe) real_execvpe = (execvpe_t)dlsym(RTLD_NEXT, "execvpe");
}

/* Construye [box64, path, argv[1..], NULL] en heap. */
static char **build_box64_argv(const char *path, char *const argv[])
{
    int argc = 0;
    while (argv && argv[argc]) argc++;
    if (argc > MAX_REDIRECT_ARGS - 3) return NULL;

    char **newv = malloc((size_t)(argc + 3) * sizeof(char *));
    if (!newv) return NULL;
    newv[0] = (char *)cfg_box64;
    newv[1] = (char *)path;
    for (int i = 1; i < argc; i++) newv[i + 1] = argv[i];
    newv[argc + 1] = NULL;
    return newv;
}

static int do_redirect_execve(const char *path, char *const argv[], char *const envp[])
{
    char resolved[PATH_MAX];
    if (!resolve_path(path, resolved, sizeof resolved)) return 0;
    if (!needs_box64(resolved)) {
        LOGT("no x86 (%s): passthrough", resolved);
        return 0;
    }
    char **newargv = build_box64_argv(resolved, argv);
    if (!newargv) {
        LOGV("OOM construyendo argv para %s", resolved);
        return 0;
    }
    LOGV("x86 detectado: %s -> %s", resolved, cfg_box64);
    (void)real_execve(cfg_box64, newargv, envp);
    /* solo llegamos aca si fallo */
    int saved = errno;
    free(newargv);
    errno = saved;
    return 1;
}

/* ---------- overrides publicos ---------- */

int execve(const char *path, char *const argv[], char *const envp[])
{
    load_cfg();
    init_real();
    if (!cfg_off && do_redirect_execve(path, argv, envp)) return -1;
    return real_execve(path, argv, envp);
}

int execv(const char *path, char *const argv[])
{
    extern char **environ;
    return execve(path, argv, environ);
}

int execvp(const char *file, char *const argv[])
{
    load_cfg();
    init_real();
    if (!cfg_off) {
        char resolved[PATH_MAX];
        if (resolve_path(file, resolved, sizeof resolved) && needs_box64(resolved)) {
            char **newargv = build_box64_argv(resolved, argv);
            if (newargv) {
                extern char **environ;
                LOGV("x86 detectado (execvp): %s -> %s", resolved, cfg_box64);
                (void)real_execve(cfg_box64, newargv, environ);
                int saved = errno;
                free(newargv);
                errno = saved;
                return -1;
            }
        }
    }
    return real_execvp(file, argv);
}

int execvpe(const char *file, char *const argv[], char *const envp[])
{
    load_cfg();
    init_real();
    if (!cfg_off) {
        char resolved[PATH_MAX];
        if (resolve_path(file, resolved, sizeof resolved) && needs_box64(resolved)) {
            char **newargv = build_box64_argv(resolved, argv);
            if (newargv) {
                LOGV("x86 detectado (execvpe): %s -> %s", resolved, cfg_box64);
                (void)real_execve(cfg_box64, newargv, envp);
                int saved = errno;
                free(newargv);
                errno = saved;
                return -1;
            }
        }
    }
    if (real_execvpe) return real_execvpe(file, argv, envp);
    /* fallback: execve con PATH propio */
    char resolved[PATH_MAX];
    if (resolve_path(file, resolved, sizeof resolved))
        return execve(resolved, argv, envp);
    errno = ENOENT;
    return -1;
}

/* ---------- posix_spawn y posix_spawnp ----------
 * El glibc moderno resuelve posix_spawn con clone+execve dentro del
 * propio proceso. Si interceptamos execve, el hijo que hace el exec
 * pasa por nuestro wrapper y ahi se redirige. Sin embargo, glibc
 * puede usar una syscall directa (clone3+execve) en algunos casos.
 * Para cubrir esos casos, envolvemos posix_spawn y reescribimos el
 * path antes de delegar.
 */
typedef int (*posix_spawn_t)(pid_t *, const char *,
                             const posix_spawn_file_actions_t *,
                             const posix_spawnattr_t *,
                             char *const[], char *const[]);
typedef int (*posix_spawnp_t)(pid_t *, const char *,
                              const posix_spawn_file_actions_t *,
                              const posix_spawnattr_t *,
                              char *const[], char *const[]);

static posix_spawn_t  real_spawn  = NULL;
static posix_spawnp_t real_spawnp = NULL;

int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *fa,
                const posix_spawnattr_t *attr,
                char *const argv[], char *const envp[])
{
    load_cfg();
    init_real();
    if (!real_spawn) real_spawn = (posix_spawn_t)dlsym(RTLD_NEXT, "posix_spawn");

    if (!cfg_off) {
        char resolved[PATH_MAX];
        if (resolve_path(path, resolved, sizeof resolved) && needs_box64(resolved)) {
            char **newargv = build_box64_argv(resolved, argv);
            if (newargv) {
                LOGV("x86 detectado (posix_spawn): %s -> %s", resolved, cfg_box64);
                int rc = real_spawn(pid, cfg_box64, fa, attr, newargv, envp);
                free(newargv);
                return rc;
            }
        }
    }
    return real_spawn(pid, path, fa, attr, argv, envp);
}

int posix_spawnp(pid_t *pid, const char *file,
                 const posix_spawn_file_actions_t *fa,
                 const posix_spawnattr_t *attr,
                 char *const argv[], char *const envp[])
{
    load_cfg();
    init_real();
    if (!real_spawnp) real_spawnp = (posix_spawnp_t)dlsym(RTLD_NEXT, "posix_spawnp");

    if (!cfg_off) {
        char resolved[PATH_MAX];
        if (resolve_path(file, resolved, sizeof resolved) && needs_box64(resolved)) {
            char **newargv = build_box64_argv(resolved, argv);
            if (newargv) {
                LOGV("x86 detectado (posix_spawnp): %s -> %s", resolved, cfg_box64);
                int rc = real_spawnp(pid, cfg_box64, fa, attr, newargv, envp);
                free(newargv);
                return rc;
            }
        }
    }
    return real_spawnp(pid, file, fa, attr, argv, envp);
}

/* ---------- fexecve ----------
 * fexecve recibe un fd, no un path. Se puede resolver via /proc/self/fd/N.
 * Lo interceptamos igual: resolvemos el path por proc y si es x86, usamos
 * execve con ese path. Si el fd esta abierto en modo exec-only (O_PATH),
 * fexecve nativo funciona sin reescritura. */
typedef int (*fexecve_t)(int, char *const[], char *const[]);
static fexecve_t real_fexecve = NULL;

int fexecve(int fd, char *const argv[], char *const envp[])
{
    load_cfg();
    init_real();
    if (!real_fexecve) real_fexecve = (fexecve_t)dlsym(RTLD_NEXT, "fexecve");

    if (!cfg_off) {
        char procpath[64];
        snprintf(procpath, sizeof procpath, "/proc/self/fd/%d", fd);
        char real[PATH_MAX];
        ssize_t n = readlink(procpath, real, sizeof real - 1);
        if (n > 0) {
            real[n] = 0;
            if (needs_box64(real)) {
                char **newargv = build_box64_argv(real, argv);
                if (newargv) {
                    LOGV("x86 detectado (fexecve): %s -> %s", real, cfg_box64);
                    (void)real_execve(cfg_box64, newargv, envp);
                    int saved = errno;
                    free(newargv);
                    errno = saved;
                    return -1;
                }
            }
        }
    }
    return real_fexecve(fd, argv, envp);
}
