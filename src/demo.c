/* demo.c - Traza mínima del servicio correcto con 8 workers.
 *
 * Escena 1: M=2 recursos y 8 workers que piden uno a la vez. Dos lo obtienen
 *           y los demás se BLOQUEAN por falta de disponibilidad; cada vez que
 *           alguien cancela su reserva, se despierta a uno. Algunos vencen su
 *           timeout.
 * Escena 2: un worker espera y main CANCELA su solicitud.
 * Escena 3: varios workers esperan y main CIERRA el servicio: todos salen
 *           con CERRADO y el programa termina limpiamente.
 *
 * Uso: ./bin/demo
 */
#include "servicio.h"

#define N_WORKERS 8

typedef struct {
    servicio_t *s;
    hilo_t h;
    int cliente;
    int timeout_ms;
    int uso_ms;          /* cuánto usa el recurso antes de cancelar */
    resultado_t res;
    int recurso;
} tarea_t;

static void *una_reserva(void *arg) {
    tarea_t *t = arg;
    t->res = servicio_reservar(t->s, &t->h, t->cliente, t->timeout_ms, &t->recurso);
    if (t->res == RES_OK) {
        dormir_us(t->uso_ms * 1000L);
        servicio_cancelar(t->s, &t->h, t->cliente, t->recurso);
    }
    return NULL;
}

static void esperar_bloqueados(servicio_t *s, int cuantos) {
    /* Espera (con límite) a que 'cuantos' clientes estén dentro de la espera.
     * Solo sirve para armar la escena; no ordena nada dentro del servicio. */
    for (int intento = 0; intento < 2000; ++intento) {
        int n = 0;
        for (int c = 0; c < s->max_clientes; ++c) n += servicio_esta_esperando(s, c);
        if (n >= cuantos) return;
        dormir_us(1000);
    }
}

static void imprimir_traza(tarea_t *t, int n, hilo_t *main_h) {
    hilo_t todos[N_WORKERS + 1];
    for (int i = 0; i < n; ++i) todos[i] = t[i].h;
    todos[n] = *main_h;
    evento_t *ev;
    int m = unir_trazas(todos, n + 1, &ev);
    for (int i = 0; i < m; ++i) imprimir_evento(stdout, &ev[i], ev[0].t_ns);
    free(ev);
}

static void resumen(tarea_t *t, int n) {
    printf("  Resultado por worker:");
    for (int i = 0; i < n; ++i)
        printf(" T%d=%s", t[i].h.id, nombre_resultado(t[i].res));
    printf("\n");
}

