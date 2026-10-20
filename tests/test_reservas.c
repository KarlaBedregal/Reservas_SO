/* test_reservas.c - Pruebas automatizadas (entregable 4).
 * Termina con código 1 si alguna prueba falla.
 *
 *   ./bin/test_reservas                      -> versión correcta (todas pasan)
 *   MODO=defectuoso ./bin/test_reservas      -> pruebas de carga contra la
 *                                               versión sin sincronización (fallan)
 */
#include "../src/carga.h"

static int pasadas = 0, fallidas = 0;

static void comprobar(bool ok, const char *nombre, const char *detalle) {
    if (ok) {
        ++pasadas;
        printf("  [OK]    %s\n", nombre);
    } else {
        ++fallidas;
        printf("  [FALLA] %s\n          %s\n", nombre, detalle ? detalle : "");
    }
}

static double ms_desde(int64_t t0) { return (double)(ahora_ns() - t0) / 1e6; }

/* ---------------------------------------------------------- pruebas de carga */
static void prueba_carga(const ops_t *ops, const char *nombre, config_carga_t cfg, int reps) {
    char detalle[700];
    for (int rep = 0; rep < reps; ++rep) {
        cfg.semilla = 500u + 100u * (unsigned)rep;
        resultado_carga_t r = ejecutar_carga(ops, &cfg);
        bool ok = r.ok;
        if (!ok) snprintf(detalle, sizeof detalle, "repeticion %d -> %s", rep, r.detalle);
        liberar_resultado(&r);
        if (!ok) {
            comprobar(false, nombre, detalle);
            return;
        }
    }
    comprobar(true, nombre, NULL);
}

/* ------------------------------------------------- pruebas de una operación */
typedef struct {
    servicio_t *s;
    int cliente, timeout_ms;
    resultado_t res;
    int recurso;
    double ms;
} espera_t;

static void *esperar_reserva(void *arg) {
    espera_t *e = arg;
    int64_t t0 = ahora_ns();
    e->res = servicio_reservar(e->s, NULL, e->cliente, e->timeout_ms, &e->recurso);
    e->ms = ms_desde(t0);
    return NULL;
}

static bool esperar_hasta_bloqueado(servicio_t *s, int cliente) {
    for (int i = 0; i < 2000; ++i) {
        if (servicio_esta_esperando(s, cliente)) return true;
        dormir_us(1000);
    }
    return false;
}

static void prueba_timeout(void) {
    servicio_t s;
    CHECK_PT(servicio_crear(&s, 1, 2));
    int r;
    servicio_reservar(&s, NULL, 0, 0, &r);               /* cliente 0 ocupa R0 */
    espera_t e = {&s, 1, 100, RES_INVALIDO, -1, 0};
    esperar_reserva(&e);
    char d[160];
    snprintf(d, sizeof d, "resultado %s en %.1f ms (se esperaba TIMEOUT cerca de 100 ms)",
             nombre_resultado(e.res), e.ms);
    comprobar(e.res == RES_TIMEOUT && e.ms >= 95 && e.ms < 400 && s.libres == 0,
              "timeout: sin recursos, reservar(100 ms) devuelve TIMEOUT", d);
    servicio_cancelar(&s, NULL, 0, r);
    servicio_destruir(&s);
}

static void prueba_cancelar_despierta(void) {
    servicio_t s;
    CHECK_PT(servicio_crear(&s, 1, 2));
    int r;
    servicio_reservar(&s, NULL, 0, 0, &r);
    espera_t e = {&s, 1, 3000, RES_INVALIDO, -1, 0};
    pthread_t t;
    CHECK_PT(pthread_create(&t, NULL, esperar_reserva, &e));
    esperar_hasta_bloqueado(&s, 1);
    int64_t t0 = ahora_ns();
    resultado_t c = servicio_cancelar(&s, NULL, 0, r);
    CHECK_PT(pthread_join(t, NULL));
    double despertar = ms_desde(t0);
    char d[160];
    snprintf(d, sizeof d, "cancelar=%s, espera=%s recurso=R%d, despertar=%.1f ms",
             nombre_resultado(c), nombre_resultado(e.res), e.recurso, despertar);
    comprobar(c == RES_OK && e.res == RES_OK && e.recurso == 0 && despertar < 200,
              "cancelar una reserva libera el recurso y despierta al que espera", d);
    servicio_cancelar(&s, NULL, 1, e.recurso);
    servicio_destruir(&s);
}

static void prueba_cancelar_solicitud(void) {
    servicio_t s;
    CHECK_PT(servicio_crear(&s, 1, 3));
    int r;
    servicio_reservar(&s, NULL, 0, 0, &r);
    espera_t e = {&s, 1, 5000, RES_INVALIDO, -1, 0};
    pthread_t t;
    CHECK_PT(pthread_create(&t, NULL, esperar_reserva, &e));
    esperar_hasta_bloqueado(&s, 1);
    bool hubo = servicio_cancelar_solicitud(&s, 1);
    CHECK_PT(pthread_join(t, NULL));
    bool sin_espera = servicio_cancelar_solicitud(&s, 2);   /* el cliente 2 no espera */
    char d[180];
    snprintf(d, sizeof d, "encontro espera=%d, resultado=%s en %.1f ms, cancelar a quien no espera=%d",
             hubo, nombre_resultado(e.res), e.ms, sin_espera);
    comprobar(hubo && e.res == RES_CANCELADA && e.ms < 1000 && !sin_espera && s.libres == 0,
              "cancelar una solicitud en espera la saca con CANCELADA", d);
    servicio_cancelar(&s, NULL, 0, r);
    servicio_destruir(&s);
}

