/* carga.c */
#include "carga.h"

#include <stdatomic.h>

config_carga_t config_por_defecto(void) {
    config_carga_t c = {
        .M = 3, .workers = 8, .ops = 50, .timeout_ms = 50, .uso_us = 300,
        .pausa_us = 0, .cancelador = false, .registrar = false, .semilla = 2026,
    };
    return c;
}

/* Observador externo: NO es parte de la solución, solo detecta errores.
 * Usa atómicos para que la propia medición no tenga carrera. */
typedef struct {
    atomic_int *ocupantes;      /* cuántos threads creen tener cada recurso */
    atomic_long dobles;
    atomic_long canc_fallidas;
    atomic_int en_uso;          /* recursos en uso según los workers */
    atomic_int en_uso_max;
    atomic_bool terminar;       /* para el thread cancelador */
} observador_t;

typedef struct {
    const ops_t *ops;
    servicio_t *s;
    const config_carga_t *cfg;
    observador_t *obs;
    hilo_t h;
    int cliente;
    /* resultados locales: cada worker escribe solo los suyos */
    long consultas, ok, timeouts, canceladas, cerradas;
    double *esperas_ms;
    int n_esperas;
} worker_t;

static void registrar_maximo(atomic_int *max, int v) {
    int actual = atomic_load(max);
    while (v > actual && !atomic_compare_exchange_weak(max, &actual, v)) {
    }
}

static void *trabajador(void *arg) {
    worker_t *w = arg;
    const config_carga_t *cfg = w->cfg;
    for (int i = 0; i < cfg->ops; ++i) {
        int p = hilo_aleatorio(&w->h, 0, 99);
        if (p < 20) {                                    /* 20 %: consultar */
            w->ops->consultar(w->s, &w->h);
            w->consultas++;
            continue;
        }
        /* 80 %: reservar -> usar -> cancelar */
        int r = -1;
        int64_t t0 = ahora_ns();
        resultado_t res = w->ops->reservar(w->s, &w->h, w->cliente, cfg->timeout_ms, &r);
        w->esperas_ms[w->n_esperas++] = (double)(ahora_ns() - t0) / 1e6;

        if (res == RES_OK) {
            w->ok++;
            int v = atomic_fetch_add(&w->obs->ocupantes[r], 1) + 1;
            if (v > 1) atomic_fetch_add(&w->obs->dobles, 1);
            registrar_maximo(&w->obs->en_uso_max, atomic_fetch_add(&w->obs->en_uso, 1) + 1);

            dormir_us(hilo_aleatorio(&w->h, 0, cfg->uso_us));   /* usa el recurso */

            atomic_fetch_sub(&w->obs->en_uso, 1);
            atomic_fetch_sub(&w->obs->ocupantes[r], 1);
            if (w->ops->cancelar(w->s, &w->h, w->cliente, r) != RES_OK)
                atomic_fetch_add(&w->obs->canc_fallidas, 1);
        } else if (res == RES_TIMEOUT) {
            w->timeouts++;
        } else if (res == RES_CANCELADA) {
            w->canceladas++;
        } else if (res == RES_CERRADO) {
            w->cerradas++;
        }
    }
    return NULL;
}

typedef struct {
    const ops_t *ops;
    servicio_t *s;
    observador_t *obs;
    int clientes;
    unsigned semilla;
} cancelador_t;

static void *cancelador(void *arg) {
    cancelador_t *c = arg;
    while (!atomic_load(&c->obs->terminar)) {
        dormir_us(500);
        int cliente = (int)(rand_r(&c->semilla) % (unsigned)c->clientes);
        c->ops->cancelar_solicitud(c->s, cliente);
    }
    return NULL;
}

static int comparar_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