static void verificar_final(servicio_t *s, const char *escena) {
    char msg[200];
    int e = servicio_verificar(s, msg, sizeof msg);
    printf("  Invariantes al final de la %s: %s (violaciones en transiciones: %ld)\n\n",
           escena, e == 0 ? "OK" : msg, s->violaciones);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    pthread_t tid[N_WORKERS];
    tarea_t t[N_WORKERS];
    hilo_t main_h;

    /* ------------------------------------------------------------ escena 1 */
    printf("=== Escena 1: M=2 recursos, 8 workers, timeout 200 ms, uso 80 ms ===\n");
    servicio_t s1;
    CHECK_PT(servicio_crear(&s1, 2, N_WORKERS + 1));
    hilo_init(&main_h, 0, 1, 0, true);
    for (int i = 0; i < N_WORKERS; ++i) {
        t[i] = (tarea_t){&s1, {0}, i, 200, 80, RES_INVALIDO, -1};
        hilo_init(&t[i].h, i + 1, 100u + (unsigned)i, 0, true);
    }
    for (int i = 0; i < N_WORKERS; ++i) CHECK_PT(pthread_create(&tid[i], NULL, una_reserva, &t[i]));
    for (int i = 0; i < N_WORKERS; ++i) CHECK_PT(pthread_join(tid[i], NULL));
    imprimir_traza(t, N_WORKERS, &main_h);
    resumen(t, N_WORKERS);
    printf("  reservas=%ld timeouts=%ld esperas(bloqueos)=%ld ocupacion_max=%d\n",
           s1.reservas, s1.timeouts, s1.esperas, s1.ocupacion_max);
    verificar_final(&s1, "escena 1");
    for (int i = 0; i < N_WORKERS; ++i) hilo_destruir(&t[i].h);
    hilo_destruir(&main_h);
    servicio_destruir(&s1);

    /* ------------------------------------------------------------ escena 2 */
    printf("=== Escena 2: cancelacion de una solicitud en espera (M=1) ===\n");
    servicio_t s2;
    CHECK_PT(servicio_crear(&s2, 1, N_WORKERS + 1));
    hilo_init(&main_h, 0, 1, 0, true);
    int r_main;
    servicio_reservar(&s2, &main_h, N_WORKERS, 0, &r_main);          /* main ocupa R0 */
    t[0] = (tarea_t){&s2, {0}, 0, 5000, 0, RES_INVALIDO, -1};
    hilo_init(&t[0].h, 1, 7, 0, true);
    CHECK_PT(pthread_create(&tid[0], NULL, una_reserva, &t[0]));
    esperar_bloqueados(&s2, 1);
    hilo_log(&main_h, EV_PIDE_CANCELAR, -1, servicio_consultar(&s2, NULL));
    bool hubo = servicio_cancelar_solicitud(&s2, 0);
    CHECK_PT(pthread_join(tid[0], NULL));
    servicio_cancelar(&s2, &main_h, N_WORKERS, r_main);
    imprimir_traza(t, 1, &main_h);
    printf("  cancelar_solicitud(T1) encontro una espera: %s; T1 termino con %s\n",
           hubo ? "si" : "no", nombre_resultado(t[0].res));
    verificar_final(&s2, "escena 2");
    hilo_destruir(&t[0].h);
    hilo_destruir(&main_h);
    servicio_destruir(&s2);

    /* ------------------------------------------------------------ escena 3 */
    printf("=== Escena 3: cierre limpio con 8 workers esperando (M=1, timeout 10 s) ===\n");
    servicio_t s3;
    CHECK_PT(servicio_crear(&s3, 1, N_WORKERS + 1));
    hilo_init(&main_h, 0, 1, 0, true);
    servicio_reservar(&s3, &main_h, N_WORKERS, 0, &r_main);          /* main ocupa R0 */
    for (int i = 0; i < N_WORKERS; ++i) {
        t[i] = (tarea_t){&s3, {0}, i, 10000, 0, RES_INVALIDO, -1};
        hilo_init(&t[i].h, i + 1, 300u + (unsigned)i, 0, true);
    }
    for (int i = 0; i < N_WORKERS; ++i) CHECK_PT(pthread_create(&tid[i], NULL, una_reserva, &t[i]));
    esperar_bloqueados(&s3, N_WORKERS);
    int64_t t0 = ahora_ns();
    hilo_log(&main_h, EV_CIERRE, -1, servicio_consultar(&s3, NULL));
    servicio_cerrar(&s3);
    for (int i = 0; i < N_WORKERS; ++i) CHECK_PT(pthread_join(tid[i], NULL));
    double ms = (double)(ahora_ns() - t0) / 1e6;
    servicio_cancelar(&s3, &main_h, N_WORKERS, r_main);              /* main libera */
    int r_tarde;
    resultado_t tarde = servicio_reservar(&s3, &main_h, N_WORKERS, 1000, &r_tarde);
    imprimir_traza(t, N_WORKERS, &main_h);
    resumen(t, N_WORKERS);
    printf("  Los 8 threads terminaron %.2f ms despues del cierre.\n", ms);
    printf("  Reserva despues del cierre: %s\n", nombre_resultado(tarde));
    verificar_final(&s3, "escena 3");
    for (int i = 0; i < N_WORKERS; ++i) hilo_destruir(&t[i].h);
    hilo_destruir(&main_h);
    servicio_destruir(&s3);

    printf("Fin: mutex y condiciones destruidos, memoria liberada.\n");
    return 0;
}
