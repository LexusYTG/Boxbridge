# Boxbridge

Interceptor transparente de binarios x86/x86_64 hacia `box64` via `LD_PRELOAD`.

## Que hace

Cuando un proceso carga esta libreria, cualquier llamada a `execve`,
`execv`, `execvp`, `execvpe`, `posix_spawn`, `posix_spawnp` o `fexecve`
que apunte a un ELF x86 o x86_64 se reescribe automaticamente a:

    box64 <binario> <args...>

No requiere root, no requiere `binfmt_misc`, no toca el kernel. Es el
mismo patron que usa Termux-exec para reescribir `execve` dentro del
sandbox de Android.

## Que cubre

- `execve` — intercepta y redirige si el path resuelve a x86.
- `execv`, `execvp`, `execvpe` — resuelven PATH y delegan en el wrapper.
- `posix_spawn`, `posix_spawnp` — reescriben el path antes de spawnear.
- `fexecve` — resuelve el path via `/proc/self/fd/N` y reescribe.

## Que NO cubre

- Syscalls directas (`syscall(SYS_execve, ...)`). No es comun en apps
  userland, pero algunos runtimes lo hacen. Si hace falta, se agrega
  interceptando `syscall` por simbolo y filtrando el numero.
- `/proc/self/exe` reescrito por la app misma.
- Binarios que cargan un ELF x86 a mano via `dlopen` en lugar de
  `execve`. Eso no es exec, es dlopen, y no aplica en este flujo.

## Uso

Compilar:

    make

Precargar al proceso que lanza subprocesos:

    export LD_PRELOAD=/ruta/libboxbridge.so
    steam

Todo `exec*` que haga Steam (o lo que sea) va a pasar por el filtro.

## Variables

- `BOXBRIDGE_OFF=1`      desactiva la interceptacion sin descargar la lib.
- `BOXBRIDGE_BOX64=PATH` ruta del binario box64. Default `/usr/bin/box64`.
- `BOXBRIDGE_VERBOSE=1`  log a stderr de cada reescritura x86 detectada.
- `BOXBRIDGE_TRACE=1`    log tambien de cada exec no interceptado.

## Ejemplo tipico (Steam)

    apt install box64
    cp libboxbridge.so /usr/local/lib/
    LD_PRELOAD=/usr/local/lib/libboxbridge.so BOXBRIDGE_VERBOSE=1 \
        /root/steam-extract/usr/bin/steam

Cada subproceso que Steam lance (webhelper, launcher, runtime) va a
pasar por el wrapper y los binarios x86_64 que encuentre se van a
ejecutar via box64. Los binarios arm64 nativos van directo, sin tocar.

## Licencia

MIT.
