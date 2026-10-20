/* comun.c */
#include "comun.h"

#include <errno.h>

int64_t ahora_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

void dormir_us(long us) {
    if (us <= 0) return;
    struct timespec t = {us / 1000000, (us % 1000000) * 1000L};
    while (nanosleep(&t, &t) == -1 && errno == EINTR) {
        /* si una señal interrumpe la pausa, se completa el resto */
    }
}

void limite_desde_ahora(struct timespec *ts, int ms) {
    clock_gettime(CLOCK_MONOTONIC, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

/* ------------------------------------------------------------- hilo_t */
void hilo_init(hilo_t *h, int id, unsigned semilla, int max_pausa_us, bool registrar) {
    h->id = id;
    h->semilla = semilla;
    h->max_pausa_us = max_pausa_us;
    h->registrar = registrar;
    h->eventos = NULL;
    h->n_eventos = 0;
    h->cap_eventos = 0;
}

void hilo_destruir(hilo_t *h) {
    free(h->eventos);
    h->eventos = NULL;
    h->n_eventos = h->cap_eventos = 0;
}

int hilo_aleatorio(hilo_t *h, int lo, int hi) {
    if (hi <= lo) return lo;
    return lo + rand_r(&h->semilla) % (hi - lo + 1);
}

void hilo_pausa(hilo_t *h) {
    /* Pausa aleatoria pequeña. NO sirve para ordenar nada: solo hace que el
     * planificador cambie de thread en momentos distintos. */
    if (h == NULL || h->max_pausa_us <= 0) return;
    dormir_us(hilo_aleatorio(h, 0, h->max_pausa_us));
}

void hilo_log(hilo_t *h, tipo_evento_t tipo, int recurso, int libres) {
    if (h == NULL || !h->registrar) return;
    if (h->n_eventos == h->cap_eventos) {
        int nueva = h->cap_eventos ? h->cap_eventos * 2 : 256;
        evento_t *p = realloc(h->eventos, (size_t)nueva * sizeof *p);
        if (p == NULL) return;              /* sin memoria: se pierde el evento */
        h->eventos = p;
        h->cap_eventos = nueva;
    }
    evento_t *e = &h->eventos[h->n_eventos++];
    e->t_ns = ahora_ns();
    e->hilo = h->id;
    e->tipo = tipo;
    e->recurso = recurso;
    e->libres = libres;
}

const char *nombre_evento(tipo_evento_t t) {
    switch (t) {
    case EV_SOLICITA:      return "solicita";
    case EV_ESPERA:        return "espera";
    case EV_VERIFICA:      return "ve_libre";
    case EV_OBTIENE:       return "obtiene";
    case EV_TIMEOUT:       return "timeout";
    case EV_LIBERA:        return "libera";
    case EV_CANCELADA:     return "cancelada";
    case EV_CERRADO:       return "rechazada";
    case EV_CONSULTA:      return "consulta";
    case EV_PIDE_CANCELAR: return "pide_cancelar";
    case EV_CIERRE:        return "cierra";
    }
    return "?";
}

static int comparar_eventos(const void *a, const void *b) {
    const evento_t *x = a, *y = b;
    if (x->t_ns != y->t_ns) return x->t_ns < y->t_ns ? -1 : 1;
    return x->hilo - y->hilo;
}

int unir_trazas(hilo_t *hilos, int n, evento_t **salida) {
    int total = 0;
    for (int i = 0; i < n; ++i) total += hilos[i].n_eventos;
    *salida = NULL;
    if (total == 0) return 0;
    evento_t *v = malloc((size_t)total * sizeof *v);
    if (v == NULL) return 0;
    int k = 0;
    for (int i = 0; i < n; ++i) {
        memcpy(v + k, hilos[i].eventos, (size_t)hilos[i].n_eventos * sizeof *v);
        k += hilos[i].n_eventos;
    }
    qsort(v, (size_t)total, sizeof *v, comparar_eventos);
    *salida = v;
    return total;
}

void imprimir_evento(FILE *f, const evento_t *e, int64_t t0) {
    char quien[16], rec[16];
    if (e->hilo == 0) snprintf(quien, sizeof quien, "main");
    else snprintf(quien, sizeof quien, "T%d", e->hilo);
    if (e->recurso >= 0) snprintf(rec, sizeof rec, "R%d", e->recurso);
    else snprintf(rec, sizeof rec, "-");
    fprintf(f, "  t=+%9.3f ms  %-4s  %-13s  %-4s  libres=%d\n",
            (double)(e->t_ns - t0) / 1e6, quien, nombre_evento(e->tipo), rec, e->libres);
}
