# Servicio concurrente de reservas (C/POSIX)

Sistemas Operativos · UNSA · 2026-B · Docente: MSc. Norman Patrick Harvey Arce
Alumna: Bedregal Coaguila, Karla Miluska

Práctica domiciliaria de la Semana 7 (sincronización de procesos). Un servicio
administra **M recursos** y varias solicitudes (threads) los **consultan,
reservan, cancelan y esperan** disponibilidad.

- **Versión correcta** (`src/servicio.c`): un `pthread_mutex_t` protege todo el
  estado y una `pthread_cond_t` permite esperar con timeout
  (`pthread_cond_timedwait`), cancelar solicitudes en espera y cerrar el
  servicio sin dejar threads colgados.
- **Versión defectuosa** (`src/servicio_defectuoso.c`): los mismos pasos sin
  sincronización, para reproducir la carrera (doble reserva y contador
  inconsistente).

## Invariantes

| | |
|---|---|
| I1 | `libres` es igual a la cantidad de recursos con `dueno == -1` |
| I2 | `0 <= libres <= M` |
| I3 | cada recurso tiene a lo sumo un dueño válido (no hay doble reserva) |
| I4 | `reservas - liberaciones == M - libres` |
| I5 | con el servicio cerrado no se concede ninguna reserva |

## Compilar y ejecutar (Linux)

```bash
sudo apt install build-essential valgrind   # una sola vez
make                       # compila todo en bin/
./bin/demo                 # traza con 8 workers: bloqueo, timeout, cancelación y cierre
./bin/carrera              # carrera: defectuosa vs correcta, 100 repeticiones
./bin/metricas             # métricas de espera y rendimiento (M = 1, 3, 10; 8/16/32 workers)
./bin/test_reservas        # pruebas automatizadas (deben pasar)
make test-defectuoso       # las mismas pruebas contra la versión defectuosa (deben fallar)
make tsan                  # ThreadSanitizer
make memcheck              # valgrind: sin fugas de memoria
```

Los resultados (CSV y trazas) se guardan en `resultados/`.

## Estructura

```
src/comun.h, comun.c                  tiempo, pausas, contexto de cada thread y traza
src/servicio.h                        modelo de estado, invariantes e interfaz
src/servicio.c                        versión correcta (mutex + variable de condición)
src/servicio_defectuoso.c             versión sin sincronización
src/carga.h, carga.c                  carga con N workers + observador de errores
src/demo.c                            traza mínima con 8 workers
src/carrera.c                         reproducción de la carrera
src/metricas.c                        métricas de espera y rendimiento
tests/test_reservas.c                 pruebas automatizadas
Makefile
```