static void prueba_cancelacion_invalida(void) {
    servicio_t s;
    CHECK_PT(servicio_crear(&s, 2, 3));
    int r;
    servicio_reservar(&s, NULL, 0, 0, &r);
    resultado_t ajeno = servicio_cancelar(&s, NULL, 1, r);     /* no es el dueño */
    resultado_t rango = servicio_cancelar(&s, NULL, 0, 99);    /* recurso inexistente */
    resultado_t bien = servicio_cancelar(&s, NULL, 0, r);
    resultado_t doble = servicio_cancelar(&s, NULL, 0, r);     /* ya lo liberó */
    resultado_t cli = servicio_reservar(&s, NULL, 99, 0, &r);  /* cliente inexistente */
    char d[200];
    snprintf(d, sizeof d, "ajeno=%s rango=%s bien=%s doble=%s cliente=%s libres=%d",
             nombre_resultado(ajeno), nombre_resultado(rango), nombre_resultado(bien),
             nombre_resultado(doble), nombre_resultado(cli), s.libres);
    comprobar(ajeno == RES_NO_DUENO && rango == RES_INVALIDO && bien == RES_OK &&
                  doble == RES_NO_DUENO && cli == RES_INVALIDO && s.libres == 2,
              "errores: cancelar ajeno, doble o fuera de rango no cambia el estado", d);
    servicio_destruir(&s);
}

static void prueba_cierre(void) {
    enum { N = 8 };
    servicio_t s;
    CHECK_PT(servicio_crear(&s, 1, N + 1));
    int r;
    servicio_reservar(&s, NULL, N, 0, &r);               /* main ocupa el único recurso */
    espera_t e[N];
    pthread_t t[N];
    for (int i = 0; i < N; ++i) {
        e[i] = (espera_t){&s, i, 10000, RES_INVALIDO, -1, 0};
        CHECK_PT(pthread_create(&t[i], NULL, esperar_reserva, &e[i]));
    }
    for (int i = 0; i < N; ++i) esperar_hasta_bloqueado(&s, i);
    int64_t t0 = ahora_ns();
    servicio_cerrar(&s);
    for (int i = 0; i < N; ++i) CHECK_PT(pthread_join(t[i], NULL));
    double ms = ms_desde(t0);
    int cerrados = 0;
    for (int i = 0; i < N; ++i) cerrados += e[i].res == RES_CERRADO;
    int r2;
    resultado_t despues = servicio_reservar(&s, NULL, 0, 1000, &r2);
    resultado_t libera = servicio_cancelar(&s, NULL, N, r);
    char d[200];
    snprintf(d, sizeof d, "%d/%d con CERRADO, join en %.1f ms, reservar despues=%s, liberar=%s",
             cerrados, N, ms, nombre_resultado(despues), nombre_resultado(libera));
    comprobar(cerrados == N && ms < 1000 && despues == RES_CERRADO && libera == RES_OK &&
                  s.libres == 1,
              "cierre: 8 workers esperando salen con CERRADO y todos terminan", d);
    servicio_destruir(&s);
}

/* --------------------------------------------- el verificador sí detecta */
static void pruebas_verificador(void) {
    servicio_t s;
    char msg[200];
    CHECK_PT(servicio_crear(&s, 3, 4));
    s.libres = 2;                                        /* contador desfasado */
    comprobar(servicio_verificar(&s, msg, sizeof msg) > 0 && strstr(msg, "I1"),
              "verificador detecta contador inconsistente (I1)", msg);
    s.libres = 3;
    s.dueno[0] = 7;                                      /* dueño inexistente */
    s.libres = 2;
    s.reservas = 1;
    comprobar(servicio_verificar(&s, msg, sizeof msg) > 0 && strstr(msg, "I3"),
              "verificador detecta dueno invalido (I3)", msg);
    s.dueno[0] = -1;
    s.libres = 4;
    s.reservas = 0;
    comprobar(servicio_verificar(&s, msg, sizeof msg) > 0 && strstr(msg, "I2"),
              "verificador detecta contador fuera de rango (I2)", msg);
    servicio_destruir(&s);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *env = getenv("MODO");
    bool seguro = !(env && strcmp(env, "defectuoso") == 0);
    const ops_t *ops = seguro ? &OPS_SEGURO : &OPS_DEFECTUOSO;
    printf("Pruebas del servicio de reservas (MODO=%s)\n", seguro ? "seguro" : "defectuoso");

    config_carga_t c = config_por_defecto();
    c.workers = 16; c.M = 3; c.ops = 100; c.timeout_ms = 30; c.uso_us = 200; c.pausa_us = 50;
    prueba_carga(ops, "sin doble reserva: 16 workers, M=3, 10 corridas", c, 10);

    c.workers = 8; c.M = 1; c.ops = 60; c.pausa_us = 100;
    prueba_carga(ops, "contador consistente: 8 workers, M=1, 10 corridas", c, 10);

    c.workers = 12; c.M = 10; c.ops = 100; c.pausa_us = 20;
    prueba_carga(ops, "mucha capacidad: 12 workers, M=10, 10 corridas", c, 10);

    if (seguro) {
        c.workers = 8; c.M = 2; c.ops = 100; c.timeout_ms = 15; c.pausa_us = 0;
        c.cancelador = true;
        prueba_carga(ops, "carga con cancelaciones al azar: 8 workers + cancelador", c, 10);

        prueba_timeout();
        prueba_cancelar_despierta();
        prueba_cancelar_solicitud();
        prueba_cancelacion_invalida();
        prueba_cierre();
    }
    pruebas_verificador();

    printf("\nResultado: %d pasadas, %d fallidas\n", pasadas, fallidas);
    return fallidas ? 1 : 0;
}
