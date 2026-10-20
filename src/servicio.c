/* servicio.c
 * Versión correcta del servicio de reservas.
 *
 *  - pthread_mutex_t m      protege JUNTOS dueno[], libres, cerrado y las
 *                           solicitudes, porque todos aparecen en los mismos
 *                           invariantes.
 *  - pthread_cond_t cambio  permite esperar disponibilidad SIN espera activa,
 *                           con timeout (pthread_cond_timedwait) y revisando
 *                           siempre el predicado en un while.
 */
#include "servicio.h"

#include <errno.h>

const char *nombre_resultado(resultado_t r) {
    switch (r) {
    case RES_OK:        return "OK";
    case RES_TIMEOUT:   return "TIMEOUT";
    case RES_CANCELADA: return "CANCELADA";
    case RES_CERRADO:   return "CERRADO";
    case RES_INVALIDO:  return "INVALIDO";
    case RES_NO_DUENO:  return "NO_DUENO";
    }
    return "?";
}

int servicio_crear(servicio_t *s, int M, int max_clientes) {
    if (M <= 0 || max_clientes <= 0) return EINVAL;
    memset(s, 0, sizeof *s);
    s->M = M;
    s->libres = M;
    s->max_clientes = max_clientes;
    s->dueno = malloc((size_t)M * sizeof *s->dueno);
    s->esperando = calloc((size_t)max_clientes, sizeof *s->esperando);
    s->cancelada = calloc((size_t)max_clientes, sizeof *s->cancelada);
    if (!s->dueno || !s->esperando || !s->cancelada) {
        free(s->dueno); free(s->esperando); free(s->cancelada);
        return ENOMEM;
    }
    for (int i = 0; i < M; ++i) s->dueno[i] = -1;

    int rc = pthread_mutex_init(&s->m, NULL);
    if (rc != 0) return rc;
    /* La condición usa el reloj MONOTÓNICO: si alguien cambia la hora del
     * sistema, los timeouts no se adelantan ni se atrasan. */
    pthread_condattr_t attr;
    CHECK_PT(pthread_condattr_init(&attr));
    CHECK_PT(pthread_condattr_setclock(&attr, CLOCK_MONOTONIC));
    rc = pthread_cond_init(&s->cambio, &attr);
    pthread_condattr_destroy(&attr);
    return rc;
}

void servicio_destruir(servicio_t *s) {
    /* Solo se llama cuando ningún thread usa el servicio (después del join). */
    CHECK_PT(pthread_cond_destroy(&s->cambio));
    CHECK_PT(pthread_mutex_destroy(&s->m));
    free(s->dueno);
    free(s->esperando);
    free(s->cancelada);
    s->dueno = NULL;
    s->esperando = s->cancelada = NULL;
}

/* ------------------------------------------------------------ invariantes */
int servicio_verificar(const servicio_t *s, char *msg, size_t n) {
    int errores = 0, libres_reales = 0;
    size_t usado = 0;
    if (msg && n) msg[0] = '\0';
#define AGREGAR(...)                                                          \
    do {                                                                      \
        ++errores;                                                            \
        if (msg && usado < n)                                                 \
            usado += (size_t)snprintf(msg + usado, n - usado, __VA_ARGS__);   \
    } while (0)

    for (int i = 0; i < s->M; ++i) {
        int d = s->dueno[i];
        if (d == -1) ++libres_reales;
        else if (d < 0 || d >= s->max_clientes)
            AGREGAR("I3 R%d tiene un dueno invalido (%d); ", i, d);
    }
    if (s->libres != libres_reales)
        AGREGAR("I1 libres=%d pero hay %d recursos libres; ", s->libres, libres_reales);
    if (s->libres < 0 || s->libres > s->M)
        AGREGAR("I2 libres=%d fuera de [0,%d]; ", s->libres, s->M);
    if (s->reservas - s->liberaciones != (long)(s->M - s->libres))
        AGREGAR("I4 reservas-liberaciones=%ld pero ocupados=%d; ",
                s->reservas - s->liberaciones, s->M - s->libres);
#undef AGREGAR
    return errores;
}

/* Se llama al final de cada transición, con el mutex tomado. */
static void verificar_en_lock(servicio_t *s) {
    char msg[160];
    if (servicio_verificar(s, msg, sizeof msg) > 0) {
        s->violaciones++;
        snprintf(s->ultima_violacion, sizeof s->ultima_violacion, "%s", msg);
    }
}

static int buscar_libre(const servicio_t *s) {
    for (int i = 0; i < s->M; ++i)
        if (s->dueno[i] == -1) return i;
    return -1;
}