resultado_carga_t ejecutar_carga(const ops_t *ops, const config_carga_t *cfg) {
    resultado_carga_t res;
    memset(&res, 0, sizeof res);

    servicio_t s;
    if (servicio_crear(&s, cfg->M, cfg->workers) != 0) {
        snprintf(res.detalle, sizeof res.detalle, "no se pudo crear el servicio");
        return res;
    }

    observador_t obs;
    obs.ocupantes = calloc((size_t)cfg->M, sizeof *obs.ocupantes);
    for (int i = 0; i < cfg->M; ++i) atomic_init(&obs.ocupantes[i], 0);
    atomic_init(&obs.dobles, 0);
    atomic_init(&obs.canc_fallidas, 0);
    atomic_init(&obs.en_uso, 0);
    atomic_init(&obs.en_uso_max, 0);
    atomic_init(&obs.terminar, false);

    worker_t *w = calloc((size_t)cfg->workers, sizeof *w);
    pthread_t *tid = malloc((size_t)cfg->workers * sizeof *tid);
    for (int i = 0; i < cfg->workers; ++i) {
        w[i].ops = ops;
        w[i].s = &s;
        w[i].cfg = cfg;
        w[i].obs = &obs;
        w[i].cliente = i;
        hilo_init(&w[i].h, i + 1, cfg->semilla + 7919u * (unsigned)i, cfg->pausa_us, cfg->registrar);
        w[i].esperas_ms = malloc((size_t)cfg->ops * sizeof(double));
    }

    pthread_t tc;
    cancelador_t ctx_c = {ops, &s, &obs, cfg->workers, cfg->semilla ^ 0xBEEFu};
    bool con_cancelador = cfg->cancelador && ops->cancelar_solicitud != NULL;

    int64_t inicio = ahora_ns();
    for (int i = 0; i < cfg->workers; ++i)
        CHECK_PT(pthread_create(&tid[i], NULL, trabajador, &w[i]));
    if (con_cancelador) CHECK_PT(pthread_create(&tc, NULL, cancelador, &ctx_c));
    for (int i = 0; i < cfg->workers; ++i) CHECK_PT(pthread_join(tid[i], NULL));
    int64_t fin = ahora_ns();
    if (con_cancelador) {
        atomic_store(&obs.terminar, true);
        CHECK_PT(pthread_join(tc, NULL));
    }

    /* ---- después del join: se juntan los resultados de cada worker ---- */
    int total_esperas = 0;
    for (int i = 0; i < cfg->workers; ++i) {
        res.consultas += w[i].consultas;
        res.reservas_ok += w[i].ok;
        res.timeouts += w[i].timeouts;
        res.canceladas += w[i].canceladas;
        res.cerradas += w[i].cerradas;
        total_esperas += w[i].n_esperas;
    }
    res.operaciones = (long)cfg->workers * cfg->ops;
    double *todas = malloc((size_t)(total_esperas ? total_esperas : 1) * sizeof(double));
    int k = 0;
    double suma = 0;
    for (int i = 0; i < cfg->workers; ++i)
        for (int j = 0; j < w[i].n_esperas; ++j) {
            todas[k++] = w[i].esperas_ms[j];
            suma += w[i].esperas_ms[j];
        }
    if (k > 0) {
        qsort(todas, (size_t)k, sizeof(double), comparar_double);
        res.espera_prom_ms = suma / k;
        res.espera_p95_ms = todas[(int)(0.95 * (k - 1))];
        res.espera_max_ms = todas[k - 1];
    }
    free(todas);

    res.tiempo_ms = (double)(fin - inicio) / 1e6;
    res.throughput = res.operaciones / (res.tiempo_ms / 1000.0);
    res.esperas = s.esperas;
    res.dobles_reservas = atomic_load(&obs.dobles);
    res.cancelaciones_fallidas = atomic_load(&obs.canc_fallidas);
    res.ocupacion_max = atomic_load(&obs.en_uso_max);
    res.libres_final = s.libres;
    res.violaciones_en_lock = s.violaciones;

    /* Al final ya nadie tiene recursos: deben estar todos libres. */
    char msg[300];
    res.errores_finales = servicio_verificar(&s, msg, sizeof msg);
    size_t u = 0;
    if (res.dobles_reservas > 0)
        u += (size_t)snprintf(res.detalle + u, sizeof res.detalle - u,
                              "DOBLE RESERVA x%ld; ", res.dobles_reservas);
    if (res.cancelaciones_fallidas > 0 && u < sizeof res.detalle)
        u += (size_t)snprintf(res.detalle + u, sizeof res.detalle - u,
                              "cancelaciones fallidas x%ld; ", res.cancelaciones_fallidas);
    if (res.libres_final != cfg->M && u < sizeof res.detalle)
        u += (size_t)snprintf(res.detalle + u, sizeof res.detalle - u,
                              "CONTADOR libres=%d al final (deberia ser %d); ",
                              res.libres_final, cfg->M);
    if (res.errores_finales > 0 && u < sizeof res.detalle)
        u += (size_t)snprintf(res.detalle + u, sizeof res.detalle - u, "%s", msg);
    if (res.violaciones_en_lock > 0 && u < sizeof res.detalle)
        snprintf(res.detalle + u, sizeof res.detalle - u, "violaciones en transiciones=%ld (%s)",
                 res.violaciones_en_lock, s.ultima_violacion);

    res.ok = res.dobles_reservas == 0 && res.cancelaciones_fallidas == 0 &&
             res.libres_final == cfg->M && res.errores_finales == 0 &&
             res.violaciones_en_lock == 0;

    if (cfg->registrar) {
        hilo_t *hs = malloc((size_t)cfg->workers * sizeof *hs);
        for (int i = 0; i < cfg->workers; ++i) hs[i] = w[i].h;
        res.n_traza = unir_trazas(hs, cfg->workers, &res.traza);
        free(hs);
    }

    for (int i = 0; i < cfg->workers; ++i) {
        hilo_destruir(&w[i].h);
        free(w[i].esperas_ms);
    }
    free(w);
    free(tid);
    free(obs.ocupantes);
    servicio_destruir(&s);
    return res;
}

