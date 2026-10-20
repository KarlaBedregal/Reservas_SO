/* servicio_defectuoso.c
 * Versión DEFECTUOSA del servicio de reservas, hecha a propósito para
 * reproducir la condición de carrera (entregable 5).
 *
 * Hace los mismos pasos que la versión correcta y con las mismas pausas,
 * pero SIN mutex ni variable de condición:
 *   - "verificar y luego actuar": ve que dueno[i] == -1 y recién después
 *     escribe su id; otro thread puede ver el mismo recurso libre en medio.
 *   - "leer, modificar, escribir" sobre libres: se pierden actualizaciones.
 *   - la espera es activa (sched_yield en un bucle) porque no hay condición.
 *
 * Se usan punteros volatile solo para que el compilador no elimine las
 * lecturas repetidas del bucle de espera. volatile NO sincroniza nada:
 * los accesos siguen siendo una carrera de datos.
 */
#include "servicio.h"

#include <sched.h>

resultado_t defectuoso_reservar(servicio_t *s, hilo_t *h, int cliente, int timeout_ms, int *recurso) {
    if (recurso) *recurso = -1;
    if (cliente < 0 || cliente >= s->max_clientes || timeout_ms < 0) return RES_INVALIDO;

    volatile int *dueno = s->dueno;
    volatile int *libres = &s->libres;
    volatile long *reservas = &s->reservas;
    int64_t limite = ahora_ns() + (int64_t)timeout_ms * 1000000LL;

    hilo_log(h, EV_SOLICITA, -1, *libres);
    bool espero = false;
    for (;;) {
        if (*libres > 0) {                               /* verificar */
            for (int i = 0; i < s->M; ++i) {
                if (dueno[i] == -1) {                    /* verificar */
                    hilo_log(h, EV_VERIFICA, i, *libres);
                    hilo_pausa(h);                       /* ventana: otro thread se mete aquí */
                    dueno[i] = cliente;                  /* actuar */
                    int t = *libres;                     /* leer */
                    hilo_pausa(h);
                    *libres = t - 1;                     /* escribir */
                    *reservas = *reservas + 1;
                    if (s->M - *libres > s->ocupacion_max) s->ocupacion_max = s->M - *libres;
                    hilo_log(h, EV_OBTIENE, i, *libres);
                    if (recurso) *recurso = i;
                    return RES_OK;
                }
            }
        }
        if (ahora_ns() >= limite) {
            s->timeouts++;
            hilo_log(h, EV_TIMEOUT, -1, *libres);
            return RES_TIMEOUT;
        }
        if (!espero) {
            espero = true;
            s->esperas++;
            hilo_log(h, EV_ESPERA, -1, *libres);
        }
        sched_yield();                                   /* espera activa: gasta CPU */
    }
}

resultado_t defectuoso_cancelar(servicio_t *s, hilo_t *h, int cliente, int recurso) {
    if (recurso < 0 || recurso >= s->M) return RES_INVALIDO;
    volatile int *dueno = s->dueno;
    volatile int *libres = &s->libres;
    volatile long *liberaciones = &s->liberaciones;

    if (dueno[recurso] != cliente) return RES_NO_DUENO;  /* verificar */
    hilo_pausa(h);
    dueno[recurso] = -1;                                 /* actuar */
    int t = *libres;                                     /* leer */
    hilo_pausa(h);
    *libres = t + 1;                                     /* escribir */
    *liberaciones = *liberaciones + 1;
    hilo_log(h, EV_LIBERA, recurso, *libres);
    return RES_OK;
}

int defectuoso_consultar(servicio_t *s, hilo_t *h) {
    int v = *(volatile int *)&s->libres;
    hilo_log(h, EV_CONSULTA, -1, v);
    return v;
}

const ops_t OPS_DEFECTUOSO = {
    "defectuoso (sin sincronizacion)",
    defectuoso_reservar, defectuoso_cancelar, defectuoso_consultar, NULL,
};
