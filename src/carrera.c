/* carrera.c - Reproduce la condición de carrera (entregable 5).
 *
 * Ejecuta la MISMA carga (mismos workers, operaciones, pausas y semillas)
 * con la versión defectuosa y con la versión correcta, la repite muchas
 * veces y cuenta en cuántas corridas se rompió algo. De la primera corrida
 * defectuosa con doble reserva guarda la traza que lo demuestra.
 *
 * Uso: ./bin/carrera                       (8 workers, M=3, 100 repeticiones)
 *      ./bin/carrera --hilos 16 --recursos 1
 *      ./bin/carrera --modo defectuoso --reps 1   (lo usa make tsan)
 */
#include <sys/stat.h>

#include "carga.h"

typedef struct {
    int corridas_con_falla, con_doble, con_contador, con_cancel, con_invariante;
    long total_dobles;
    double tiempo_total;
} resumen_t;

static void uso(const char *p) {
    fprintf(stderr, "Uso: %s [--hilos N>=1] [--recursos M] [--reps N] [--ops N] "
                    "[--pausa US] [--modo seguro|defectuoso|ambos]\n", p);
    exit(2);
}

static resumen_t correr(const ops_t *ops, config_carga_t cfg, int reps, const char *csv_nombre,
                        bool guardar_traza) {
    resumen_t r = {0};
    char ruta[128];
    snprintf(ruta, sizeof ruta, "resultados/%s.csv", csv_nombre);
    FILE *csv = fopen(ruta, "w");
    if (csv) fprintf(csv, "rep,reservas_ok,timeouts,dobles_reservas,cancelaciones_fallidas,"
                          "libres_final,M,violaciones,estado,detalle\n");

    printf("=== %s: %d workers, M=%d, %d ops c/u, %d repeticiones, pausa max %d us ===\n",
           ops->nombre, cfg.workers, cfg.M, cfg.ops, reps, cfg.pausa_us);
    bool traza_guardada = false;
    unsigned base = cfg.semilla;
    for (int rep = 1; rep <= reps; ++rep) {
        cfg.semilla = base + 1000u * (unsigned)rep;
        cfg.registrar = guardar_traza && !traza_guardada;
        resultado_carga_t res = ejecutar_carga(ops, &cfg);
        r.tiempo_total += res.tiempo_ms;
        if (!res.ok) {
            r.corridas_con_falla++;
            if (res.dobles_reservas > 0) r.con_doble++;
            if (res.libres_final != cfg.M) r.con_contador++;
            if (res.cancelaciones_fallidas > 0) r.con_cancel++;
            if (res.errores_finales > 0 || res.violaciones_en_lock > 0) r.con_invariante++;
            r.total_dobles += res.dobles_reservas;
        }
        if (rep <= 3 || rep > reps - 2 || (!res.ok && r.corridas_con_falla <= 2))
            printf("rep %3d: ok=%3ld timeouts=%3ld dobles=%2ld canc_fallidas=%2ld libres_final=%2d/%d -> %s %s\n",
                   rep, res.reservas_ok, res.timeouts, res.dobles_reservas,
                   res.cancelaciones_fallidas, res.libres_final, cfg.M,
                   res.ok ? "OK" : "FALLA", res.detalle);
        else if (rep == 4)
            printf("...\n");
        if (csv)
            fprintf(csv, "%d,%ld,%ld,%ld,%ld,%d,%d,%ld,%s,\"%s\"\n", rep, res.reservas_ok,
                    res.timeouts, res.dobles_reservas, res.cancelaciones_fallidas,
                    res.libres_final, cfg.M, res.violaciones_en_lock, res.ok ? "OK" : "FALLA",
                    res.detalle);

        if (cfg.registrar && res.dobles_reservas > 0 && res.n_traza > 0) {
            evento_t ev[64];
            int n = buscar_doble_reserva(res.traza, res.n_traza, ev, 64);
            if (n > 0) {
                traza_guardada = true;
                snprintf(ruta, sizeof ruta, "resultados/%s_traza.txt", csv_nombre);
                FILE *f = fopen(ruta, "w");
                printf("\nDoble reserva encontrada en la repeticion %d (eventos sobre R%d):\n",
                       rep, ev[0].recurso);
                for (int i = 0; i < n; ++i) imprimir_evento(stdout, &ev[i], ev[0].t_ns);
                if (f) {
                    fprintf(f, "Doble reserva en la repeticion %d\n", rep);
                    for (int i = 0; i < n; ++i) imprimir_evento(f, &ev[i], ev[0].t_ns);
                    fprintf(f, "\nTraza completa de esa corrida:\n");
                    for (int i = 0; i < res.n_traza; ++i)
                        imprimir_evento(f, &res.traza[i], res.traza[0].t_ns);
                    fclose(f);
                    printf("Traza completa guardada en: %s\n\n", ruta);
                }
            }
        }
        liberar_resultado(&res);
    }
    if (csv) fclose(csv);

    printf("--- Resumen (%s) ---\n", ops->nombre);
    printf("Corridas con alguna falla: %d/%d\n", r.corridas_con_falla, reps);
    printf("  doble reserva (dos threads con el mismo recurso): %d corridas (%ld casos)\n",
           r.con_doble, r.total_dobles);
    printf("  contador libres inconsistente al final: %d corridas\n", r.con_contador);
    printf("  cancelacion de un recurso que ya no era suyo: %d corridas\n", r.con_cancel);
    printf("  invariantes I1-I4 rotos: %d corridas\n", r.con_invariante);
    printf("Tiempo promedio por corrida: %.1f ms\n", r.tiempo_total / reps);
    printf("CSV guardado en: resultados/%s.csv\n\n", csv_nombre);
    return r;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    config_carga_t cfg = config_por_defecto();
    cfg.workers = 8;
    cfg.M = 3;
    cfg.ops = 30;
    cfg.timeout_ms = 50;
    cfg.uso_us = 200;
    cfg.pausa_us = 200;
    int reps = 100;
    const char *modo = "ambos";
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) uso(argv[0]);
        if (!strcmp(argv[i], "--hilos")) cfg.workers = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--recursos")) cfg.M = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--reps")) reps = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--ops")) cfg.ops = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--pausa")) cfg.pausa_us = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--modo")) modo = argv[i + 1];
        else uso(argv[0]);
    }
    if (cfg.workers < 1 || cfg.M < 1 || reps < 1 || cfg.ops < 1 || cfg.pausa_us < 0) uso(argv[0]);
    mkdir("resultados", 0755);

    int fallas_seguro = 0;
    if (!strcmp(modo, "defectuoso") || !strcmp(modo, "ambos"))
        correr(&OPS_DEFECTUOSO, cfg, reps, "carrera_defectuoso", true);
    if (!strcmp(modo, "seguro") || !strcmp(modo, "ambos"))
        fallas_seguro = correr(&OPS_SEGURO, cfg, reps, "carrera_seguro", false).corridas_con_falla;
    return fallas_seguro ? 1 : 0;
}