void liberar_resultado(resultado_carga_t *r) {
    free(r->traza);
    r->traza = NULL;
    r->n_traza = 0;
}

int buscar_doble_reserva(const evento_t *t, int n, evento_t *salida, int max) {
    /* dueño actual de cada recurso según la traza, y dónde lo vio libre */
    enum { MAXR = 64, MAXH = 128 };
    int duenio[MAXR], idx_ve[MAXR];
    int ultimo_ve[MAXH][MAXR];
    for (int r = 0; r < MAXR; ++r) { duenio[r] = -1; idx_ve[r] = -1; }
    for (int h = 0; h < MAXH; ++h)
        for (int r = 0; r < MAXR; ++r) ultimo_ve[h][r] = -1;

    for (int k = 0; k < n; ++k) {
        const evento_t *e = &t[k];
        if (e->recurso < 0 || e->recurso >= MAXR || e->hilo >= MAXH) continue;
        int r = e->recurso;
        if (e->tipo == EV_VERIFICA) {
            ultimo_ve[e->hilo][r] = k;
        } else if (e->tipo == EV_OBTIENE) {
            if (duenio[r] != -1 && duenio[r] != e->hilo) {
                /* r ya tenía dueño y otro thread también lo obtuvo */
                int a = duenio[r], b = e->hilo, desde = idx_ve[r];
                if (ultimo_ve[b][r] >= 0 && ultimo_ve[b][r] < desde) desde = ultimo_ve[b][r];
                if (desde < 0) desde = 0;
                int c = 0;
                for (int j = desde; j <= k && c < max; ++j)
                    if (t[j].recurso == r && (t[j].hilo == a || t[j].hilo == b))
                        salida[c++] = t[j];
                return c;
            }
            duenio[r] = e->hilo;
            idx_ve[r] = ultimo_ve[e->hilo][r];
        } else if (e->tipo == EV_LIBERA) {
            if (duenio[r] == e->hilo) duenio[r] = -1;
        }
    }
    return 0;
}