/* ---------------------------------------------------------------- reservar */
resultado_t servicio_reservar(servicio_t *s, hilo_t *h, int cliente, int timeout_ms, int *recurso) {
    if (recurso) *recurso = -1;
    if (cliente < 0 || cliente >= s->max_clientes || timeout_ms < 0) return RES_INVALIDO;

    struct timespec limite;
    limite_desde_ahora(&limite, timeout_ms);

    CHECK_PT(pthread_mutex_lock(&s->m));                 /* ---- entrada ---- */
    hilo_log(h, EV_SOLICITA, -1, s->libres);
    s->esperando[cliente] = true;

    /* Espera por el PREDICADO, no por quién despierta primero.
     * pthread_cond_timedwait libera m mientras duerme y lo vuelve a tomar
     * antes de retornar, así el predicado siempre se revisa con m tomado. */
    bool espero = false;
    while (!s->cerrado && !s->cancelada[cliente] && s->libres == 0) {
        if (!espero) {
            espero = true;
            s->esperas++;
            hilo_log(h, EV_ESPERA, -1, s->libres);
        }
        int rc = pthread_cond_timedwait(&s->cambio, &s->m, &limite);
        if (rc == ETIMEDOUT) break;
        if (rc != 0) CHECK_PT(rc);
    }
    s->esperando[cliente] = false;
    bool fue_cancelada = s->cancelada[cliente];
    s->cancelada[cliente] = false;

    resultado_t res;
    if (s->cerrado) {                                    /* I5 */
        res = RES_CERRADO;
        s->rechazos_cierre++;
        hilo_log(h, EV_CERRADO, -1, s->libres);
    } else if (fue_cancelada) {
        res = RES_CANCELADA;
        s->canceladas++;
        hilo_log(h, EV_CANCELADA, -1, s->libres);
    } else if (s->libres > 0) {
        /* ---- sección crítica: verificar y actuar en la MISMA región ---- */
        int r = buscar_libre(s);
        hilo_log(h, EV_VERIFICA, r, s->libres);
        hilo_pausa(h);                                   /* misma pausa que la versión defectuosa */
        s->dueno[r] = cliente;
        int t = s->libres;
        hilo_pausa(h);
        s->libres = t - 1;
        s->reservas++;
        if (s->M - s->libres > s->ocupacion_max) s->ocupacion_max = s->M - s->libres;
        if (recurso) *recurso = r;
        res = RES_OK;
        hilo_log(h, EV_OBTIENE, r, s->libres);
    } else {
        res = RES_TIMEOUT;
        s->timeouts++;
        hilo_log(h, EV_TIMEOUT, -1, s->libres);
    }

    /* Si este thread se va sin usar un aviso de "hay recurso libre"
     * (por timeout o cancelación), lo pasa a otro para no perderlo. */
    if (res != RES_OK && s->libres > 0) CHECK_PT(pthread_cond_signal(&s->cambio));

    verificar_en_lock(s);
    CHECK_PT(pthread_mutex_unlock(&s->m));               /* ---- salida ---- */
    return res;
}

/* ---------------------------------------------------------------- cancelar */
resultado_t servicio_cancelar(servicio_t *s, hilo_t *h, int cliente, int recurso) {
    if (recurso < 0 || recurso >= s->M) return RES_INVALIDO;

    CHECK_PT(pthread_mutex_lock(&s->m));
    if (s->dueno[recurso] != cliente) {                  /* solo el dueño libera */
        CHECK_PT(pthread_mutex_unlock(&s->m));
        return RES_NO_DUENO;
    }
    hilo_pausa(h);
    s->dueno[recurso] = -1;
    int t = s->libres;
    hilo_pausa(h);
    s->libres = t + 1;
    s->liberaciones++;
    hilo_log(h, EV_LIBERA, recurso, s->libres);
    /* Se liberó UNA unidad: basta despertar a UN interesado. */
    CHECK_PT(pthread_cond_signal(&s->cambio));
    verificar_en_lock(s);
    CHECK_PT(pthread_mutex_unlock(&s->m));
    return RES_OK;
}

int servicio_consultar(servicio_t *s, hilo_t *h) {
    CHECK_PT(pthread_mutex_lock(&s->m));
    int v = s->libres;
    hilo_log(h, EV_CONSULTA, -1, v);
    CHECK_PT(pthread_mutex_unlock(&s->m));
    return v;
}

bool servicio_cancelar_solicitud(servicio_t *s, int cliente) {
    if (cliente < 0 || cliente >= s->max_clientes) return false;
    CHECK_PT(pthread_mutex_lock(&s->m));
    bool hay = s->esperando[cliente];
    if (hay) {
        s->cancelada[cliente] = true;
        /* Todos duermen en la misma condición: se despierta a todos y
         * cada uno revisa su propio predicado. */
        CHECK_PT(pthread_cond_broadcast(&s->cambio));
    }
    CHECK_PT(pthread_mutex_unlock(&s->m));
    return hay;
}

bool servicio_esta_esperando(servicio_t *s, int cliente) {
    CHECK_PT(pthread_mutex_lock(&s->m));
    bool v = s->esperando[cliente];
    CHECK_PT(pthread_mutex_unlock(&s->m));
    return v;
}

void servicio_cerrar(servicio_t *s) {
    CHECK_PT(pthread_mutex_lock(&s->m));
    s->cerrado = true;
    CHECK_PT(pthread_cond_broadcast(&s->cambio));        /* nadie se queda esperando */
    CHECK_PT(pthread_mutex_unlock(&s->m));
}

const ops_t OPS_SEGURO = {
    "seguro (mutex + condicion)",
    servicio_reservar, servicio_cancelar, servicio_consultar, servicio_cancelar_solicitud,
};
